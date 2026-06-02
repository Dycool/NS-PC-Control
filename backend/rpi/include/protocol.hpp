#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// protocol.hpp - Shared definitions for PC frontend and Raspberry Pi backend
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <chrono>

namespace ns {

// ── Tuning & Constants ────────────────────────────────────────────────────────
static constexpr uint32_t PROTO_MAGIC     = 0x4E535743u; // 'NSWC'
static constexpr uint8_t  PROTO_VERSION   = 2;
static constexpr uint16_t DEFAULT_PORT    = 7331;
static constexpr int      HEARTBEAT_MS    = 50;
static constexpr int      WATCHDOG_MS     = 1200;
static constexpr int      WRITER_HZ       = 250;
static constexpr int      AUTOFIRE_HZ     = 12;
static constexpr int      MAX_CONTROLLERS = 7; // Hardware limit for dwc2 USB OTG

// ── Switch Pro Controller Button Bitmask ──────────────────────────────────────
enum Button : uint16_t {
    BTN_Y       = 1u <<  0, BTN_B       = 1u <<  1, BTN_A       = 1u <<  2, BTN_X       = 1u <<  3,
    BTN_L       = 1u <<  4, BTN_R       = 1u <<  5, BTN_ZL      = 1u <<  6, BTN_ZR      = 1u <<  7,
    BTN_MINUS   = 1u <<  8, BTN_PLUS    = 1u <<  9, BTN_LSTICK  = 1u << 10, BTN_RSTICK  = 1u << 11,
    BTN_HOME    = 1u << 12, BTN_CAPTURE = 1u << 13,
};

// ── D-pad HAT Switch Values ───────────────────────────────────────────────────
enum Hat : uint8_t {
    HAT_N  = 0, HAT_NE = 1, HAT_E  = 2, HAT_SE = 3,
    HAT_S  = 4, HAT_SW = 5, HAT_W  = 6, HAT_NW = 7,
    HAT_NEUTRAL = 8,
};

// ── 8-byte HID Input Report ───────────────────────────────────────────────────
// Layout MUST exactly match the USB gadget HID descriptor.
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = HAT_NEUTRAL;
    uint8_t  lx      = 128; // 128 = center
    uint8_t  ly      = 128;
    uint8_t  rx      = 128;
    uint8_t  ry      = 128;
    uint8_t  vendor  = 0;

    void reset() noexcept { *this = HIDReport{}; }

    bool operator==(const HIDReport& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
    bool operator!=(const HIDReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));

static_assert(sizeof(HIDReport) == 8, "HIDReport must be 8 bytes");

// ── Packet Flags ──────────────────────────────────────────────────────────────
enum Flags : uint8_t {
    FLAG_NONE     = 0x00,
    FLAG_RESET    = 0x01, // Backend should zero all inputs for this controller
    FLAG_AUTOFIRE = 0x02, // autofire_mask is active
};

// ── UDP Wire Packet (Frontend -> Backend) ─────────────────────────────────────
struct Packet {
    uint32_t  magic;
    uint8_t   version;
    uint8_t   flags;
    uint8_t   controller_id; // 0 to MAX_CONTROLLERS - 1
    uint8_t   pad;           // Memory alignment padding
    uint16_t  autofire_mask;
    uint32_t  seq;
    uint64_t  ts_us;
    HIDReport report;
} __attribute__((packed));

static constexpr std::size_t PACKET_SIZE = sizeof(Packet);

// ── Utilities ─────────────────────────────────────────────────────────────────
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline bool packet_ok(const Packet& p) noexcept {
    return p.magic == PROTO_MAGIC && 
           p.version == PROTO_VERSION && 
           p.controller_id < MAX_CONTROLLERS;
}

} // namespace ns