#include "virtual_controller.hpp"
#include "app_state.hpp"
#include "bluetooth_manager.hpp"
#include "gadget_wakeup.hpp"
#include "switch2_native.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <print>
#include <format>
#include <span>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>

using namespace ns;

uint8_t pro_timer_from_us(uint64_t t_us) { return (uint8_t)((t_us / 5000ULL) & 0xFF); }

static void write_u16le(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static void write_i16le(uint8_t* dst, int16_t v) {
    write_u16le(dst, static_cast<uint16_t>(v));
}

static void write_u32le(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

static void write_i32le(uint8_t* dst, int32_t v) {
    write_u32le(dst, static_cast<uint32_t>(v));
}

namespace {

struct S2MotionState {
    uint16_t tick = 0;
    uint8_t cadence_phase = 0;
    uint8_t controller_type = 0;
    bool active = false;
    std::array<uint32_t, 3> angular_phase{0x00000000u, 0x00000000u, 0x80000000u};
};

struct S2AveragedMotion {
    double accel[3]{0.0, 0.0, 4096.0};
    double gyro[3]{0.0, 0.0, 0.0};
};

std::array<S2MotionState, HID_PORT_COUNT> g_s2_motion_state{};
std::mutex g_s2_motion_mtx;

constexpr std::array<uint8_t, 5> S2_IMU_TICK_PATTERN = {3, 3, 3, 3, 4};
constexpr int16_t S2_DEFAULT_TEMPERATURE = 0x0C00;
// MotionReport retains the Switch 1 raw IMU unit used by every client:
// 16.384 counts per degree/second. Convert that unit accurately when updating
// the native S2 phase accumulator instead of treating it as 48000/turn.
constexpr double S2_GYRO_COUNTS_PER_FULL_TURN_PER_SEC = 360.0 * 16.384;
constexpr double S2_INTERNAL_IMU_HZ = 800.0;
constexpr long double S2_PHASE_UNITS_PER_TURN = 4294967296.0L;

// Majority-bit template from a real, stationary, face-up Right Joy-Con 2
// 40-byte native motion capture. Confirmed sensor fields are overwritten below;
// unknown codec state is intentionally preserved instead of being zero-filled.
constexpr std::array<uint8_t, 36> JOYCON2_STATIONARY_PAYLOAD = {
    0x83, 0x90, 0xF8, 0xA9, 0x84, 0x3C, 0x00, 0xD0, 0xE7,
    0x02, 0x48, 0xEE, 0x88, 0x4F, 0xFE, 0xEF, 0xFF, 0xFF,
    0xDF, 0x02, 0x2C, 0x77, 0xE3, 0x03, 0xFF, 0xEB, 0xFF,
    0xFE, 0xBF, 0x0B, 0x20, 0xB9, 0x33, 0x3E, 0xC0, 0x03
};

int s2_motion_port(int port) {
    return (port >= 0 && port < HID_PORT_COUNT) ? port : 0;
}

void reset_s2_state_locked(S2MotionState& state, uint8_t controller_type) {
    state = S2MotionState{};
    state.controller_type = controller_type;
}

uint16_t advance_s2_motion_clock(S2MotionState& state, uint8_t& elapsed_ticks) {
    elapsed_ticks = S2_IMU_TICK_PATTERN[state.cadence_phase];
    state.cadence_phase = static_cast<uint8_t>((state.cadence_phase + 1u) % S2_IMU_TICK_PATTERN.size());
    state.tick = static_cast<uint16_t>((state.tick + elapsed_ticks) & 0x0FFFu);
    return static_cast<uint16_t>((static_cast<uint16_t>(elapsed_ticks) << 12) | state.tick);
}

S2AveragedMotion average_s2_motion(const MotionReport motion_samples[3], bool has_motion) {
    S2AveragedMotion result{};
    if (!has_motion || !motion_samples) return result;

    const int32_t accel_sum[3] = {
        static_cast<int32_t>(motion_samples[0].ax) + motion_samples[1].ax + motion_samples[2].ax,
        static_cast<int32_t>(motion_samples[0].ay) + motion_samples[1].ay + motion_samples[2].ay,
        static_cast<int32_t>(motion_samples[0].az) + motion_samples[1].az + motion_samples[2].az,
    };
    const int32_t gyro_sum[3] = {
        static_cast<int32_t>(motion_samples[0].gx) + motion_samples[1].gx + motion_samples[2].gx,
        static_cast<int32_t>(motion_samples[0].gy) + motion_samples[1].gy + motion_samples[2].gy,
        static_cast<int32_t>(motion_samples[0].gz) + motion_samples[1].gz + motion_samples[2].gz,
    };
    for (size_t axis = 0; axis < 3; ++axis) {
        result.accel[axis] = static_cast<double>(accel_sum[axis]) / 3.0;
        result.gyro[axis] = static_cast<double>(gyro_sum[axis]) / 3.0;
    }
    return result;
}

bool put_bits_lsb(uint8_t* dst, size_t dst_len, unsigned start_bit, unsigned width, uint32_t value) {
    if (!dst || width == 0 || width > 31 || start_bit + width > dst_len * 8u) return false;
    for (unsigned bit = 0; bit < width; ++bit) {
        const unsigned absolute = start_bit + bit;
        const size_t byte_index = absolute / 8u;
        const uint8_t mask = static_cast<uint8_t>(1u << (absolute % 8u));
        if ((value >> bit) & 1u) dst[byte_index] |= mask;
        else dst[byte_index] &= static_cast<uint8_t>(~mask);
    }
    return true;
}

uint32_t pack_signed_field(int64_t value, unsigned width) {
    const int64_t min_value = -(int64_t{1} << (width - 1u));
    const int64_t max_value =  (int64_t{1} << (width - 1u)) - 1;
    const int64_t clipped = std::clamp(value, min_value, max_value);
    return static_cast<uint32_t>(clipped) & ((uint32_t{1} << width) - 1u);
}

int32_t accel_q16(double raw_counts) {
    constexpr int64_t min_q16 = std::numeric_limits<int32_t>::min();
    constexpr int64_t max_q16 = std::numeric_limits<int32_t>::max();
    const long double scaled = static_cast<long double>(raw_counts) * 65536.0L;
    const int64_t rounded = static_cast<int64_t>(std::llround(scaled));
    return static_cast<int32_t>(std::clamp(rounded, min_q16, max_q16));
}

// Native Joy-Con 2 gyro shares the raw sensor unit with MotionReport
// (16.384 counts per degree/second measured as ~16.4 by gravity-integration),
// so gyro values pass through 1:1. The transmitted fields clamp at +/-500 dps.
constexpr int64_t JOYCON2_GYRO_CLAMP_COUNTS = 8191;  // int14 full scale

void mark_s2_motion_inactive(int port) {
    std::lock_guard<std::mutex> lock(g_s2_motion_mtx);
    g_s2_motion_state[s2_motion_port(port)].active = false;
}

} // namespace

void reset_s2_motion_state(int port) {
    std::lock_guard<std::mutex> lock(g_s2_motion_mtx);
    reset_s2_state_locked(g_s2_motion_state[s2_motion_port(port)], 0);
}

// Plain modern descriptor: reports 0x30/0x21/0x81 and the 0x01/0x10/0x80 outputs,
// max report 64 bytes. This is the exact descriptor the console-matched wired
// session enumerated with; it keeps the interrupt endpoints at 64 bytes.
extern const uint8_t VIRTUAL_CONTROLLER_REPORT_DESC[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0xC0
};
extern const size_t VIRTUAL_CONTROLLER_REPORT_DESC_SIZE = sizeof(VIRTUAL_CONTROLLER_REPORT_DESC);

extern const uint8_t LEGACY_REPORT_DESC[85] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x01,0x75,0x01,
    0x95,0x0D,0x05,0x09,0x19,0x01,0x29,0x0D,0x81,0x02,0x95,0x03,0x81,0x01,0x05,0x01,
    0x25,0x07,0x46,0x3B,0x01,0x75,0x04,0x95,0x01,0x65,0x14,0x09,0x39,0x81,0x42,
    0x65,0x00,0x95,0x01,0x81,0x01,0x26,0xFF,0x00,0x46,0xFF,0x00,0x09,0x30,0x09,
    0x31,0x09,0x32,0x09,0x35,0x75,0x08,0x95,0x04,0x81,0x02,0x06,0x00,0xFF,0x09,
    0x20,0x75,0x08,0x95,0x01,0x81,0x02,0xC0
};

// Exact Switch 2 Pro Controller HID report descriptor from PicoSwitch2/ns2-testing
// and ndeadly's controller research: input reports 0x05/0x09 and output 0x02.
// Joy-Con 2 report IDs are intentionally not mixed into this native Pro2 path.
extern const uint8_t S2_PRO_REPORT_DESC[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x85, 0x05, 0x05, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x95, 0x3F, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x09, 0x09, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x15, 0x25, 0x01,
    0x95, 0x15, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x35,
    0x26, 0xFF, 0x0F, 0x95, 0x04, 0x75, 0x0C, 0x81, 0x02,
    0xC0,
    0x05, 0xFF, 0x09, 0x02, 0x26, 0xFF, 0x00,
    0x95, 0x34, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x02, 0x09, 0x01, 0x95, 0x3F, 0x91, 0x02,
    0xC0
};
extern const size_t S2_PRO_REPORT_DESC_SIZE = sizeof(S2_PRO_REPORT_DESC);
static_assert(sizeof(S2_PRO_REPORT_DESC) == 97, "S2 Pro Controller HID descriptor must be 97 bytes");

uint8_t CTRL_MAC_BE[4][6] = {
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA0}, {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA1},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA2}, {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA3}
};

std::string CTRL_SERIAL[4] = { "NSGP260606A0", "NSGP260606A1", "NSGP260606A2", "NSGP260606A3" };

const uint8_t VIRTUAL_BODY_RGB[4][3] = {
    {0xE6, 0x00, 0x12}, // virtual Pro Controller 1: red
    {0xFF, 0xCC, 0x00}, // virtual Pro Controller 2: yellow
    {0x00, 0x64, 0xFF}, // virtual Pro Controller 3: blue
    {0x00, 0xC8, 0x53}, // virtual Pro Controller 4: green
};

// ===========================================================================
// Joy-Con (R) mode (--joycon)
//
// The console accepts wired Joy-Con input when the USB session is typed Pro
// Controller in the 0x80-01 status reply while device info (subcmd 0x02),
// SPI 0x6012 and the per-report conn_info nibble claim Joy-Con (R). Found by
// the identity experiments on 2026-07-04; keeping 0x80-01 = Pro is what stops
// the console from downgrading the session to a pairing-only channel.
// ===========================================================================

static uint8_t g_port_controller_type[HID_PORT_COUNT] = {
    NS_TYPE_PRO, NS_TYPE_PRO, NS_TYPE_PRO, NS_TYPE_PRO
};
bool g_port_switch2[HID_PORT_COUNT] = { false, false, false, false };
static uint8_t g_port_protocol_type[HID_PORT_COUNT] = {
    ns::CONTROLLER_TYPE_PRO, ns::CONTROLLER_TYPE_PRO, ns::CONTROLLER_TYPE_PRO, ns::CONTROLLER_TYPE_PRO
};

// Identity the console last read during a USB handshake, and when the live
// types last diverged from it. See s1_identity_reenumeration_due().
static uint8_t g_enumerated_ns_type[HID_PORT_COUNT] = {
    NS_TYPE_PRO, NS_TYPE_PRO, NS_TYPE_PRO, NS_TYPE_PRO
};
static std::atomic<uint64_t> g_s1_identity_change_us{0};

static std::vector<uint8_t> g_amiibo_data[HID_PORT_COUNT];
static std::chrono::steady_clock::time_point g_amiibo_expiry[HID_PORT_COUNT];
static bool g_amiibo_modified[HID_PORT_COUNT] = {};
static std::mutex g_amiibo_mtx;

static void publish_amiibo_request_for_port(int port, bool requested);

namespace {

std::string nfc_hex(std::span<const uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

long long nfc_expiry_remaining_ms(int port, std::chrono::steady_clock::time_point now) {
    if (port < 0 || port >= HID_PORT_COUNT || g_amiibo_data[port].empty()) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(g_amiibo_expiry[port] - now).count();
}

} // namespace

bool is_amiibo_placed(int port) {
    if (port < 0 || port >= HID_PORT_COUNT) return false;
    std::lock_guard<std::mutex> lk(g_amiibo_mtx);
    return !g_amiibo_data[port].empty() && std::chrono::steady_clock::now() < g_amiibo_expiry[port];
}
constexpr size_t SPI_FLASH_SIZE = 0x200000; // 2MB per research for S2, 64k for S1 compatibility
uint8_t g_spi_flash[4][SPI_FLASH_SIZE];
bool g_spi_initialized[4] = {};

uint8_t controller_type_for_port(int ctrl) {
    return ctrl >= 0 && ctrl < HID_PORT_COUNT ? g_port_controller_type[ctrl] : NS_TYPE_PRO;
}

void configure_usb_controller_family(UsbControllerFamily family) {
    g_ctx.usb_controller_family = family;
    uint8_t profile = ns::CONTROLLER_TYPE_PRO;
    switch (family) {
        case UsbControllerFamily::Switch1:
            profile = ns::CONTROLLER_TYPE_PRO;
            break;
        case UsbControllerFamily::Switch2:
            profile = ns::CONTROLLER_TYPE_PRO_S2;
            break;
        case UsbControllerFamily::Hori:
            profile = ns::CONTROLLER_TYPE_HORI;
            break;
    }
    for (int port = 0; port < HID_PORT_COUNT; ++port) {
        uint8_t port_profile = profile;
        if (family == UsbControllerFamily::Switch2 && port != 0) {
            // Non-native slots are real Switch 1 f_hid functions and must
            // start with a matching S1 identity even before a client is mapped.
            port_profile = ns::CONTROLLER_TYPE_PRO;
        }
        set_controller_type_for_port(port, port_profile);
    }
}

void set_amiibo_data_for_port(int port, const uint8_t* data, size_t len) {
    if (g_ctx.verbose) {
        std::println("[s2][nfc][upload] t_us={} port={} len={} data_ptr={} supports_nfc={}",
                     now_us(), port, len, data != nullptr,
                     port >= 0 && port < HID_PORT_COUNT && controller_port_supports_amiibo(port));
    }
    if (port < 0 || port >= HID_PORT_COUNT || !data) {
        if (g_ctx.verbose)
            std::println(stderr, "[s2][nfc][upload] rejected: invalid port or null data");
        return;
    }
    if (!controller_port_supports_amiibo(port)) {
        if (g_ctx.verbose) std::println(stderr, "[s2][nfc][upload] rejected: port {} has no native NFC", port);
        return;
    }
    constexpr size_t AMIIBO_SIZE = 540;
    // A short/overlong image cannot be exposed as a raw NTAG215 dump without
    // changing its page layout, UID, lock bytes, or configuration pages.
    if (len != AMIIBO_SIZE) {
        if (g_ctx.verbose)
            std::println(stderr, "[s2][nfc][upload] rejected: expected 540 bytes, got {}", len);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_amiibo_mtx);
        g_amiibo_data[port].assign(data, data + AMIIBO_SIZE);
        g_amiibo_expiry[port] = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        g_amiibo_modified[port] = false;
    }
    if (g_ctx.verbose) {
        std::println("[s2][nfc][upload] accepted t_us={} port={} uid={:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x} expiry_ms=15000 modified=false",
                     now_us(), port, data[0], data[1], data[2], data[4], data[5], data[6], data[7]);
        std::println("[s2][nfc][upload] raw_bin={}",
                     nfc_hex(std::span<const uint8_t>(data, AMIIBO_SIZE)));
    }
    // Selecting a tag satisfies the outstanding UI request immediately.
    publish_amiibo_request_for_port(port, false);
}

void check_amiibo_expiry(int port) {
    if (port < 0 || port >= HID_PORT_COUNT) return;
    std::vector<uint8_t> writeback;
    bool expired = false;
    bool was_modified = false;
    {
        std::lock_guard<std::mutex> lk(g_amiibo_mtx);
        if (g_amiibo_data[port].empty()
                || std::chrono::steady_clock::now() <= g_amiibo_expiry[port]) return;
        expired = true;
        was_modified = g_amiibo_modified[port];
        if (was_modified) writeback = g_amiibo_data[port];
        g_amiibo_data[port].clear();
        g_amiibo_modified[port] = false;
    }
    if (g_ctx.verbose && expired) {
        std::println("[s2][nfc][expiry] t_us={} port={} expired=true modified={} writeback_len={}",
                     now_us(), port, was_modified, writeback.size());
        if (!writeback.empty())
            std::println("[s2][nfc][expiry] writeback_raw_bin={}", nfc_hex(writeback));
    }
    if (!writeback.empty()) {
        int client_idx = -1;
        int sub_idx = -1;
        if (client_subpad_for_console_port(port, client_idx, sub_idx)) {
            if (g_ctx.verbose)
                std::println("[s2][nfc][expiry] routing writeback port={} -> client={} subpad={}",
                             port, client_idx, sub_idx);
            publish_amiibo_writeback(client_idx, sub_idx, writeback.data(),
                                     static_cast<uint16_t>(writeback.size()));
        } else if (g_ctx.verbose) {
            std::println(stderr, "[s2][nfc][expiry] no client/subpad mapping for port {}; writeback dropped", port);
        }
    }
}

static void publish_amiibo_request_for_port(int port, bool requested) {
    int client_idx = -1;
    int sub_idx = -1;
    if (client_subpad_for_console_port(port, client_idx, sub_idx)) {
        if (g_ctx.verbose)
            std::println("[s2][nfc][ui-route] t_us={} port={} requested={} -> client={} subpad={}",
                         now_us(), port, requested, client_idx, sub_idx);
        publish_amiibo_request(client_idx, sub_idx, requested);
    } else if (g_ctx.verbose) {
        std::println(stderr, "[s2][nfc][ui-route] port={} requested={} has no client/subpad mapping", port, requested);
    }
}

void set_controller_type_for_port(int ctrl, uint8_t protocol_type) {
    if (ctrl < 0 || ctrl >= HID_PORT_COUNT) return;
    if (g_port_protocol_type[ctrl] == protocol_type) return;

    bool is_s2 = false;
    uint8_t ns_type = NS_TYPE_PRO;
    switch (protocol_type) {
        case ns::CONTROLLER_TYPE_JOYCON_L:
        case ns::CONTROLLER_TYPE_JOYCON_L_S2:
            ns_type = NS_TYPE_JOYCON_L;
            is_s2 = (protocol_type == ns::CONTROLLER_TYPE_JOYCON_L_S2);
            break;
        case ns::CONTROLLER_TYPE_JOYCON_R:
        case ns::CONTROLLER_TYPE_JOYCON_R_S2:
            ns_type = NS_TYPE_JOYCON_R;
            is_s2 = (protocol_type == ns::CONTROLLER_TYPE_JOYCON_R_S2);
            break;
        case ns::CONTROLLER_TYPE_JOYCON_PAIR:
        case ns::CONTROLLER_TYPE_JOYCON_PAIR_S2:
            // Pair sets L and R separately in caller
            ns_type = NS_TYPE_PRO;
            is_s2 = (protocol_type == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2);
            break;
        case ns::CONTROLLER_TYPE_PRO:
        case ns::CONTROLLER_TYPE_PRO_S2:
            ns_type = NS_TYPE_PRO;
            is_s2 = (protocol_type == ns::CONTROLLER_TYPE_PRO_S2);
            break;
        case ns::CONTROLLER_TYPE_HORI:
            ns_type = NS_TYPE_HORI;
            is_s2 = false;
            break;
        default:
            ns_type = NS_TYPE_PRO;
            is_s2 = false;
            break;
    }
    
    const uint8_t prev_ns_type = g_port_controller_type[ctrl];
    g_port_controller_type[ctrl] = ns_type;
    g_port_switch2[ctrl] = is_s2;
    g_port_protocol_type[ctrl] = protocol_type;
    g_spi_initialized[ctrl] = false;
    init_spi_flash(ctrl);

    // Native S2 ports carry the split identity in switch2_native's factory
    // block (memory 0x13014 + ep0 identity), which the console reads instead
    // of the S1 SPI image. Keep it in sync with the selected type.
    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2
            && ctrl == 0) {
        uint8_t pid_lo = 0x69; // Pro Controller 2
        if (ns_type == NS_TYPE_JOYCON_R) pid_lo = 0x66;
        else if (ns_type == NS_TYPE_JOYCON_L) pid_lo = 0x67;
        switch2_native_set_port_pid(ctrl, pid_lo);
    }

    // The USB descriptor and device identity are fixed by UsbControllerFamily
    // at startup; rebuilding the gadget here directly used to disconnect every
    // player whenever any source appeared, disappeared, or changed profile.
    // But the console reads identity only during the USB handshake — S1
    // device info/SPI and the S2 ep0 identity / factory 0x13014 alike — so a
    // type change stays invisible until the gadget re-enumerates. Note the
    // change; the writer forces one debounced re-enumeration once the layout
    // settles. Hori is the only fixed-identity family.
    if (ns_type != prev_ns_type && g_ctx.usb_controller_family != UsbControllerFamily::Hori) {
        g_s1_identity_change_us.store(now_us(), std::memory_order_relaxed);
    }
}

void mark_s1_identity_enumerated() {
    std::memcpy(g_enumerated_ns_type, g_port_controller_type, sizeof(g_enumerated_ns_type));
    g_s1_identity_change_us.store(0, std::memory_order_relaxed);
}

bool s1_identity_reenumeration_due(uint64_t now) {
    // Hori has a fixed identity. Switch 1 identity and the native Switch 2
    // EP0/factory identity are both latched by the console during enumeration,
    // so either family must re-enumerate after a settled Pro/L/R type change.
    if (g_ctx.usb_controller_family == UsbControllerFamily::Hori) return false;
    const uint64_t changed = g_s1_identity_change_us.load(std::memory_order_relaxed);
    if (changed == 0) return false;
    if (elapsed_us_saturated(now, changed) < S1_TYPE_REENUM_QUIET_US) return false;
    if (std::memcmp(g_enumerated_ns_type, g_port_controller_type, sizeof(g_enumerated_ns_type)) == 0) {
        // Settled back to exactly what the console already read.
        g_s1_identity_change_us.store(0, std::memory_order_relaxed);
        return false;
    }
    return true;
}

uint8_t controller_protocol_type_for_port(int ctrl) {
    if (ctrl >= 0 && ctrl < HID_PORT_COUNT) return g_port_protocol_type[ctrl];
    return ns::CONTROLLER_TYPE_PRO;
}

uint8_t switch2_input_report_id_for_port(int ctrl) {
    switch (controller_type_for_port(ctrl)) {
        case NS_TYPE_JOYCON_L: return 0x07;
        case NS_TYPE_JOYCON_R: return 0x08;
        case NS_TYPE_PRO:
        default: return 0x09;
    }
}

uint8_t switch2_output_report_id_for_port(int ctrl) {
    switch (controller_type_for_port(ctrl)) {
        case NS_TYPE_JOYCON_L:
        case NS_TYPE_JOYCON_R:
            return 0x01;
        case NS_TYPE_PRO:
        default:
            return 0x02;
    }
}

bool controller_port_supports_amiibo(int ctrl) {
    if (ctrl < 0 || ctrl >= HID_PORT_COUNT) return false;
    if (!g_port_switch2[ctrl]) return false;
    const uint8_t t = controller_type_for_port(ctrl);
    return t == NS_TYPE_PRO || t == NS_TYPE_JOYCON_R;
}

void apply_controller_type_input(uint8_t type, HIDReport& r, bool pair_member) {
    if (type == NS_TYPE_JOYCON_R) {
        // Single-stick clients normally drive the left stick; a right Joy-Con
        // exposes that physical stick in the right-stick report field.
        if (!pair_member && r.input.rx == 128 && r.input.ry == 128) {
            r.input.rx = r.input.lx; r.input.ry = r.input.ly;
        }
        r.input.buttons &= BTN_Y | BTN_B | BTN_A | BTN_X | BTN_R | BTN_ZR |
                           BTN_PLUS | BTN_RSTICK | BTN_HOME;
        r.input.hat = HAT_NEUTRAL;
        r.input.lx = r.input.ly = 128;
    } else if (type == NS_TYPE_JOYCON_L) {
        r.input.buttons &= BTN_L | BTN_ZL | BTN_MINUS | BTN_LSTICK | BTN_CAPTURE;
        r.input.rx = r.input.ry = 128;
    }
}

void apply_controller_type_report(uint8_t type, uint8_t extra_buttons, uint8_t* buf) {
    if (type != NS_TYPE_JOYCON_L && type != NS_TYPE_JOYCON_R) return;
    // buf is a 0x30/0x21 report: conn_info at [2], buttons at [3..5].
    // conn_info low nibble 0xF = Joy-Con ((v >> 1) & 3 == 3) + USB-powered bit.
    buf[2] = (buf[2] & 0xF0) | 0x0F;
    // Map the side's shoulder/trigger pair onto SR/SL so the normal
    // single-Joy-Con "press SL+SR" registration gesture remains available.
    uint8_t& side = type == NS_TYPE_JOYCON_R ? buf[3] : buf[5];
    if (side & 0x40) side |= 0x10;
    if (side & 0x80) side |= 0x20;
    if (extra_buttons & EXT_BUTTON_SR) side |= 0x10;
    if (extra_buttons & EXT_BUTTON_SL) side |= 0x20;
}

void apply_s2_controller_type_report(uint8_t type, uint8_t* buf) {
    if (type != NS_TYPE_JOYCON_L && type != NS_TYPE_JOYCON_R) return;
    if (buf[0] != 0x09) return;
    // buf is a 0x09 report: power info at [2], buttons at [3..5].
    // S2 does NOT use conn_info in buf[2] like S1, so we leave it as battery info.
    // Map L/R and ZL/ZR to spare bits in buf[5] for SL/SR single-joycon pairing gesture.
    // In S2 buf[5]: bits 5,6,7 are unused (0x20, 0x40, 0x80).
    // Let's map SR to 0x40 and SL to 0x80.
    if (type == NS_TYPE_JOYCON_R) {
        if (buf[3] & 0x10) buf[5] |= 0x40; // R -> SR
        if (buf[3] & 0x20) buf[5] |= 0x80; // ZR -> SL
    } else {
        if (buf[4] & 0x10) buf[5] |= 0x40; // L -> SR
        if (buf[4] & 0x20) buf[5] |= 0x80; // ZL -> SL
    }
}

bool read_random_bytes(uint8_t* dst, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t r = read(fd, dst + off, len - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        close(fd); if (off == len) return true;
    }
    uint64_t seed = (uint64_t)now_us() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < len; ++i) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; dst[i] = (uint8_t)seed; }
    return true;
}

void randomize_controller_identity() {
    uint8_t rnd[16]{}; read_random_bytes(rnd, sizeof(rnd));
    for (int i = 0; i < 4; ++i) {
        CTRL_MAC_BE[i][0] = 0x02; CTRL_MAC_BE[i][1] = 0x4E; CTRL_MAC_BE[i][2] = 0x53;
        CTRL_MAC_BE[i][3] = rnd[(i * 3) % 16]; CTRL_MAC_BE[i][4] = rnd[(i * 3 + 1) % 16]; CTRL_MAC_BE[i][5] = rnd[(i * 3 + 2) % 16] + i;
        CTRL_SERIAL[i] = "NSGP" + std::format("{:02X}{:02X}{:02X}{:02X}", CTRL_MAC_BE[i][2], CTRL_MAC_BE[i][3], CTRL_MAC_BE[i][4], CTRL_MAC_BE[i][5]);
    }
    g_ctx.usb_serial = std::format("{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
}

void init_spi_flash(int ctrl) {
    if (ctrl < 0 || ctrl >= 4 || g_spi_initialized[ctrl]) return;
    uint8_t* flash = g_spi_flash[ctrl]; std::memset(flash, 0xFF, SPI_FLASH_SIZE);
    uint8_t prod = controller_type_for_port(ctrl);
    flash[0x6012] = prod; flash[0x6013] = 0xA0; flash[0x601B] = 0x02;
    if (g_port_switch2[ctrl]) {
        // Same trick as the Switch 1 Joy-Con discovery: the USB device always
        // enumerates as a Pro Controller 2 (PID 0x2069) to keep the wired
        // session alive, but the factory-memory product ID reported to the
        // console identifies the actual controller. Per ndeadly research:
        //   Pro Controller 2 = 0x2069, Joy-Con 2 (R) = 0x2066, (L) = 0x2067.
        uint8_t pid_lo = 0x69; // Pro Controller 2
        switch (controller_type_for_port(ctrl)) {
            case NS_TYPE_JOYCON_R: pid_lo = 0x66; break;
            case NS_TYPE_JOYCON_L: pid_lo = 0x67; break;
            default: break;
        }
        // S2 factory data (memory_layout.md): serial, VID/PID, colors.
        static const char kSerial[] = "HEJ710011212470";
        std::memcpy(flash + 0x13002, kSerial, 15); // 0x13002..0x13011 (16B, last NUL)
        flash[0x13012] = 0x7E; flash[0x13013] = 0x05;          // VID 0x057E (LE)
        flash[0x13014] = pid_lo; flash[0x13015] = 0x20;        // PID 0x20xx (LE)
        flash[0x13019] = 0x23; flash[0x1301A] = 0x23; flash[0x1301B] = 0x23; // body
        flash[0x1301C] = 0xA0; flash[0x1301D] = 0xA0; flash[0x1301E] = 0xA0; // buttons
        flash[0x1301F] = 0xE6; flash[0x13020] = 0xE6; flash[0x13021] = 0xE6; // highlight
        flash[0x13022] = 0x32; flash[0x13023] = 0x32; flash[0x13024] = 0x32; // grip
        flash[0x1FA000] = 0x00; // pairing count = 0 over USB
    }

    auto put16 = [&](uint16_t a, int16_t v) { flash[a] = v; flash[a+1] = v >> 8; };
    auto pack12 = [](uint8_t* d, uint16_t x, uint16_t y) { d[0] = x; d[1] = (x >> 8) | (y << 4); d[2] = y >> 4; };

    pack12(flash + 0x603D, 0x600, 0x600); pack12(flash + 0x6040, 0x800, 0x800);
    pack12(flash + 0x6043, 0x600, 0x600); pack12(flash + 0x6046, 0x800, 0x800);
    pack12(flash + 0x6049, 0x600, 0x600); pack12(flash + 0x604C, 0x600, 0x600);

    const int16_t imu[12] = {0, 0, 0, 0x4000, 0x4000, 0x4000, 0, 0, 0, 0x343B, 0x343B, 0x343B};
    for (int i = 0; i < 12; ++i) put16(0x6020 + i * 2, imu[i]);
    put16(0x6080, 0); put16(0x6082, 0); put16(0x6084, 0); pack12(flash + 0x6089, 0x0A0, 0x100);
    for (size_t i = 6; i < 0x24; ++i) flash[0x6086 + i] = (i & 1) ? 0x30 : 0x0F;

    // Joy-Cons deliberately share the same per-player palette as Pro Controllers.
    std::memcpy(flash + 0x6050, VIRTUAL_BODY_RGB[ctrl], 3); std::memset(flash + 0x6053, 0xFF, 3);
    std::memcpy(flash + 0x6056, VIRTUAL_BODY_RGB[ctrl], 3); std::memset(flash + 0x6059, 0xFF, 3);
    flash[0x605C] = 0x00; g_spi_initialized[ctrl] = true;
}

void set_identity_in_0x81(uint8_t* r81, int ctrl) {
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    r81[4] = mac[5]; r81[5] = mac[4]; r81[6] = mac[3]; r81[7] = mac[2]; r81[8] = mac[1]; r81[9] = mac[0];
}

size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl) {
    memset(out, 0, PRO_REPORT_SIZE); out[0] = 0x81; out[1] = subtype;
    if (subtype == 0x01) {
        uint8_t dev_type = NS_TYPE_PRO;
        out[3] = dev_type;
        set_identity_in_0x81(out, ctrl);
    }
    return PRO_REPORT_SIZE;
}

void build_get_device_info_response(uint8_t* out, int ctrl) {
    // Keep the 0x80-01 USB-session type as Pro Controller, but report the
    // logical controller type here. This split identity is what lets wired
    // Joy-Con profiles stay on the USB input path instead of being pushed into
    // a Bluetooth-pairing-only flow.
    uint8_t dev_type = controller_type_for_port(ctrl);
    memset(out, 0, 36); out[0] = 0x03; out[1] = 0x49; out[2] = dev_type; out[3] = 0x02;
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    out[4] = mac[5]; out[5] = mac[4]; out[6] = mac[3]; out[7] = mac[2]; out[8] = mac[1]; out[9] = mac[0];
    out[10] = 0x01; out[11] = 0x02;
}

uint8_t pro_conn_info_from_hid(const HIDReport& src) {
    if ((src.reserved[1] & EXT_STATUS_BATTERY_VALID) == 0 || src.reserved[0] > 100) return PRO_BAT_CON;

    const int pct = src.reserved[0];
    uint8_t coarse_level = 0;
    if (pct >= 90) coarse_level = 4;      // full
    else if (pct >= 70) coarse_level = 3; // high
    else if (pct >= 45) coarse_level = 2; // medium
    else if (pct >= 20) coarse_level = 1; // low
    else coarse_level = 0;                // critical/empty

    // Pro Controller bat_con byte:
    //   bits 7..5 = coarse battery level (0..4)
    //   bit 4     = charging/external power
    //   bit 0     = controller connected/host-powered flag used by our capture profile
    uint8_t bat_con = static_cast<uint8_t>((coarse_level << 5) | (PRO_BAT_CON & 0x01));
    if (src.reserved[1] & EXT_STATUS_BATTERY_CHARGING) bat_con |= 0x10;
    return bat_con;
}

// The console drives a flashing/cycling player-LED pattern (upper nibble = flash bits) while
// the "Change Grip/Order" / controller-pairing screen is open; a steady player assignment uses
// only the lower (solid) nibble. Our emulated pad always reports full battery, so the console
// never flashes for low power — a flash pattern reliably means a controller-management screen.
bool player_leds_indicate_pairing(uint8_t player_leds) {
    return (player_leds & 0xF0) != 0;
}

void fill_neutral_controls(ProInputReport30& r) {
    r.conn_info = PRO_BAT_CON; r.buttons[0] = 0x00; r.buttons[1] = 0x80; r.buttons[2] = 0x00;
    r.left_stick[0] = r.right_stick[0] = 0x00; r.left_stick[1] = r.right_stick[1] = 0x08; r.left_stick[2] = r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT; memset(r.vendor_rest, 0, sizeof(r.vendor_rest));
}

void fill_neutral_controls(ProInputReport21& r) {
    r.conn_info = PRO_BAT_CON; r.buttons[0] = 0x00; r.buttons[1] = 0x80; r.buttons[2] = 0x00;
    r.left_stick[0] = r.right_stick[0] = 0x00; r.left_stick[1] = r.right_stick[1] = 0x08; r.left_stick[2] = r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT;
}

uint16_t axis8_to_12(uint8_t v) {
    if (v == 128) return 0x800;
    int32_t delta = (int32_t)v - 128;
    return (uint16_t)std::clamp(0x800 + (delta * 0x600) / (delta > 0 ? 127 : 128), 0x200, 0xE00);
}

uint8_t invert_axis8_centered(uint8_t v) { return v == 128 ? 128 : (uint8_t)(255 - v); }

void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8) {
    uint16_t x = axis8_to_12(x8), y = axis8_to_12(invert_axis8_centered(y8));
    out[0] = x; out[1] = (x >> 8) | (y << 4); out[2] = y >> 4;
}

void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]) {
    if (hat > 7) return;
    static const uint8_t hat_map[8] = { 0x02, 0x02 | 0x04, 0x04, 0x01 | 0x04, 0x01, 0x01 | 0x08, 0x08, 0x02 | 0x08 };
    buttons[2] |= hat_map[hat];
}

void map_buttons(uint16_t in_btns, uint8_t hat, uint8_t out_btns[3]) {
    out_btns[0] = (in_btns & BTN_Y ? 0x01 : 0) | (in_btns & BTN_X ? 0x02 : 0) | (in_btns & BTN_B ? 0x04 : 0) |
                  (in_btns & BTN_A ? 0x08 : 0) | (in_btns & BTN_R ? 0x40 : 0) | (in_btns & BTN_ZR ? 0x80 : 0);
    out_btns[1] = 0x80 | (in_btns & BTN_MINUS ? 0x01 : 0) | (in_btns & BTN_PLUS ? 0x02 : 0) |
                  (in_btns & BTN_RSTICK ? 0x04 : 0) | (in_btns & BTN_LSTICK ? 0x08 : 0) |
                  (in_btns & BTN_HOME ? 0x10 : 0) | (in_btns & BTN_CAPTURE ? 0x20 : 0);
    out_btns[2] = (in_btns & BTN_L ? 0x40 : 0) | (in_btns & BTN_ZL ? 0x80 : 0);
    hat_to_pro_buttons(hat, out_btns);
}

void apply_input_controls_to_pro21(const HIDReport& src, ProInputReport21& out) {
    out.conn_info = pro_conn_info_from_hid(src);
    out.vibrator = PRO_VIBRATOR_REPORT; map_buttons(src.input.buttons, src.input.hat, out.buttons);
    pack_stick_12(out.left_stick, src.input.lx, src.input.ly); pack_stick_12(out.right_stick, src.input.rx, src.input.ry);
}

void build_standard_report(const ns::HIDReport& src, const ns::MotionReport motion_samples[3], bool has_motion, bool imu_enabled, uint8_t timer, ProInputReport30& out, bool is_switch2) {
    memset(&out, 0, sizeof(out));
    out.id = is_switch2 ? 0x09 : RID_INPUT_STANDARD;  // S2 Pro uses 0x09 per research; start with common for others
    out.timer = timer;
    out.conn_info = pro_conn_info_from_hid(src);
    out.vibrator = PRO_VIBRATOR_REPORT;
    map_buttons(src.input.buttons, src.input.hat, out.buttons);
    pack_stick_12(out.left_stick, src.input.lx, src.input.ly); pack_stick_12(out.right_stick, src.input.rx, src.input.ry);
    MotionReport imu[3]{};
    if (imu_enabled && has_motion && motion_samples) std::memcpy(imu, motion_samples, sizeof(imu));
    out.accel_y_0 = imu[0].ax; out.accel_x_0 = imu[0].ay; out.accel_z_0 = imu[0].az;
    out.gyro_y_0  = imu[0].gx; out.gyro_x_0  = imu[0].gy; out.gyro_z_0  = imu[0].gz;
    out.accel_y_1 = imu[1].ax; out.accel_x_1 = imu[1].ay; out.accel_z_1 = imu[1].az;
    out.gyro_y_1  = imu[1].gx; out.gyro_x_1  = imu[1].gy; out.gyro_z_1  = imu[1].gz;
    out.accel_y_2 = imu[2].ax; out.accel_x_2 = imu[2].ay; out.accel_z_2 = imu[2].az;
    out.gyro_y_2  = imu[2].gx; out.gyro_x_2  = imu[2].gy; out.gyro_z_2  = imu[2].gz;
}

static uint8_t s2_power_info_from_hid(const HIDReport& src) {
    // S2 power info bitfield (research): [0] external, [1] charging,
    // [2:5] battery level 0-9, [6:7] reserved.
    // Without client battery info default to a full battery like the S1 path;
    // level 0 makes the console show a low-battery controller.
    uint8_t lvl = 9;
    if (src.reserved[1] & EXT_STATUS_BATTERY_VALID) {
        int pct = std::clamp<int>(src.reserved[0], 0, 100);
        lvl = static_cast<uint8_t>(std::min(9, pct / 11));
    }
    uint8_t pwr = static_cast<uint8_t>((lvl << 2) | 0x01);
    if (src.reserved[1] & EXT_STATUS_BATTERY_CHARGING) pwr |= 0x02;
    return pwr;
}

// Native Switch 2 motion is controller-specific. Pro Controller 2 uses a
// byte-aligned 30-byte block. Joy-Con 2 uses a 40-byte packed codec. The two
// formats share only the timing/temperature prefix and must not be conflated.
static void write_s2_pro_motion_block(uint8_t* out,
                                      size_t motion_len_index,
                                      size_t motion_data_index,
                                      const MotionReport motion_samples[3],
                                      bool has_motion,
                                      bool imu_enabled,
                                      int port) {
    if (!out || motion_data_index + 30u > PRO_REPORT_SIZE) return;
    if (!imu_enabled) {
        mark_s2_motion_inactive(port);
        return;
    }

    const S2AveragedMotion motion = average_s2_motion(motion_samples, has_motion);
    const int motion_port = s2_motion_port(port);

    std::lock_guard<std::mutex> lock(g_s2_motion_mtx);
    S2MotionState& state = g_s2_motion_state[motion_port];
    if (!state.active || state.controller_type != NS_TYPE_PRO) {
        reset_s2_state_locked(state, NS_TYPE_PRO);
        state.active = true;
    }

    uint8_t elapsed_ticks = 0;
    const uint16_t timing = advance_s2_motion_clock(state, elapsed_ticks);

    // The captured Pro Controller 2 block stores three wrapping 32-bit angular
    // phase accumulators. The observed scale is one full turn per 2^32 units.
    // MotionReport gyro values use the common S1 scale: 16.384 counts per
    // degree/second (5898.24 counts per full turn/second).
    const long double phase_scale =
        S2_PHASE_UNITS_PER_TURN /
        (S2_GYRO_COUNTS_PER_FULL_TURN_PER_SEC * S2_INTERNAL_IMU_HZ);
    for (size_t axis = 0; axis < 3; ++axis) {
        const int64_t increment = std::llround(
            static_cast<long double>(motion.gyro[axis]) * elapsed_ticks * phase_scale);
        state.angular_phase[axis] += static_cast<uint32_t>(increment);
    }

    out[motion_len_index] = 30;
    uint8_t* dst = out + motion_data_index;
    write_u16le(dst + 0x00, timing);
    write_i16le(dst + 0x02, S2_DEFAULT_TEMPERATURE);
    write_u32le(dst + 0x04, state.angular_phase[0]);
    write_u32le(dst + 0x08, state.angular_phase[1]);
    write_u32le(dst + 0x0C, state.angular_phase[2]);
    write_i32le(dst + 0x10, accel_q16(motion.accel[0]));
    write_i32le(dst + 0x14, accel_q16(motion.accel[1]));
    write_i32le(dst + 0x18, accel_q16(motion.accel[2]));
    // This field is real but unresolved. Zero is the dominant value in the
    // default capture and is safer than inventing a correction algorithm.
    write_i16le(dst + 0x1C, 0);
}

static void write_s2_joycon_motion_block(uint8_t* out,
                                         size_t motion_len_index,
                                         size_t motion_data_index,
                                         const MotionReport motion_samples[3],
                                         bool has_motion,
                                         bool imu_enabled,
                                         int port,
                                         bool right) {
    if (!out || motion_data_index + 40u > PRO_REPORT_SIZE) return;
    if (!imu_enabled) {
        mark_s2_motion_inactive(port);
        return;
    }

    const int motion_port = s2_motion_port(port);
    const uint8_t type = right ? NS_TYPE_JOYCON_R : NS_TYPE_JOYCON_L;

    std::lock_guard<std::mutex> lock(g_s2_motion_mtx);
    S2MotionState& state = g_s2_motion_state[motion_port];
    if (!state.active || state.controller_type != type) {
        reset_s2_state_locked(state, type);
        state.active = true;
    }

    uint8_t elapsed_ticks = 0;
    const uint16_t timing = advance_s2_motion_clock(state, elapsed_ticks);
    (void)elapsed_ticks;

    out[motion_len_index] = 40;
    uint8_t* dst = out + motion_data_index;
    write_u16le(dst + 0x00, timing);
    write_i16le(dst + 0x02, S2_DEFAULT_TEMPERATURE);
    std::memcpy(dst + 0x04, JOYCON2_STATIONARY_PAYLOAD.data(), JOYCON2_STATIONARY_PAYLOAD.size());
    uint8_t* payload = dst + 0x04;

    // Decoded native Joy-Con 2 layout (validated against synchronized
    // dual-controller grip captures and guided single-axis captures at
    // cross-sensor correlation 0.996-0.999; see
    // docs/switch2_native_motion_map_v2.md in the RE workspace):
    //
    //   bit   1        1 = normal rate, 0 = gyro at/near the +/-500 dps clamp
    //   bits 68..110   accel sample A (oldest)  3 x int14, 4096 counts/g
    //   bits 110..149  gyro  sample M (middle)  3 x int13, value = counts/2
    //   bits 149..188  accel sample B (middle)  3 x int13, value = counts/2
    //   bits 188..230  gyro  sample G (newest)  3 x int14, 16.4 counts/dps
    //   bits 230..272  accel sample C (newest)  3 x int14
    //
    // The three client MotionReport samples map naturally onto the three
    // sample slots (oldest / middle / newest). MotionReport already uses the
    // raw sensor units (4096 counts/g, 16.384 counts/dps), so values pass
    // through 1:1 apart from the half-resolution middle fields.
    const MotionReport* s = motion_samples;
    MotionReport zero{};
    MotionReport smp[3];
    if (has_motion && s) {
        smp[0] = s[0]; smp[1] = s[1]; smp[2] = s[2];
    } else {
        zero.az = 4096;  // 1 g face-up so a motionless client reads as resting
        smp[0] = zero; smp[1] = zero; smp[2] = zero;
    }

    const int16_t acc_a[3] = {smp[0].ax, smp[0].ay, smp[0].az};
    const int16_t acc_b[3] = {smp[1].ax, smp[1].ay, smp[1].az};
    const int16_t acc_c[3] = {smp[2].ax, smp[2].ay, smp[2].az};
    const int16_t gyr_m[3] = {smp[1].gx, smp[1].gy, smp[1].gz};
    const int16_t gyr_g[3] = {smp[2].gx, smp[2].gy, smp[2].gz};

    bool clipped = false;
    for (size_t axis = 0; axis < 3; ++axis) {
        // accel A (int14 @68), gyro M (int13 @110, counts/2),
        // accel B (int13 @149, counts/2), gyro G (int14 @188), accel C (int14 @230)
        put_bits_lsb(payload, 36, 68u + 14u * axis, 14,
                     pack_signed_field(acc_a[axis], 14));
        put_bits_lsb(payload, 36, 110u + 13u * axis, 13,
                     pack_signed_field(std::llround(gyr_m[axis] / 2.0), 13));
        put_bits_lsb(payload, 36, 149u + 13u * axis, 13,
                     pack_signed_field(std::llround(acc_b[axis] / 2.0), 13));
        put_bits_lsb(payload, 36, 188u + 14u * axis, 14,
                     pack_signed_field(gyr_g[axis], 14));
        put_bits_lsb(payload, 36, 230u + 14u * axis, 14,
                     pack_signed_field(acc_c[axis], 14));
        if (std::abs(static_cast<int64_t>(gyr_g[axis])) >= JOYCON2_GYRO_CLAMP_COUNTS ||
            std::abs(static_cast<int64_t>(gyr_m[axis])) >= JOYCON2_GYRO_CLAMP_COUNTS) {
            clipped = true;
        }
    }

    // bit 0 is always 1 in captures; bit 1 flags proximity to the gyro clamp.
    put_bits_lsb(payload, 36, 0, 1, 1u);
    put_bits_lsb(payload, 36, 1, 1, clipped ? 0u : 1u);
}

static void build_s2_joycon_report(const HIDReport& src,
                                   const MotionReport motion_samples[3],
                                   bool has_motion,
                                   bool imu_enabled,
                                   uint8_t timer,
                                   int port,
                                   uint8_t* out,
                                   bool right) {
    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = right ? 0x08 : 0x07;
    out[1] = timer;
    out[2] = s2_power_info_from_hid(src);

    const uint16_t btn = src.input.buttons;
    const uint8_t extra = static_cast<uint8_t>(src.input.vendor & EXT_BUTTON_MASK);
    const uint8_t hat = src.input.hat;
    if (right) {
        out[3] = (btn & BTN_RSTICK ? 0x80 : 0) | (btn & BTN_PLUS ? 0x40 : 0) |
                 (btn & BTN_ZR ? 0x20 : 0) | (btn & BTN_R ? 0x10 : 0) |
                 (btn & BTN_X ? 0x08 : 0) | (btn & BTN_Y ? 0x04 : 0) |
                 (btn & BTN_A ? 0x02 : 0) | (btn & BTN_B ? 0x01 : 0);
        // Expose SL/SR through the same physical shoulder pair so the normal
        // single Joy-Con registration gesture is available over USB.
        out[4] = (btn & BTN_ZR ? 0x80 : 0) | (btn & BTN_R ? 0x40 : 0) |
                 (extra & EXT_BUTTON_SL ? 0x80 : 0) | (extra & EXT_BUTTON_SR ? 0x40 : 0) |
                 (extra & EXT_BUTTON_C ? 0x10 : 0) | (btn & BTN_HOME ? 0x01 : 0);
        pack_stick_12(out + 6, src.input.rx, src.input.ry);
    } else {
        out[3] = (btn & BTN_LSTICK ? 0x80 : 0) | (btn & BTN_MINUS ? 0x40 : 0) |
                 (btn & BTN_ZL ? 0x20 : 0) | (btn & BTN_L ? 0x10 : 0) |
                 ((hat == HAT_N || hat == HAT_NE || hat == HAT_NW) ? 0x08 : 0) |
                 ((hat == HAT_W || hat == HAT_NW || hat == HAT_SW) ? 0x04 : 0) |
                 ((hat == HAT_E || hat == HAT_NE || hat == HAT_SE) ? 0x02 : 0) |
                 ((hat == HAT_S || hat == HAT_SE || hat == HAT_SW) ? 0x01 : 0);
        out[4] = (btn & BTN_ZL ? 0x80 : 0) | (btn & BTN_L ? 0x40 : 0) |
                 (extra & EXT_BUTTON_SL ? 0x80 : 0) | (extra & EXT_BUTTON_SR ? 0x40 : 0) |
                 (btn & BTN_CAPTURE ? 0x01 : 0);
        pack_stick_12(out + 6, src.input.lx, src.input.ly);
    }

    out[5] = 0x07; // observed constant for Joy-Con 2 report 0x07/0x08
    out[9] = 0x00; // unknown; mouse data at 0x0A..0x0E intentionally disabled for now
    out[15] = (right && controller_port_supports_amiibo(port) && is_amiibo_placed(port)) ? 0x01 : 0x00;
    write_s2_joycon_motion_block(out, 16, 17, motion_samples, has_motion, imu_enabled, port, right);
}

// S2 report builder. Pro Controller 2 uses report 0x09; Joy-Con 2 L/R use
// reports 0x07/0x08. The USB session/descriptor can remain Pro-like, but the
// logical device info/SPI/report stream follows the selected pad type.
void build_s2_pro_report(const HIDReport& src, const MotionReport motion_samples[3], bool has_motion, bool imu_enabled, uint8_t timer, int port, uint8_t* out) {
    const uint8_t ns_type = controller_type_for_port(port);
    if (ns_type == NS_TYPE_JOYCON_L || ns_type == NS_TYPE_JOYCON_R) {
        build_s2_joycon_report(src, motion_samples, has_motion, imu_enabled, timer, port, out, ns_type == NS_TYPE_JOYCON_R);
        return;
    }

    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = 0x09;
    out[1] = timer;
    out[2] = s2_power_info_from_hid(src);

    // Buttons per research hid_reports.md for 0x09.
    uint16_t btn = src.input.buttons;
    const uint8_t extra = static_cast<uint8_t>(src.input.vendor & EXT_BUTTON_MASK);
    uint8_t hat = src.input.hat;
    out[3] = (btn & BTN_RSTICK ? 0x80 : 0) | (btn & BTN_PLUS ? 0x40 : 0) |
             (btn & BTN_ZR ? 0x20 : 0) | (btn & BTN_R ? 0x10 : 0) |
             (btn & BTN_X ? 0x08 : 0) | (btn & BTN_Y ? 0x04 : 0) |
             (btn & BTN_A ? 0x02 : 0) | (btn & BTN_B ? 0x01 : 0);
    out[4] = (btn & BTN_LSTICK ? 0x80 : 0) | (btn & BTN_MINUS ? 0x40 : 0) |
             (btn & BTN_ZL ? 0x20 : 0) | (btn & BTN_L ? 0x10 : 0) |
             ((hat == HAT_N || hat == HAT_NE || hat == HAT_NW) ? 0x08 : 0) |
             ((hat == HAT_W || hat == HAT_NW || hat == HAT_SW) ? 0x04 : 0) |
             ((hat == HAT_E || hat == HAT_NE || hat == HAT_SE) ? 0x02 : 0) |
             ((hat == HAT_S || hat == HAT_SE || hat == HAT_SW) ? 0x01 : 0);
    out[5] = (btn & BTN_CAPTURE ? 0x02 : 0) | (btn & BTN_HOME ? 0x01 : 0) |
             (extra & EXT_BUTTON_GR ? 0x04 : 0) | (extra & EXT_BUTTON_GL ? 0x08 : 0) |
             (extra & EXT_BUTTON_C ? 0x10 : 0);

    pack_stick_12(out + 6, src.input.lx, src.input.ly);
    pack_stick_12(out + 9, src.input.rx, src.input.ry);

    out[12] = 0x30;
    out[13] = (controller_port_supports_amiibo(port) && is_amiibo_placed(port)) ? 0x01 : 0x00;
    out[14] = 0x00;
    write_s2_pro_motion_block(out, 15, 16, motion_samples, has_motion, imu_enabled, port);
}


int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply) {
    std::ranges::fill(reply->reply_data, 0); reply->ack = 0x80; reply->subcmd_id = subcmd;
    uint8_t proto = controller_protocol_type_for_port(rt.ctrl);
    bool is_s2 = (proto == ns::CONTROLLER_TYPE_PRO_S2 || proto == ns::CONTROLLER_TYPE_JOYCON_L_S2 || proto == ns::CONTROLLER_TYPE_JOYCON_R_S2 || proto == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2);
    if (is_s2) {
        return handle_s2_subcommand(rt, subcmd, cmd_data, reply);
    }
    switch (subcmd) {
    case CMD_BT_MANUAL_PAIRING:
        reply->ack = 0x81;
        if (controller_protocol_type_for_port(rt.ctrl) != ns::CONTROLLER_TYPE_PRO &&
            controller_protocol_type_for_port(rt.ctrl) != ns::CONTROLLER_TYPE_PRO_S2) {
            reply->reply_data[0] = 0x03; reply->reply_data[1] = 0x01;
            return 2;
        }
        if (!cmd_data.empty() && (cmd_data[0] == 0x02 || cmd_data[0] == 0x03)) { std::ranges::fill_n(reply->reply_data, 16, 0x00); return 16; }
        return 0;
    case CMD_TRIGGER_BUTTONS: reply->ack = 0x83; reply->reply_data[0] = 0x00; return 1;
    case CMD_SET_SHIP_MODE: return 0;
    case CMD_SET_IMU_SENS: return 0;
    case CMD_GET_DEVICE_INFO: {
        uint8_t info[36]; build_get_device_info_response(info, rt.ctrl); reply->ack = 0x82;
        std::ranges::copy(info, reply->reply_data); return 36;
    }
    case CMD_SET_DATA_FORMAT:
        rt.full_report_enabled = true;
        {
            uint8_t p = controller_protocol_type_for_port(rt.ctrl);
            if (p == ns::CONTROLLER_TYPE_PRO_S2) rt.input_report_mode = 0x09;
            else if (p == ns::CONTROLLER_TYPE_JOYCON_L_S2) rt.input_report_mode = 0x07;
            else if (p == ns::CONTROLLER_TYPE_JOYCON_R_S2) rt.input_report_mode = 0x08;
            else rt.input_report_mode = RID_INPUT_STANDARD;
        }
        return 0;
    case CMD_SPI_FLASH_READ: {
        if (cmd_data.size() < 5) { reply->ack = 0x00; return 0; }
        uint32_t addr = cmd_data[0] | (cmd_data[1] << 8) | (cmd_data[2] << 16) | (cmd_data[3] << 24);
        uint8_t size = std::min((size_t)cmd_data[4], (size_t)44); reply->ack = 0x90;
        std::memcpy(reply->reply_data, cmd_data.data(), 5);
        if (addr < SPI_FLASH_SIZE) {
            size_t to_copy = std::min((size_t)size, (size_t)(SPI_FLASH_SIZE - addr));
            std::memcpy(reply->reply_data + 5, g_spi_flash[rt.ctrl] + addr, to_copy);
            if (to_copy < size) std::memset(reply->reply_data + 5 + to_copy, 0xFF, size - to_copy);
        } else std::memset(reply->reply_data + 5, 0xFF, size);
        return 5 + size;
    }
    case CMD_SET_PLAYER_LIGHTS: case 0x33: return 0;
    case CMD_ENABLE_IMU: rt.imu_enabled = (cmd_data.empty() || cmd_data[0] != 0); return 0;
    case CMD_ENABLE_VIBRATION: rt.vibration_enabled = (cmd_data.empty() || cmd_data[0] != 0); return 0;
    default: return 0;
    }
}

// For S2, additional subcmds from research (commands.md)
// These are handled to return plausible responses for S2 console compatibility.
int handle_s2_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply) {
    // S2 specific or adjusted responses
    switch (subcmd) {
    case 0x01: // example initial or NFC
        reply->ack = 0x80;
        return 0;
    case 0x0A: // vibration or player
        reply->ack = 0x80;
        return 0;
    case 0x0D: // initialise USB from research
        reply->ack = 0x80;
        if (!cmd_data.empty()) {
            reply->reply_data[0] = 0x01;
        }
        return 1;
    // NFC subs from research for amiibo (kept for fallback; primary path for cmd 0x01 uses raw header responses)
    case 0x05: // Get status - signal scan requested if no tag (match pcap structure)
        reply->ack = 0x80;
        reply->reply_data[0] = 0x09;
        reply->reply_data[1] = 0x00;
        reply->reply_data[2] = 0x00;
        reply->reply_data[3] = 0x00;
        reply->reply_data[4] = 0x01;
        reply->reply_data[5] = 0x01;
        reply->reply_data[6] = 0x02;
        reply->reply_data[7] = 0x00;
        {
            std::lock_guard<std::mutex> lk(g_amiibo_mtx);
            const auto& data = g_amiibo_data[rt.ctrl];
            if (data.size() >= 8) {
                reply->reply_data[8] = 0x07;
                reply->reply_data[9] = 0x04;
                reply->reply_data[10] = data[0]; reply->reply_data[11] = data[1];
                reply->reply_data[12] = data[2]; reply->reply_data[13] = data[4];
                reply->reply_data[14] = data[5]; reply->reply_data[15] = data[6];
                reply->reply_data[16] = data[7];
            }
        }
        publish_amiibo_request_for_port(rt.ctrl, !is_amiibo_placed(rt.ctrl));
        return 32;
    case 0x06: // Read device
        reply->ack = 0x80;
        {
            std::lock_guard<std::mutex> lk(g_amiibo_mtx);
            if (g_amiibo_data[rt.ctrl].size() >= 8)
                std::memcpy(reply->reply_data, g_amiibo_data[rt.ctrl].data(), 8);
        }
        return 16;
    case 0x15: { // Read buffer - serve the amiibo bin
        reply->ack = 0x80;
        size_t to_copy = 0;
        std::lock_guard<std::mutex> lk(g_amiibo_mtx);
        if (!g_amiibo_data[rt.ctrl].empty() && cmd_data.size() >= 2) {
            uint16_t offset = cmd_data[0] | (cmd_data[1] << 8);
            to_copy = (offset < g_amiibo_data[rt.ctrl].size()) ? std::min(g_amiibo_data[rt.ctrl].size() - offset, (size_t)45) : 0;
            if (to_copy) std::memcpy(reply->reply_data + 4, g_amiibo_data[rt.ctrl].data() + offset, to_copy);
            reply->reply_data[0] = 0x00;
            reply->reply_data[1] = cmd_data[0];
            reply->reply_data[2] = cmd_data[1];
            reply->reply_data[3] = (uint8_t)to_copy;
        }
        return 4 + static_cast<int>(to_copy);
    }
    case 0x14: // Write buffer - write to bin data (accurate per PC2_Write_Amiibo.pcapng + research)
        reply->ack = 0x80;
        {
            std::lock_guard<std::mutex> lk(g_amiibo_mtx);
            if (!g_amiibo_data[rt.ctrl].empty() && cmd_data.size() >= 4) {
                // Format from pcap: after the 8-byte cmd header the data starts with
                //   [offset (LE, 2 bytes)] 00 [chunk_size-ish (2B)] + real NTAG data
                // e.g. 00 00 4c 00 <data for off 0>,   4c 00 4c 00 <data for off 0x4c>, etc.
                // Real data starts at +4; write it at the offset into our 540-byte image.
                uint16_t offset = cmd_data[0] | (cmd_data[1] << 8);
                size_t wstart = 4;
                size_t len = cmd_data.size() - wstart;
                if (offset + len > g_amiibo_data[rt.ctrl].size()) {
                    len = g_amiibo_data[rt.ctrl].size() > offset ? g_amiibo_data[rt.ctrl].size() - offset : 0;
                }
                if (len > 0) {
                    std::memcpy(g_amiibo_data[rt.ctrl].data() + offset, cmd_data.data() + wstart, len);
                    g_amiibo_modified[rt.ctrl] = true;
                }
            }
        }
        return 0;
    default:
        reply->ack = 0x80;
        return 0;
    }
}


bool rumble_half_is_all_zero(const uint8_t* f) { return f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0; }
bool rumble_half_is_neutral_carrier(const uint8_t* f) { return f[0] == 0x00 && f[1] == 0x01 && f[2] == 0x40 && f[3] == 0x40; }

// Native Switch 2 NFC command 0x01 responses.
//
// These replies travel over the vendor bulk endpoint, so they are not limited
// to the 64-byte HID report size. The observed 0x05 status response contains a
// 60-byte payload, and the observed 0x15 buffer response contains a 73-byte
// payload. Returning short, HID-sized approximations causes the console to
// abort the NFC transaction after it sees the tag UID.
namespace {
constexpr size_t S2_AMIIBO_RAW_SIZE = 540;
constexpr size_t S2_NFC_MAX_RESPONSE_PAYLOAD = 120;
constexpr size_t S2_NFC_READ_CHUNK_SIZE = 69;

// The controller's NFC staging buffer starts with a two-byte 0x07D0 prefix,
// followed by the raw NTAG215 image. This is visible in captured 0x14 Write
// Buffer traffic: [offset_le][length_le] d0 07 04 <UID...>.
std::vector<uint8_t> make_s2_nfc_staging_buffer(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> staging;
    staging.reserve(2 + S2_AMIIBO_RAW_SIZE);
    staging.push_back(0xD0);
    staging.push_back(0x07);
    staging.insert(staging.end(), raw.begin(), raw.end());
    if (staging.size() < 2 + S2_AMIIBO_RAW_SIZE)
        staging.resize(2 + S2_AMIIBO_RAW_SIZE, 0);
    return staging;
}
}

// Builds the payload after the standard eight-byte command-response header.
size_t fill_nfc_response_payload(uint8_t nfc_sub, std::span<const uint8_t> cmd_data, uint8_t* payload, int port) {
    if (port < 0 || port >= HID_PORT_COUNT) port = 0;
    std::memset(payload, 0, S2_NFC_MAX_RESPONSE_PAYLOAD);
    if (!controller_port_supports_amiibo(port)) {
        if (g_ctx.verbose) {
            std::println(stderr,
                         "[s2][nfc][state] t_us={} port={} sub=0x{:02x} rejected: controller has no native NFC request_data={}",
                         now_us(), port, nfc_sub, nfc_hex(cmd_data));
        }
        publish_amiibo_request_for_port(port, false);
        return 0;
    }

    std::unique_lock<std::mutex> amiibo_lk(g_amiibo_mtx);
    auto& adata = g_amiibo_data[port];
    const auto now = std::chrono::steady_clock::now();
    bool placed = adata.size() == S2_AMIIBO_RAW_SIZE && now < g_amiibo_expiry[port];

    if (g_ctx.verbose) {
        std::println("[s2][nfc][state] t_us={} port={} sub=0x{:02x} request_len={} request_data={} placed={} raw_size={} expiry_remaining_ms={} modified={}",
                     now_us(), port, nfc_sub, cmd_data.size(), nfc_hex(cmd_data), placed,
                     adata.size(), nfc_expiry_remaining_ms(port, now), g_amiibo_modified[port]);
    }

    // Keep the virtual tag present for the complete multi-command transaction.
    // Five seconds was occasionally short enough to race game/UI delays.
    if (placed) g_amiibo_expiry[port] = now + std::chrono::seconds(15);

    size_t plen = 0;
    int request_state = -1;

    switch (nfc_sub) {
    case 0x03: // Enter NFC scan mode.
        request_state = placed ? 0 : 1;
        if (g_ctx.verbose)
            std::println("[s2][nfc][parse] sub=0x03 enter-scan placed={} ui_scan_requested={}",
                         placed, request_state == 1);
        break;

    case 0x04: // Leave NFC scan mode.
        request_state = 0;
        if (g_ctx.verbose)
            std::println("[s2][nfc][parse] sub=0x04 leave-scan ui_scan_requested=false");
        break;

    case 0x05: { // Get status.
        // Captured response payload is exactly 60 bytes:
        // 09 00 00 00 01 01 02 00 07 04 <7-byte UID> <zero padding>.
        payload[0] = 0x09;
        payload[4] = 0x01;
        payload[5] = 0x01;
        payload[6] = 0x02;
        if (placed) {
            // In the captured response, byte 8 is the UID length and the
            // seven UID bytes begin immediately at byte 9. 0x04 is UID0
            // (the NXP manufacturer byte), not a separate tag-type field.
            // The previous implementation inserted an extra 0x04 before
            // the UID, shifting/truncating the identity seen by the console.
            payload[8] = 0x07;
            // Raw NTAG215 page 0 stores UID0..2, BCC0, UID3..6.
            payload[9]  = adata[0];
            payload[10] = adata[1];
            payload[11] = adata[2];
            payload[12] = adata[4];
            payload[13] = adata[5];
            payload[14] = adata[6];
            payload[15] = adata[7];
        }
        request_state = placed ? 0 : 1;
        plen = 60;
        if (g_ctx.verbose) {
            if (placed) {
                std::println("[s2][nfc][parse] sub=0x05 get-status tag_present=true uid={:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x} uid_offset=9 payload_len={}",
                             adata[0], adata[1], adata[2], adata[4], adata[5], adata[6], adata[7], plen);
            } else {
                std::println("[s2][nfc][parse] sub=0x05 get-status tag_present=false payload_len={} ui_scan_requested=true", plen);
            }
        }
        break;
    }

    case 0x06: // Read device: starts the read, response is header-only.
    case 0x08: // Write device: observed as a header-only ACK.
        plen = 0;
        if (g_ctx.verbose)
            std::println("[s2][nfc][parse] sub=0x{:02x} {} header-only-ack placed={}",
                         nfc_sub, nfc_sub == 0x06 ? "read-device" : "write-device", placed);
        break;

    case 0x15: { // Read buffer.
        if (placed && cmd_data.size() >= 2) {
            const uint16_t off = static_cast<uint16_t>(cmd_data[0])
                               | (static_cast<uint16_t>(cmd_data[1]) << 8);
            const std::vector<uint8_t> staging = make_s2_nfc_staging_buffer(adata);

            // Captured response prefix for request offset 0x0046 is
            //   00 46 00 0f
            // followed by 69 bytes of staging-buffer data. Keep that exact
            // framing and pad the final chunk with zeroes.
            payload[0] = 0x00;
            payload[1] = static_cast<uint8_t>(off);
            payload[2] = static_cast<uint8_t>(off >> 8);
            const size_t remaining = off < staging.size() ? staging.size() - off : 0;
            payload[3] = static_cast<uint8_t>(std::min<size_t>(0xFF, (remaining + 31) / 32));

            if (remaining != 0) {
                const size_t to_copy = std::min(remaining, S2_NFC_READ_CHUNK_SIZE);
                std::memcpy(payload + 4, staging.data() + off, to_copy);
                if (g_ctx.verbose) {
                    std::println("[s2][nfc][parse] sub=0x15 read-buffer offset=0x{:04x} staging_size={} remaining={} copied={} padded={} descriptor=0x{:02x}",
                                 off, staging.size(), remaining, to_copy,
                                 S2_NFC_READ_CHUNK_SIZE - to_copy, payload[3]);
                    std::println("[s2][nfc][buffer] read_data={}",
                                 nfc_hex(std::span<const uint8_t>(payload + 4, S2_NFC_READ_CHUNK_SIZE)));
                }
            } else if (g_ctx.verbose) {
                std::println("[s2][nfc][parse] sub=0x15 read-buffer offset=0x{:04x} out_of_range=true staging_size={} copied=0 descriptor=0x{:02x}",
                             off, staging.size(), payload[3]);
            }
            plen = 4 + S2_NFC_READ_CHUNK_SIZE;
        } else if (g_ctx.verbose) {
            std::println(stderr,
                         "[s2][nfc][parse] sub=0x15 read-buffer cannot-serve placed={} request_len={} required_len=2",
                         placed, cmd_data.size());
        }
        break;
    }

    case 0x14: { // Write buffer: [offset LE][length LE] + staging data.
        if (placed && cmd_data.size() >= 4) {
            const uint16_t off = static_cast<uint16_t>(cmd_data[0])
                               | (static_cast<uint16_t>(cmd_data[1]) << 8);
            const uint16_t declared = static_cast<uint16_t>(cmd_data[2])
                                    | (static_cast<uint16_t>(cmd_data[3]) << 8);
            const size_t available = cmd_data.size() - 4;
            const size_t incoming = std::min<size_t>(declared, available);

            std::vector<uint8_t> staging = make_s2_nfc_staging_buffer(adata);
            size_t copied = 0;
            if (off < staging.size() && incoming != 0) {
                const size_t to_copy = std::min(incoming, staging.size() - off);
                std::memcpy(staging.data() + off, cmd_data.data() + 4, to_copy);
                copied = to_copy;

                // Staging bytes 0..1 are transport metadata. Persist only the
                // raw 540-byte NTAG215 image back to the selected .bin.
                std::copy_n(staging.begin() + 2, S2_AMIIBO_RAW_SIZE, adata.begin());
                g_amiibo_modified[port] = true;
            }
            if (g_ctx.verbose) {
                std::println("[s2][nfc][parse] sub=0x14 write-buffer offset=0x{:04x} declared={} available={} accepted={} copied={} truncated={} staging_size={} modified={}",
                             off, declared, available, incoming, copied, incoming - copied,
                             staging.size(), g_amiibo_modified[port]);
                std::println("[s2][nfc][buffer] write_data={}",
                             nfc_hex(cmd_data.subspan(4, incoming)));
            }
        } else if (g_ctx.verbose) {
            std::println(stderr,
                         "[s2][nfc][parse] sub=0x14 write-buffer cannot-serve placed={} request_len={} required_len=4",
                         placed, cmd_data.size());
        }
        plen = 0;
        break;
    }

    default:
        if (g_ctx.verbose)
            std::println(stderr, "[s2][nfc][parse] unknown NFC subcommand 0x{:02x}", nfc_sub);
        break;
    }

    const bool modified_after = g_amiibo_modified[port];
    const long long expiry_after_ms = nfc_expiry_remaining_ms(port, std::chrono::steady_clock::now());
    amiibo_lk.unlock();
    if (request_state >= 0) publish_amiibo_request_for_port(port, request_state != 0);
    if (g_ctx.verbose) {
        std::println("[s2][nfc][state] completed sub=0x{:02x} response_payload_len={} ui_state_action={} expiry_remaining_ms={} modified={}",
                     nfc_sub, plen,
                     request_state < 0 ? "none" : (request_state != 0 ? "request-scan" : "stop-scan"),
                     expiry_after_ms, modified_after);
    }
    return plen;
}

struct DecodedPrecisionRumbleHalf { uint8_t low = 0; uint8_t high = 0; };

uint8_t rumble_scale_capture_delta(int v) {
    v = (v * RUMBLE_GAIN_PERCENT) / 100;
    return (uint8_t)std::clamp(v > 0 ? std::max((int)RUMBLE_MIN_NONZERO, v) : 0, 0, 255);
}

DecodedPrecisionRumbleHalf rumble_decode_half_precision_to_dual(const uint8_t* f) {
    DecodedPrecisionRumbleHalf out{};
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f)) return out;
    int high_delta = std::abs((int)(f[1] & 0x7F) - 0x01), low_delta = std::abs((int)(f[3] & 0x7F) - 0x40);
    out.high = rumble_scale_capture_delta(high_delta * 3 + std::abs((int)f[0]) / 3);
    out.low  = rumble_scale_capture_delta(low_delta * 3 + std::abs((int)(f[2] & 0x7F) - 0x40) / 2);
    return out;
}

uint8_t rumble_decode_half_to_u8(const uint8_t* f) {
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f)) return 0;
    static const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};
    int max_diff = 0, sum_diff = 0;
    for (int i = 0; i < 4; ++i) {
        int d = std::abs((int)f[i] - neutral[i]);
        max_diff = std::max(max_diff, d); sum_diff += d;
    }
    int strength = (max_diff * 2 + sum_diff / 4) * RUMBLE_GAIN_PERCENT / 100;
    return (uint8_t)std::clamp(strength > 0 ? std::max((int)RUMBLE_MIN_NONZERO, strength) : 0, 0, 255);
}

static void publish_decoded_rumble_event(int client_idx, int sub_idx, uint8_t low, uint8_t high,
                                         bool publish_neutral, const uint8_t* precision = nullptr) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return;
    bool neutral = (low == 0 && high == 0);
    if (neutral && !publish_neutral) return;

    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    if (neutral && !g_ctx.clients[client_idx].rumble_active[sub_idx]) return;

    RumblePacket& ev = g_ctx.clients[client_idx].rumble[sub_idx];
    ev.magic = RUMBLE_MAGIC; ev.subpad = (uint8_t)sub_idx;
    ev.low_freq = neutral ? 0 : low; ev.high_freq = neutral ? 0 : high; ev.duration_10ms = neutral ? 0 : 1;

    PrecisionRumblePacket& precision_ev = g_ctx.clients[client_idx].precision_rumble[sub_idx];
    precision_ev.magic = PRECISION_RUMBLE_MAGIC; precision_ev.subpad = (uint8_t)sub_idx;
    precision_ev.low_freq = ev.low_freq; precision_ev.high_freq = ev.high_freq; precision_ev.duration_10ms = ev.duration_10ms;
    if (precision) memcpy(precision_ev.precision, precision, sizeof(precision_ev.precision));
    else std::ranges::fill(precision_ev.precision, 0);

    g_ctx.clients[client_idx].rumble_active[sub_idx] = !neutral;
    g_ctx.clients[client_idx].rumble_seq[sub_idx]++;
}

static void publish_rumble_event_from_bytes(int client_idx, int sub_idx, const uint8_t* rb, bool publish_neutral) {
    if (!rb) return;
    DecodedPrecisionRumbleHalf left = rumble_decode_half_precision_to_dual(rb), right = rumble_decode_half_precision_to_dual(rb + 4);
    uint8_t low = std::max(left.low, right.low), high = std::max(left.high, right.high);

    if (low == 0 && high == 0 && !(rumble_half_is_all_zero(rb) || rumble_half_is_neutral_carrier(rb)) && !(rumble_half_is_all_zero(rb + 4) || rumble_half_is_neutral_carrier(rb + 4))) {
        low = rumble_decode_half_to_u8(rb); high = rumble_decode_half_to_u8(rb + 4);
    }
    publish_decoded_rumble_event(client_idx, sub_idx, low, high, publish_neutral, rb);
}

void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (len < 10 || !packet) return;
    publish_rumble_event_from_bytes(client_idx, sub_idx, packet + 2, publish_neutral);
}

void publish_s2_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (!packet || len < 7 || (packet[0] != 0x01 && packet[0] != 0x02)) return;

    // S2 LRA data is not the Switch 1 four-byte HD-rumble encoding.  Each 16-byte
    // motor block begins with a counter byte followed by a 40-bit little-endian
    // value: frequency0(10) | amplitude0(10) | frequency1(10) | amplitude1(10).
    // Frequency fields remain non-zero at rest, so decoding them as intensity causes
    // a persistent idle buzz.  Use the peak of the two amplitude fields instead.
    const auto motor_amplitude = [](const uint8_t* p) -> uint8_t {
        const uint64_t packed = (uint64_t)p[0] | ((uint64_t)p[1] << 8)
            | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32);
        const uint16_t amplitude = std::max<uint16_t>((packed >> 10) & 0x3FF, (packed >> 30) & 0x3FF);
        return rumble_scale_capture_delta(amplitude >> 2); // 10-bit amplitude -> SDL's 8-bit range
    };

    const uint8_t left = motor_amplitude(packet + 2); // skip report ID and LRA counter
    // Pro Controller 2 has a second 16-byte LRA block; a Joy-Con 2 has only its
    // one actuator, so feed that amplitude to both SDL channels.
    const uint8_t right = (packet[0] == 0x02 && len >= 23) ? motor_amplitude(packet + 18) : left;
    publish_decoded_rumble_event(client_idx, sub_idx, left, right, publish_neutral);
}
