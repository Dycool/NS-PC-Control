#include "macro_client.hpp"
#include "input_settings.hpp"
#include "stream_runtime.hpp"
#include "shared/sha256.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <cstdio>

uint32_t g_macro_udp_seq = 0;

uint32_t next_macro_upload_id() {
    uint32_t v = ++g_macro_udp_seq;
    if (v == 0) v = ++g_macro_udp_seq;
    return v;
}

bool send_udp_upload_packet(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32],
                            std::span<const uint8_t> payload, uint8_t subpad,
                            ns::macro::UploadKind kind, bool force_chunked) {
    if (payload.empty() || payload.size() > ns::macro::UDP_TEXT_MAX) return false;
    auto sign_and_send = [&](const void* hdr_ptr, size_t hdr_size, const uint8_t* data_ptr, size_t data_size) {
        std::vector<uint8_t> buf(hdr_size + data_size + ns::HMAC_TAG_SIZE);
        std::memcpy(buf.data(), hdr_ptr, hdr_size);
        if (data_size) std::memcpy(buf.data() + hdr_size, data_ptr, data_size);
        uint8_t hmac[32];
        hmac_sha256(std::span(hmac_key, 32), std::span(buf.data(), hdr_size + data_size), std::span<uint8_t, 32>(hmac));
        std::memcpy(buf.data() + hdr_size + data_size, hmac, ns::HMAC_TAG_SIZE);
        return send_all_udp(sock, dest, buf) != SOCKET_ERROR;
    };

    // Keep the old NSMC single-datagram path strictly macro-only. Typed chunked uploads
    // must use NSMK chunks so the server can read header.reserved.
    if (!force_chunked && kind == ns::macro::UploadKind::Macro &&
            payload.size() + ns::macro::UDP_HEADER_SIZE + ns::HMAC_TAG_SIZE <= 1400) {
        ns::macro::MacroUdpHeaderWire hdr{.magic = ns::macro::UDP_MAGIC, .version = ns::PROTO_VERSION,
                                          .subpad = subpad, .text_len = (uint32_t)payload.size(),
                                          .seq = next_macro_upload_id()};
        return sign_and_send(&hdr, sizeof(hdr), payload.data(), payload.size());
    }

    const uint32_t upload_id = next_macro_upload_id();
    const uint32_t chunk_count = (uint32_t)((payload.size() + ns::macro::UDP_CHUNK_MAX - 1) / ns::macro::UDP_CHUNK_MAX);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        size_t off = (size_t)i * ns::macro::UDP_CHUNK_MAX;
        size_t n = std::min(ns::macro::UDP_CHUNK_MAX, payload.size() - off);
        ns::macro::MacroUdpChunkHeaderWire hdr{
            .magic = ns::macro::UDP_CHUNK_MAGIC, .version = ns::PROTO_VERSION, .subpad = subpad,
            .flags = (uint8_t)((i + 1 == chunk_count) ? ns::macro::CHUNK_FLAG_LAST : 0x00),
            .reserved = static_cast<uint8_t>(kind),
            .upload_id = upload_id, .chunk_index = i, .chunk_count = chunk_count,
            .total_len = (uint32_t)payload.size(), .chunk_len = (uint16_t)n,
            .seq = next_macro_upload_id()
        };
        if (!sign_and_send(&hdr, sizeof(hdr), payload.data() + off, n)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool send_macro_udp_packet(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32],
                                  const std::string& json_or_commands, uint8_t subpad) {
    return send_udp_upload_packet(sock, dest, hmac_key,
                                  std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json_or_commands.data()), json_or_commands.size()),
                                  subpad, ns::macro::UploadKind::Macro, false);
}

std::mutex g_macro_mtx;
std::vector<ns::macro::Step> g_macro_steps;
std::atomic<bool> g_macro_running{false};
std::atomic<bool> g_macro_recording{false};
uint64_t g_macro_start_us = 0;
std::string g_macro_upload_pending;
std::vector<ns::macro::Entry> g_macro_entries;
std::unordered_map<std::string, bool> g_macro_hotkey_down;

int find_macro_entry_by_name(const std::string& name) {
    std::string wanted = ns::macro::upper(ns::macro::trim(name));
    if (wanted.empty()) return -1;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (ns::macro::upper(g_macro_entries[i].name) == wanted) return i;
    }
    return -1;
}

std::string unique_macro_name(const std::string& base_raw) {
    std::string base = ns::macro::trim(base_raw);
    if (base.empty()) base = "Recorded Macro";
    std::string name = base;
    int suffix = 2;
    while (find_macro_entry_by_name(name) >= 0) name = base + " " + std::to_string(suffix++);
    return name;
}

bool macro_hotkey_conflicts(const std::string& hotkey, std::string* conflict_name) {
    std::string hk = normalize_key_name(hotkey);
    if (hk.empty()) return false;
    std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
    for (const auto& kv : g_keyBindings) {
        if (normalize_key_name(kv.second) == hk) {
            if (conflict_name) *conflict_name = kv.first;
            return true;
        }
    }
    return false;
}

bool macro_entry_hotkey_conflicts(const std::string& hotkey, int skip_index, std::string* conflict_name) {
    std::string hk = normalize_key_name(hotkey);
    if (hk.empty()) return false;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (i == skip_index) continue;
        if (normalize_key_name(g_macro_entries[i].hotkey) == hk) {
            if (conflict_name) *conflict_name = g_macro_entries[i].name.empty() ? "another macro" : g_macro_entries[i].name;
            return true;
        }
    }
    return false;
}

void rebuild_macro_hotkey_state() {
    g_macro_hotkey_down.clear();
    for (const auto& e : g_macro_entries) {
        std::string hk = normalize_key_name(e.hotkey);
        if (!hk.empty()) g_macro_hotkey_down[hk] = false;
    }
}

void load_macro_entries() {
    std::string err;
    std::vector<ns::macro::Entry> loaded;
    std::string raw = ns::macro::read_text_file_limited(macros_path(), &err);
    if (!ns::macro::parse_entries_text(raw, loaded, err, normalize_macro_hotkey_for_io)) loaded.clear();
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_entries = std::move(loaded);
    rebuild_macro_hotkey_state();
}

bool save_macro_entries_to_disk() {
    std::string json;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        json = ns::macro::entries_to_json(g_macro_entries, normalize_macro_hotkey_for_io);
    }
    if (json.size() > ns::macro::JSON_MAX_BYTES) return false;
    std::ofstream f(macros_path(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(json.data(), (std::streamsize)json.size());
    return (bool)f;
}

std::expected<void, std::string> start_macro_text(const std::string& txt) {
    std::vector<ns::macro::Step> parsed;
    if (!ns::macro::validate_text(txt, parsed, nullptr)) {
        return std::unexpected(ns::macro::last_error());
    }
    std::string pretty, err;
    if (!ns::macro::validate_to_pretty_json(txt, pretty, err, "Macro")) {
        return std::unexpected(err);
    }
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_upload_pending = pretty;
    g_macro_steps = std::move(parsed);
    g_macro_running.store(false, std::memory_order_relaxed);
    g_macro_start_us = ns::now_us();
    return {};
}

std::expected<void, std::string> upsert_macro_entry(ns::macro::Entry e, bool force_unique_name) {
    std::string pretty, err;
    if (!ns::macro::validate_to_pretty_json(e.json, pretty, err, e.name.empty() ? "Macro" : e.name)) {
        return std::unexpected(err);
    }
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        e.name = ns::macro::trim(e.name);
        if (e.name.empty()) e.name = ns::macro::extract_name_or_default(pretty, "Macro");
        if (force_unique_name) e.name = unique_macro_name(e.name);
        e.hotkey = normalize_key_name(e.hotkey);
        e.json = ns::macro::pretty_json_with_forced_name(pretty, e.name);
        int existing = force_unique_name ? -1 : find_macro_entry_by_name(e.name);
        std::string conflict;
        if (macro_hotkey_conflicts(e.hotkey, &conflict) || macro_entry_hotkey_conflicts(e.hotkey, existing, &conflict)) {
            e.hotkey.clear();
        }
        if (existing >= 0) g_macro_entries[existing] = std::move(e);
        else g_macro_entries.push_back(std::move(e));
        rebuild_macro_hotkey_state();
    }
    save_macro_entries_to_disk();
    return {};
}

void poll_macro_entry_hotkeys() {
    std::vector<std::string> to_run;
    const bool input_suspended = keyboard_mouse_input_suspended();
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        for (const auto& e : g_macro_entries) {
            std::string hk = normalize_key_name(e.hotkey);
            if (hk.empty()) continue;
            if (macro_hotkey_conflicts(hk, nullptr)) continue;
            bool down = key_is_down(hk);
            bool was_down = g_macro_hotkey_down[hk];
            g_macro_hotkey_down[hk] = down;
            if (!input_suspended && down && !was_down) to_run.push_back(e.json);
        }
    }
    for (const auto& json : to_run) (void)start_macro_text(json);
}

ns::macro::Recorder g_macro_recorder;

void macro_record_start() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recorder.start(ns::now_us());
    g_macro_recording.store(true, std::memory_order_relaxed);
}

std::string macro_record_stop() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recording.store(false, std::memory_order_relaxed);
    return g_macro_recorder.stop(ns::now_us());
}

void macro_record_sample(const ns::HoriHIDReport& report) {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recorder.sample(report, ns::now_us(), g_macro_running.load(std::memory_order_relaxed));
}

bool poll_macro_record_p1(ns::HoriHIDReport& report) {
    report.reset();
    auto sdl = g_sdlInput.snapshot();
    if (sdl[0].connected) {
        report = sdl[0].input;
        apply_controller_bindings(report);
        return true;
    }
    int km = g_keyboardMode.load();
    if (km != KB_OFF) {
        apply_keyboard_to_report(report, km == KB_OVERRIDE);
        return true;
    }
    return false;
}

void macro_record_sample_p1() {
    ns::HoriHIDReport report;
    poll_macro_record_p1(report);
    macro_record_sample(report);
}

bool apply_macro_override(ns::HoriHIDReport logical_reports[4], bool present[4], bool has_motion[4]) {
    (void)has_motion;
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_running.load(std::memory_order_relaxed)) return false;
    uint64_t elapsed_ms = (ns::now_us() - g_macro_start_us) / 1000ULL;
    ns::HoriHIDReport mr;
    bool active = ns::macro::report_at(g_macro_steps, elapsed_ms, mr);
    for (int i = 0; i < 4; ++i) {
        logical_reports[i].reset();
        present[i] = false;
        has_motion[i] = false;
    }
    logical_reports[0] = mr;
    present[0] = true;
    if (!active && elapsed_ms > ns::macro::total_ms(g_macro_steps) + 120) g_macro_running.store(false, std::memory_order_relaxed);
    return true;
}

