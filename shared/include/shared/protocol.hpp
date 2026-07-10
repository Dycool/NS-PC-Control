#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  protocol.hpp  –  Shared between frontend clients and backend servers
//
//  Goals:
//    - Keep legacy 8-byte reports byte-compatible: HIDReport == 8 bytes.
//    - Keep legacy UDP packets byte-compatible: Packet == 68 bytes.
//    - Provide modern optional motion/rumble structs for 64-byte report servers
//      and upgraded UDP/WebSocket clients without breaking old clients.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <chrono>

// Portable packing: GCC/Clang use __attribute__, MSVC uses #pragma pack.
#ifdef _MSC_VER
  #define NS_PACKED_ATTR
  __pragma(pack(push, 1))
#else
  #define NS_PACKED_ATTR __attribute__((packed))
#endif

namespace ns {

// ── Tuning constants ─────────────────────────────────────────────────────────
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u; // 'NSWC'
static constexpr uint8_t  PROTO_VERSION = 4;           // Legacy 4-player UDP packet
static constexpr uint8_t  WEB_PROTO_VERSION = 5;       // Extended input + optional motion
static constexpr uint8_t  WEB_PROTO_VERSION_3 = 6;     // Extended input + 3 motion samples per pad
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr int      LEGACY_UDP_HZ = 250;
static constexpr int      PRO_UDP_HZ    = 250;
static constexpr int      LEGACY_UDP_INTERVAL_MS = 4;
static constexpr int      PRO_UDP_INTERVAL_MS  = 4;

static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr std::size_t HMAC_TAG_SIZE = 16;

static constexpr uint32_t RUMBLE_MAGIC = 0x4E535652u; // 'NSVR'
static constexpr uint32_t PRECISION_RUMBLE_MAGIC = 0x4E535648u; // 'NSVH'
static constexpr uint32_t CONTROLLER_STATUS_MAGIC = 0x4E534353u; // 'NSCS'
static constexpr uint32_t CLIENT_ASSIGNMENT_MAGIC = 0x4E534341u; // 'NSCA' server -> client slot/console-port assignment
static constexpr uint32_t SERVER_INFO_MAGIC = 0x4E535349u; // 'NSSI'
static constexpr uint32_t CLIENT_NAMES_MAGIC = 0x4E53434Eu; // 'NSCN'
static constexpr uint32_t ROSTER_MAGIC = 0x4E53524Fu; // 'NSRO'
static constexpr uint8_t  SERVER_INFO_VERSION = 1;
static constexpr uint8_t  CLIENT_ASSIGNMENT_VERSION = 1;

enum ServerBackend : uint8_t {
    SERVER_BACKEND_UNKNOWN = 0,
    SERVER_BACKEND_LEGACY  = 1,
    SERVER_BACKEND_PRO     = 2,
};

static constexpr uint8_t SERVER_INFO_FLAG_SWITCH_ASLEEP = 1u << 0;
static constexpr uint8_t SERVER_INFO_FLAG_SERVER_FULL   = 1u << 1;
// Server-selected USB identity. Clients use this to request the matching
// Switch 2 protocol variants and to expose Switch 2-only UI such as Amiibo.
static constexpr uint8_t SERVER_INFO_FLAG_SWITCH2_MODE  = 1u << 2;
static constexpr uint8_t SERVER_INFO_FLAG_HORI_MODE     = 1u << 3;

// ── Buttons / hats / flags ───────────────────────────────────────────────────
enum Button : uint16_t {
    BTN_Y       = 1u <<  0,
    BTN_B       = 1u <<  1,
    BTN_A       = 1u <<  2,
    BTN_X       = 1u <<  3,
    BTN_L       = 1u <<  4,
    BTN_R       = 1u <<  5,
    BTN_ZL      = 1u <<  6,
    BTN_ZR      = 1u <<  7,
    BTN_MINUS   = 1u <<  8,
    BTN_PLUS    = 1u <<  9,
    BTN_LSTICK  = 1u << 10,
    BTN_RSTICK  = 1u << 11,
    BTN_HOME    = 1u << 12,
    BTN_CAPTURE = 1u << 13,
};

enum Hat : uint8_t {
    HAT_N  = 0,
    HAT_NE = 1,
    HAT_E  = 2,
    HAT_SE = 3,
    HAT_S  = 4,
    HAT_SW = 5,
    HAT_W  = 6,
    HAT_NW = 7,
    HAT_NEUTRAL = 8,
};

enum Flags : uint8_t {
    FLAG_NONE       = 0x00,
    FLAG_RESET      = 0x01, // Backend should zero inputs immediately.
    FLAG_AUTOFIRE   = 0x02, // Autofire mask is active.
    FLAG_SINGLE_PAD = 0x04, // Web/mobile packet should only claim pad 1.
    FLAG_DISCONNECT = 0x08, // Authenticated UDP client is leaving; free its slot now.
};

// Extended clients set these in HoriHIDReport::vendor inside HIDReport.
// Server code clears vendor before writing legacy 8-byte reports. Keeping the
// extra controls here preserves the established 8-byte input report and its
// 16-bit Switch 1 button field.
static constexpr uint8_t EXT_PAD_PRESENT = 0x01;
static constexpr uint8_t EXT_BUTTON_C     = 0x02;
static constexpr uint8_t EXT_BUTTON_GL    = 0x04;
static constexpr uint8_t EXT_BUTTON_GR    = 0x08;
static constexpr uint8_t EXT_BUTTON_SL    = 0x10;
static constexpr uint8_t EXT_BUTTON_SR    = 0x20;
static constexpr uint8_t EXT_BUTTON_MASK  = EXT_BUTTON_C | EXT_BUTTON_GL | EXT_BUTTON_GR
                                          | EXT_BUTTON_SL | EXT_BUTTON_SR;
// Extended HIDReport::reserved[1] status flags. Older clients leave these zero.
static constexpr uint8_t EXT_STATUS_BATTERY_VALID = 0x01;
static constexpr uint8_t EXT_STATUS_BATTERY_CHARGING = 0x02;
static constexpr uint8_t EXT_STATUS_BATTERY_PERCENT_UNKNOWN = 0xFF;
// HIDReport::reserved[2]. Zero is treated as Pro for older clients.
enum ControllerType : uint8_t {
    CONTROLLER_TYPE_DEFAULT  = 0,
    CONTROLLER_TYPE_JOYCON_L = 1,
    CONTROLLER_TYPE_JOYCON_R    = 2,
    CONTROLLER_TYPE_PRO         = 3,
    // Requested profile: one source pad should occupy two server-side virtual
    // slots, exposed to the Switch as Joy-Con L + Joy-Con R. The client sends
    // this value for the physical/source pad; the server owns the L/R expansion.
    CONTROLLER_TYPE_JOYCON_PAIR = 4,
    CONTROLLER_TYPE_HORI        = 5,
    // Switch 2 variants (when g_switch2ModeEnabled on client)
    CONTROLLER_TYPE_PRO_S2         = 6,
    CONTROLLER_TYPE_JOYCON_L_S2    = 7,
    CONTROLLER_TYPE_JOYCON_R_S2    = 8,
    CONTROLLER_TYPE_JOYCON_PAIR_S2 = 9,
};
static constexpr std::size_t ROSTER_NAME_CAP = 48;

static constexpr uint8_t CONTROLLER_PLAYER_INDEX_UNKNOWN = 0xFF;
static constexpr uint8_t CONTROLLER_CONSOLE_PORT_NONE = 0xFF;
static constexpr uint8_t CONTROLLER_STATUS_FLAG_BODY_RGB_VALID = 0x01;
static constexpr uint8_t CLIENT_ASSIGNMENT_FLAG_ACCEPTED         = 0x01;
static constexpr uint8_t CLIENT_ASSIGNMENT_FLAG_SERVER_FULL      = 0x02;
static constexpr uint8_t CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP    = 0x04;
static constexpr uint8_t CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID = 0x08;
static constexpr uint8_t CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED = 0x10;


// ── Legacy input reports ─────────────────────────────────────────────────────
// Exactly 8 bytes. This is the complete HID report written to the old
// legacy 8-byte gadget endpoints.
struct HoriHIDReport {
    uint16_t buttons = 0;
    uint8_t  hat = HAT_NEUTRAL;
    uint8_t  lx = 128;
    uint8_t  ly = 128;
    uint8_t  rx = 128;
    uint8_t  ry = 128;
    uint8_t  vendor = 0;

    void reset() noexcept {
        buttons = 0;
        hat = HAT_NEUTRAL;
        lx = 128; ly = 128; rx = 128; ry = 128;
        vendor = 0;
    }

    bool operator==(const HoriHIDReport&) const = default;
} NS_PACKED_ATTR;


// ── Optional motion / modern reports ─────────────────────────────────────────
struct MotionReport {
    int16_t ax = 0, ay = 0, az = 0;
    int16_t gx = 0, gy = 0, gz = 0;

    void reset() noexcept {
        ax = ay = az = 0;
        gx = gy = gz = 0;
    }
} NS_PACKED_ATTR;

struct HIDReport {
    HoriHIDReport input{};        // 8 bytes; input.vendor bit 0 = EXT_PAD_PRESENT.
    MotionReport motion[3]{}; // 3x 12 bytes, oldest -> newest.
    uint8_t has_motion = 0;
    uint8_t reserved[3]{};

    void reset() noexcept {
        input.reset();
        for (auto& m : motion) m.reset();
        has_motion = 0;
        reserved[0] = reserved[1] = reserved[2] = 0;
    }
} NS_PACKED_ATTR;

struct MultiReport {
    HIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
} NS_PACKED_ATTR;

struct RumblePacket {
    uint32_t magic = RUMBLE_MAGIC;
    uint8_t  subpad = 0;        // 0..3 logical pad inside the client.
    uint8_t  low_freq = 0;      // 0..255
    uint8_t  high_freq = 0;     // 0..255
    uint8_t  duration_10ms = 0; // Duration units used by the web/UDP clients.
} NS_PACKED_ATTR;

struct PrecisionRumblePacket {
    uint32_t magic = PRECISION_RUMBLE_MAGIC;
    uint8_t  subpad = 0;        // 0..3 logical pad inside the client.
    uint8_t  low_freq = 0;      // Classic fallback weak/low motor, 0..255.
    uint8_t  high_freq = 0;     // Classic fallback strong/high motor, 0..255.
    uint8_t  duration_10ms = 0; // Classic fallback duration.
    uint8_t  precision[8]{};           // Raw precision rumble bytes: left[4], right[4].
    uint8_t  reserved[4]{};
} NS_PACKED_ATTR;



struct ServerInfoProbe {
    uint32_t magic = SERVER_INFO_MAGIC;
    uint8_t  version = SERVER_INFO_VERSION;
    uint8_t  reserved[3]{};
} NS_PACKED_ATTR;

struct ServerInfoReply {
    uint32_t magic = SERVER_INFO_MAGIC;
    uint8_t  version = SERVER_INFO_VERSION;
    uint8_t  backend = SERVER_BACKEND_UNKNOWN;
    uint16_t udp_interval_ms = PRO_UDP_INTERVAL_MS;
    uint16_t udp_hz = PRO_UDP_HZ;
    uint8_t  reserved[6]{};
} NS_PACKED_ATTR;


struct ClientAssignmentPacket {
    uint32_t magic = CLIENT_ASSIGNMENT_MAGIC;
    uint8_t  version = CLIENT_ASSIGNMENT_VERSION;
    uint8_t  flags = 0;
    uint8_t  server_slot = CONTROLLER_PLAYER_INDEX_UNKNOWN; // 0..3 client session slot, or 0xFF when refused/unknown.
    uint8_t  subpad = 0;       // 0..3 logical/source pad inside this client.
    uint8_t  console_port_mask = 0; // bit0..bit3 = Switch USB virtual ports occupied by this source pad.
    uint8_t  primary_console_port = CONTROLLER_CONSOLE_PORT_NONE; // 0..3, or 0xFF unmapped.
    uint8_t  requested_type = CONTROLLER_TYPE_DEFAULT;
    uint8_t  virtual_type = CONTROLLER_TYPE_DEFAULT;
    uint8_t  active_clients = 0;
    uint8_t  max_clients = 4;
    uint8_t  free_virtual_slots = 0;
    uint8_t  reserved = 0;
} NS_PACKED_ATTR;

struct ControllerStatusPacket {
    uint32_t magic = CONTROLLER_STATUS_MAGIC;
    uint8_t  version = SERVER_INFO_VERSION;
    uint8_t  subpad = 0;          // 0..3 logical pad inside the client.
    uint8_t  player_index = CONTROLLER_PLAYER_INDEX_UNKNOWN; // 0..3, or 0xFF unknown/off.
    uint8_t  player_leds = 0;     // Raw Switch subcommand 0x30 LED bitfield.
    // reserved[0..2]: virtual Pro Controller body RGB for RGB/lightbar controllers.
    // reserved[3]: CONTROLLER_STATUS_FLAG_* bitfield.
    uint8_t  reserved[4]{};
} NS_PACKED_ATTR;

struct RosterEntry {
    uint8_t present = 0;
    uint8_t has_gyro = 0;
    char    name[ROSTER_NAME_CAP]{};
} NS_PACKED_ATTR;

struct ClientNamesPacket {
    uint32_t   magic = CLIENT_NAMES_MAGIC;
    uint8_t    version = SERVER_INFO_VERSION;
    uint8_t    reserved[3]{};
    RosterEntry pads[4];
    uint8_t    hmac[HMAC_TAG_SIZE]{};
} NS_PACKED_ATTR;

struct RosterPacket {
    uint32_t   magic = ROSTER_MAGIC;
    uint8_t    version = SERVER_INFO_VERSION;
    uint8_t    reserved[3]{};
    RosterEntry ports[4];
} NS_PACKED_ATTR;

static constexpr uint32_t AMIIBO_REQUEST_MAGIC = 0x4E534152u; // 'NSAR'
struct AmiiboRequestPacket {
    uint32_t magic = AMIIBO_REQUEST_MAGIC;
    uint8_t subpad = 0;
    uint8_t requested = 0; // 1 = console wants Amiibo scan
    uint8_t reserved[2]{};
} NS_PACKED_ATTR;

static constexpr uint32_t AMIIBO_DATA_MAGIC = 0x4E534144u; // 'NSAD'
struct AmiiboDataPacket {
    uint32_t magic = AMIIBO_DATA_MAGIC;
    uint8_t subpad = 0;
    uint16_t data_len = 0;
    uint8_t data[540]{}; // NTAG215 Amiibo data
} NS_PACKED_ATTR;
// ── Unified UDP packet ───────────────────────────────────────────────────────
struct Packet {
    uint32_t    magic = PROTO_MAGIC;
    uint8_t     version = WEB_PROTO_VERSION;
    uint8_t     flags = FLAG_NONE;
    uint16_t    reserved = 0;
    uint32_t    seq = 0;
    uint64_t    ts_us = 0;
    MultiReport report{};
    uint8_t     hmac[HMAC_TAG_SIZE]{};
} NS_PACKED_ATTR;

static constexpr std::size_t PACKET_SIZE      = sizeof(Packet);
static constexpr std::size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

static constexpr std::size_t REPORT_SIZE   = sizeof(HIDReport);
static constexpr std::size_t WEB_PACKET_SIZE   = PACKET_AUTH_SIZE;
static constexpr std::size_t CLIENT_NAMES_AUTH_SIZE = sizeof(ClientNamesPacket) - HMAC_TAG_SIZE;

// ── Hard wire-layout checks ──────────────────────────────────────────────────
static_assert(sizeof(HoriHIDReport) == 8,
              "HoriHIDReport must stay 8 bytes for legacy gadget reports");
static_assert(sizeof(MotionReport) == 12,
              "MotionReport wire layout changed");
static_assert(sizeof(HIDReport) == 48,
              "HIDReport wire layout changed");
static_assert(sizeof(MultiReport) == 192,
              "MultiReport wire layout changed");
static_assert(sizeof(Packet) == 228,
              "Packet wire layout changed");
static_assert(sizeof(RumblePacket) == 8,
              "RumblePacket wire layout changed");
static_assert(sizeof(PrecisionRumblePacket) == 20,
              "PrecisionRumblePacket wire layout changed");
static_assert(sizeof(ClientAssignmentPacket) == 16,
              "ClientAssignmentPacket wire layout changed");
static_assert(sizeof(ControllerStatusPacket) == 12,
              "ControllerStatusPacket wire layout changed");
static_assert(sizeof(RosterEntry) == 50,
              "RosterEntry wire layout changed");
static_assert(sizeof(ClientNamesPacket) == 224,
              "ClientNamesPacket wire layout changed");
static_assert(sizeof(RosterPacket) == 208,
              "RosterPacket wire layout changed");

static_assert(sizeof(ServerInfoProbe) == 8,
              "ServerInfoProbe wire layout changed");
static_assert(sizeof(ServerInfoReply) == 16,
              "ServerInfoReply wire layout changed");

// ── Utilities ────────────────────────────────────────────────────────────────
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

} // namespace ns

#ifdef _MSC_VER
__pragma(pack(pop))
#endif
