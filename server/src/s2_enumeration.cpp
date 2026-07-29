#include "s2_enumeration.hpp"

#include "app_state.hpp"
#include "gadget_wakeup.hpp"

#include <atomic>
#include <mutex>
#include <print>
#include <string>

namespace {

std::atomic<S2EnumerationState> g_state{S2EnumerationState::WaitingForHost};
std::atomic<unsigned> g_failures{0};
std::atomic<bool> g_recovery_requested{false};
std::atomic<bool> g_recovery_running{false};
std::atomic<bool> g_automatic_recovery_enabled{true};
std::atomic<bool> g_duplicate_request_logged{false};
std::atomic<bool> g_suppressed_request_logged{false};
std::mutex g_reason_mtx;
std::string g_recovery_reason;

const char* state_name(S2EnumerationState state) {
    switch (state) {
        case S2EnumerationState::WaitingForHost: return "waiting-for-host";
        case S2EnumerationState::UsbConfigured: return "usb-configured";
        case S2EnumerationState::NativeHandshake: return "native-handshake";
        case S2EnumerationState::Streaming: return "streaming";
        case S2EnumerationState::Recovering: return "recovering";
    }
    return "unknown";
}

void transition_to(S2EnumerationState next) {
    const S2EnumerationState previous = g_state.exchange(next, std::memory_order_acq_rel);
    if (previous != next && g_ctx.verbose) {
        std::println("[s2-enum] state {} -> {}", state_name(previous), state_name(next));
    }
}

} // namespace

S2EnumerationState switch2_enumeration_state() {
    return g_state.load(std::memory_order_acquire);
}

unsigned switch2_enumeration_failure_count() {
    return g_failures.load(std::memory_order_acquire);
}

void switch2_enumeration_gadget_started() {
    transition_to(S2EnumerationState::WaitingForHost);
}

void switch2_enumeration_usb_configured() {
    if (g_recovery_requested.load(std::memory_order_acquire)
            || g_recovery_running.load(std::memory_order_acquire)) return;
    transition_to(S2EnumerationState::UsbConfigured);
}

void switch2_enumeration_native_handshake() {
    if (g_recovery_requested.load(std::memory_order_acquire)
            || g_recovery_running.load(std::memory_order_acquire)) return;
    const auto state = g_state.load(std::memory_order_acquire);
    if (state == S2EnumerationState::UsbConfigured)
        transition_to(S2EnumerationState::NativeHandshake);
}

void switch2_enumeration_streaming_validated(uint8_t report_id) {
    if (g_recovery_requested.load(std::memory_order_acquire)
            || g_recovery_running.load(std::memory_order_acquire)) return;
    g_failures.store(0, std::memory_order_release);
    g_automatic_recovery_enabled.store(true, std::memory_order_release);
    g_duplicate_request_logged.store(false, std::memory_order_release);
    g_suppressed_request_logged.store(false, std::memory_order_release);
    transition_to(S2EnumerationState::Streaming);
    if (g_ctx.verbose) {
        std::println("[s2-enum] validated native streaming command; report=0x{:02x}, "
                     "consecutive failures reset to 0",
                     report_id);
    }
}

void switch2_enumeration_bus_reset() {
    if (g_recovery_requested.load(std::memory_order_acquire)
            || g_recovery_running.load(std::memory_order_acquire)) return;
    transition_to(S2EnumerationState::WaitingForHost);
}

void switch2_enumeration_client_connected() {
    // A new client session is the explicit boundary that re-enables recovery
    // after retry exhaustion. It never requests a USB reconnect by itself.
    g_failures.store(0, std::memory_order_release);
    g_automatic_recovery_enabled.store(true, std::memory_order_release);
    g_duplicate_request_logged.store(false, std::memory_order_release);
    g_suppressed_request_logged.store(false, std::memory_order_release);
}

void switch2_enumeration_client_disconnected() {
    // Failure accounting is session-specific. Do not re-enable a retry-exhausted
    // gadget until a new client session is actually allocated.
    g_failures.store(0, std::memory_order_release);
}

void request_switch2_reenumeration(std::string_view reason) {
    if (g_ctx.usb_controller_family != UsbControllerFamily::Switch2) return;
    if (!g_automatic_recovery_enabled.load(std::memory_order_acquire)) {
        if (g_ctx.verbose
                && !g_suppressed_request_logged.exchange(
                    true, std::memory_order_acq_rel)) {
            std::println("[s2-enum] recovery suppressed until a client reconnects: {}",
                         reason);
        }
        return;
    }

    if (g_recovery_running.load(std::memory_order_acquire)) {
        if (g_ctx.verbose
                && !g_duplicate_request_logged.exchange(
                    true, std::memory_order_acq_rel)) {
            std::println("[s2-enum] coalescing duplicate recovery request: {}", reason);
        }
        return;
    }

    bool expected = false;
    if (!g_recovery_requested.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        if (g_ctx.verbose
                && !g_duplicate_request_logged.exchange(
                    true, std::memory_order_acq_rel)) {
            std::println("[s2-enum] coalescing duplicate recovery request: {}", reason);
        }
        return;
    }

    g_duplicate_request_logged.store(false, std::memory_order_release);
    g_suppressed_request_logged.store(false, std::memory_order_release);
    const unsigned failures = g_failures.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lk(g_reason_mtx);
        g_recovery_reason.assign(reason);
    }

    std::println(stderr, "[s2-enum] validated enumeration failure {}/{}: {}",
                 failures, SWITCH2_MAX_ENUMERATION_RETRIES, reason);

    if (failures >= SWITCH2_MAX_ENUMERATION_RETRIES) {
        g_recovery_requested.store(false, std::memory_order_release);
        g_automatic_recovery_enabled.store(false, std::memory_order_release);
        transition_to(S2EnumerationState::WaitingForHost);
        release_switch2_active_session(
            "S2 enumeration retry limit reached", false);
        // The released session is finished; the next allocated session starts
        // with a fresh counter, while automatic recovery remains disabled until
        // that allocation occurs.
        g_failures.store(0, std::memory_order_release);
        std::println(stderr,
                     "[s2-enum] retry limit reached; active P1 session released, "
                     "waiting for client reconnect");
        return;
    }

    transition_to(S2EnumerationState::Recovering);
}

bool service_switch2_reenumeration() {
    if (!g_recovery_requested.load(std::memory_order_acquire)) return false;
    if (g_recovery_running.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    if (!g_recovery_requested.exchange(false, std::memory_order_acq_rel)) {
        g_recovery_running.store(false, std::memory_order_release);
        return false;
    }

    std::string reason;
    {
        std::lock_guard<std::mutex> lk(g_reason_mtx);
        reason = g_recovery_reason;
    }
    if (g_ctx.verbose) {
        std::println("[s2-enum] restarting Raw Gadget after failure {}/{}: {}",
                     switch2_enumeration_failure_count(),
                     SWITCH2_MAX_ENUMERATION_RETRIES, reason);
    }

    clear_switch2_usb_activity();
    const bool started = run_gadget_setup_if_needed(true, reason.c_str());
    g_recovery_running.store(false, std::memory_order_release);

    if (started) {
        switch2_enumeration_gadget_started();
    } else {
        transition_to(S2EnumerationState::WaitingForHost);
        request_switch2_reenumeration("Raw Gadget restart failed");
    }
    return true;
}
