#include "virtual_controller.hpp"
#include "app_state.hpp"
#include "bluetooth_manager.hpp"
#include "gadget_wakeup.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <print>
#include <format>
#include <span>
#include <cstring>
#include <sstream>
#include <string_view>

using namespace ns;

uint8_t pro_timer_from_us(uint64_t t_us) { return (uint8_t)((t_us / 5000ULL) & 0xFF); }

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

// S2 report descriptors - EXACT from ndeadly/switch2_controller_research/descriptors.md
// These are the precise HID report descriptors the Switch 2 expects for its controllers.
// (Previously approximated; now using the real parsed bytes for compatibility.)
extern const uint8_t S2_PRO_REPORT_DESC[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x05, 0x05, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x3F, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x09, 0x09, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x15, 0x25, 0x01, 0x95, 0x15, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x35, 0x26, 0xFF, 0x0F, 0x95, 0x04, 0x75, 0x0C, 0x81, 0x02,
    0xC0,
    0x05, 0xFF, 0x09, 0x02, 0x26, 0xFF, 0x00, 0x95, 0x34, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x02, 0x09, 0x01, 0x95, 0x3F, 0x91, 0x02,
    0xC0
};
extern const size_t S2_PRO_REPORT_DESC_SIZE = sizeof(S2_PRO_REPORT_DESC);

extern const uint8_t S2_JC_REPORT_DESC[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x05, 0x05, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x3F, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x07, 0x09, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
    0x05, 0xFF, 0x09, 0x01, 0x26, 0xFF, 0x00, 0x95, 0x01, 0x75, 0x08, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x26, 0xFF, 0x0F, 0x95, 0x02, 0x75, 0x0C, 0x81, 0x02,
    0xC0,
    0x05, 0xFF, 0x09, 0x02, 0x26, 0xFF, 0x00, 0x95, 0x37, 0x75, 0x08, 0x81, 0x02,
    0x85, 0x01, 0x09, 0x01, 0x95, 0x3F, 0x91, 0x02,
    0xC0
};
extern const size_t S2_JC_REPORT_DESC_SIZE = sizeof(S2_JC_REPORT_DESC);


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

static std::vector<uint8_t> g_amiibo_data[HID_PORT_COUNT];
static std::chrono::steady_clock::time_point g_amiibo_expiry[HID_PORT_COUNT];
static bool g_amiibo_modified[HID_PORT_COUNT] = {};
constexpr size_t SPI_FLASH_SIZE = 0x200000; // 2MB per research for S2, 64k for S1 compatibility
uint8_t g_spi_flash[4][SPI_FLASH_SIZE];
bool g_spi_initialized[4] = {};

uint8_t controller_type_for_port(int ctrl) {
    return ctrl >= 0 && ctrl < HID_PORT_COUNT ? g_port_controller_type[ctrl] : NS_TYPE_PRO;
}

void set_amiibo_data_for_port(int port, const uint8_t* data, size_t len) {
    if (port < 0 || port >= HID_PORT_COUNT || !data) return;
    g_amiibo_data[port].assign(data, data + len);
    g_amiibo_expiry[port] = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    g_amiibo_modified[port] = false;
}

void check_amiibo_expiry(int port) {
    if (port < 0 || port >= HID_PORT_COUNT || g_amiibo_data[port].empty()) return;
    if (std::chrono::steady_clock::now() > g_amiibo_expiry[port]) {
        if (g_amiibo_modified[port]) {
            publish_amiibo_writeback(0, port, g_amiibo_data[port].data(), (uint16_t)g_amiibo_data[port].size());
        }
        g_amiibo_data[port].clear();
    }
}

void set_controller_type_for_port(int ctrl, uint8_t protocol_type) {
    if (ctrl < 0 || ctrl >= HID_PORT_COUNT) return;
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
        default:
            ns_type = NS_TYPE_PRO;
            is_s2 = false;
            break;
    }
    if (g_port_controller_type[ctrl] == ns_type && g_port_switch2[ctrl] == is_s2) return;
    g_port_controller_type[ctrl] = ns_type;
    g_port_switch2[ctrl] = is_s2;
    g_port_protocol_type[ctrl] = protocol_type;
    g_spi_initialized[ctrl] = false;
    init_spi_flash(ctrl);
}

uint8_t controller_protocol_type_for_port(int ctrl) {
    if (ctrl >= 0 && ctrl < HID_PORT_COUNT) return g_port_protocol_type[ctrl];
    return ns::CONTROLLER_TYPE_PRO;
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

void apply_controller_type_report(uint8_t type, uint8_t* buf) {
    if (type != NS_TYPE_JOYCON_L && type != NS_TYPE_JOYCON_R) return;
    // buf is a 0x30/0x21 report: conn_info at [2], buttons at [3..5].
    // conn_info low nibble 0xF = Joy-Con ((v >> 1) & 3 == 3) + USB-powered bit.
    buf[2] = (buf[2] & 0xF0) | 0x0F;
    // Map the side's shoulder/trigger pair onto SR/SL so the normal
    // single-Joy-Con "press SL+SR" registration gesture remains available.
    uint8_t& side = type == NS_TYPE_JOYCON_R ? buf[3] : buf[5];
    if (side & 0x40) side |= 0x10;
    if (side & 0x80) side |= 0x20;
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
    if (g_port_switch2[ctrl]) {
        // S2 product IDs per ndeadly research (JoyCon2 R: 0x2066, L: 0x2067, Pro: 0x2069)
        uint8_t proto = controller_protocol_type_for_port(ctrl);
        if (proto == ns::CONTROLLER_TYPE_JOYCON_R_S2 || proto == ns::CONTROLLER_TYPE_JOYCON_R) prod = 0x66; // low byte example
        else if (proto == ns::CONTROLLER_TYPE_JOYCON_L_S2 || proto == ns::CONTROLLER_TYPE_JOYCON_L) prod = 0x67;
        else prod = 0x69; // Pro S2
    }
    flash[0x6012] = prod; flash[0x6013] = 0xA0; flash[0x601B] = 0x02;
    if (g_port_switch2[ctrl]) {
        // S2 factory data per research memory_layout.md and descriptors
        // VID/PID
        flash[0x13012] = 0x7E;
        flash[0x13013] = 0x05;
        flash[0x13014] = prod;
        flash[0x13015] = 0x20;
        // example colors from research (can be customized)
        flash[0x13019] = 0x23; flash[0x1301A] = 0x23; flash[0x1301B] = 0x23; // body
        flash[0x1301C] = 0xA0; flash[0x1301D] = 0xA0; flash[0x1301E] = 0xA0; // buttons
        // cals etc from research (simplified, full in repo)
        // pairing etc at 0x1FA000 would be set if needed
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
        uint8_t proto = controller_protocol_type_for_port(ctrl);
        uint8_t dev_type = proto;
        if (proto == ns::CONTROLLER_TYPE_PRO_S2) dev_type = NS_TYPE_PRO;
        else if (proto == ns::CONTROLLER_TYPE_JOYCON_L_S2) dev_type = NS_TYPE_JOYCON_L;
        else if (proto == ns::CONTROLLER_TYPE_JOYCON_R_S2) dev_type = NS_TYPE_JOYCON_R;
        out[3] = dev_type;
        set_identity_in_0x81(out, ctrl);
    }
    return PRO_REPORT_SIZE;
}

void build_get_device_info_response(uint8_t* out, int ctrl) {
    uint8_t proto = controller_protocol_type_for_port(ctrl);
    uint8_t dev_type = proto;
    if (proto == ns::CONTROLLER_TYPE_PRO_S2) dev_type = NS_TYPE_PRO;
    else if (proto == ns::CONTROLLER_TYPE_JOYCON_L_S2) dev_type = NS_TYPE_JOYCON_L;
    else if (proto == ns::CONTROLLER_TYPE_JOYCON_R_S2) dev_type = NS_TYPE_JOYCON_R;
    else if (proto == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2) dev_type = NS_TYPE_PRO; // or handle pair
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

void build_standard_report(const HIDReport& src, const MotionReport motion_samples[3], bool has_motion, bool imu_enabled, uint8_t timer, ProInputReport30& out, bool is_switch2 = false) {
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

// Basic S2 report builder based on ndeadly research (hid_reports.md)
// For Pro S2: report ID 0x09, specific button layout (includes C/GL/GR), 12bit sticks, motion
void build_s2_pro_report(const HIDReport& src, const MotionReport motion_samples[3], bool has_motion, bool imu_enabled, uint8_t timer, uint8_t* out) {
    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = 0x09;                    // Report ID 0x09 (S2 Pro main input per research)
    out[1] = timer;                   // counter (research: byte 0 after ID)
    // S2 power info bitfield (research): [0] external, [1] charging, [2-5] level 0-9
    uint8_t pwr = 0x01;
    if (src.reserved[1] & EXT_STATUS_BATTERY_VALID) {
      int pct = src.reserved[0];
      uint8_t lvl = std::min<uint8_t>(9, pct / 11);
      pwr = (lvl << 2) | 0x01;
    }
    if (src.reserved[1] & EXT_STATUS_BATTERY_CHARGING) pwr |= 0x02;
    out[2] = pwr;

    // Buttons per research hid_reports.md for 0x09
    uint16_t btn = src.input.buttons;
    uint8_t hat = src.input.hat;
    out[3] = (btn & BTN_RSTICK ? 0x80 : 0) | (btn & BTN_PLUS ? 0x40 : 0) |
             (btn & BTN_ZR ? 0x20 : 0) | (btn & BTN_R ? 0x10 : 0) |
             (btn & BTN_X ? 0x08 : 0) | (btn & BTN_Y ? 0x04 : 0) |
             (btn & BTN_A ? 0x02 : 0) | (btn & BTN_B ? 0x01 : 0);
    out[4] = (btn & BTN_LSTICK ? 0x80 : 0) | (btn & BTN_MINUS ? 0x40 : 0) |
             (btn & BTN_ZL ? 0x20 : 0) | (btn & BTN_L ? 0x10 : 0) |
             ((hat == HAT_N || hat == HAT_NE || hat == HAT_NW) ? 0x08 : 0) | // Up
             ((hat == HAT_W || hat == HAT_NW || hat == HAT_SW) ? 0x04 : 0) | // Left
             ((hat == HAT_E || hat == HAT_NE || hat == HAT_SE) ? 0x02 : 0) | // Right
             ((hat == HAT_S || hat == HAT_SE || hat == HAT_SW) ? 0x01 : 0);  // Down
    out[5] = (btn & BTN_CAPTURE ? 0x20 : 0) | (btn & BTN_HOME ? 0x10 : 0);
    // C, GL, GR, headset bits per research (extra in S2 Pro) - 0 for base

    // Left stick (research bytes 5-7, 12-bit)
    pack_stick_12(out + 6, src.input.lx, src.input.ly);
    // Right stick (research bytes 8-10)
    pack_stick_12(out + 9, src.input.rx, src.input.ry);

    // Unknown, NFC state, headset audio state (research)
    out[12] = 0x30;
    out[13] = 0; // NFC state
    out[14] = 0; // headset

    // Motion: length + data (research uses 0x28 for 3 samples, packed)
    out[15] = (has_motion && imu_enabled) ? 0x28 : 0;
    if (has_motion && imu_enabled && motion_samples) {
        // Placeholder - research has specific accel/gyro packing per sample
        memcpy(out + 16, motion_samples, sizeof(MotionReport) * 3);
    }
}

// S2 Joy-Con builder (research 0x07 for L, 0x08 for R)
void build_s2_jc_report(const HIDReport& src, const MotionReport motion_samples[3], bool has_motion, bool imu_enabled, uint8_t timer, uint8_t report_id, uint8_t* out) {
    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = report_id; // 0x07 L or 0x08 R
    out[1] = timer;
    uint8_t pwr = 0x01;
    if (src.reserved[1] & EXT_STATUS_BATTERY_VALID) {
      int pct = src.reserved[0];
      uint8_t lvl = std::min<uint8_t>(9, pct / 11);
      pwr = (lvl << 2) | 0x01;
    }
    if (src.reserved[1] & EXT_STATUS_BATTERY_CHARGING) pwr |= 0x02;
    out[2] = pwr;

    uint16_t btn = src.input.buttons;
    uint8_t hat = src.input.hat;
    if (report_id == 0x07) { // L per research
        out[3] = (btn & BTN_LSTICK ? 0x80 : 0) | (btn & BTN_MINUS ? 0x40 : 0) | (btn & BTN_ZL ? 0x20 : 0) | (btn & BTN_L ? 0x10 : 0) |
                 ((hat == HAT_N || hat == HAT_NE || hat == HAT_NW) ? 0x08 : 0) |
                 ((hat == HAT_W || hat == HAT_NW || hat == HAT_SW) ? 0x04 : 0) |
                 ((hat == HAT_E || hat == HAT_NE || hat == HAT_SE) ? 0x02 : 0) |
                 ((hat == HAT_S || hat == HAT_SE || hat == HAT_SW) ? 0x01 : 0);
        out[4] = (btn & BTN_CAPTURE ? 0x80 : 0);
        pack_stick_12(out + 5, src.input.lx, src.input.ly);
    } else { // R
        out[3] = (btn & BTN_RSTICK ? 0x80 : 0) | (btn & BTN_PLUS ? 0x40 : 0) | (btn & BTN_ZR ? 0x20 : 0) | (btn & BTN_R ? 0x10 : 0) |
                 (btn & BTN_X ? 0x08 : 0) | (btn & BTN_Y ? 0x04 : 0) | (btn & BTN_A ? 0x02 : 0) | (btn & BTN_B ? 0x01 : 0);
        out[4] = (btn & BTN_HOME ? 0x40 : 0);
        pack_stick_12(out + 5, src.input.rx, src.input.ry);
    }
    out[9] = 0; // NFC
    out[10] = (has_motion && imu_enabled) ? 0x28 : 0;
    if (has_motion && imu_enabled && motion_samples) {
        memcpy(out + 11, motion_samples, sizeof(MotionReport) * 3);
    }
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
    case 0x15: // pairing related
        reply->ack = 0x80;
        return 0;
    // NFC subs from research for amiibo
    case 0x05: // Get status - signal scan requested if no tag
        reply->ack = 0x80;
        // return status with no tag or with if placed
        reply->reply_data[0] = 0x09; // example
        // if data placed, set state to present
        if (!g_amiibo_data[rt.ctrl].empty()) {
            reply->reply_data[1] = 0x01; // present
        }
        publish_amiibo_request(0, rt.ctrl, g_amiibo_data[rt.ctrl].empty()); // notify client if no data yet
        return 8;
    case 0x06: // Read device
        reply->ack = 0x80;
        if (!g_amiibo_data[rt.ctrl].empty()) {
            // return some device info or UID from bin
            std::memcpy(reply->reply_data, g_amiibo_data[rt.ctrl].data(), std::min<size_t>(16, g_amiibo_data[rt.ctrl].size()));
        }
        return 16;
    case 0x15: // Read buffer - serve the amiibo bin
        reply->ack = 0x80;
        if (!g_amiibo_data[rt.ctrl].empty() && cmd_data.size() >= 2) {
            uint16_t offset = cmd_data[0] | (cmd_data[1] << 8); // assume
            size_t to_copy = std::min(g_amiibo_data[rt.ctrl].size() - offset, (size_t)64);
            std::memcpy(reply->reply_data, g_amiibo_data[rt.ctrl].data() + offset, to_copy);
            reply->reply_data[0] = (uint8_t)to_copy; // length?
        }
        return 64;
    case 0x14: // Write buffer - write to bin data
        reply->ack = 0x80;
        if (!g_amiibo_data[rt.ctrl].empty() && cmd_data.size() > 2) {
            uint16_t offset = cmd_data[0] | (cmd_data[1] << 8);
            size_t len = cmd_data.size() - 2;
            if (offset + len <= g_amiibo_data[rt.ctrl].size()) {
                std::memcpy(g_amiibo_data[rt.ctrl].data() + offset, cmd_data.data() + 2, len);
                g_amiibo_modified[rt.ctrl] = true;
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

void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4 || len < 10) return;
    const uint8_t* rb = packet + 2;
    DecodedPrecisionRumbleHalf left = rumble_decode_half_precision_to_dual(rb), right = rumble_decode_half_precision_to_dual(rb + 4);
    uint8_t low = std::max(left.low, right.low), high = std::max(left.high, right.high);

    if (low == 0 && high == 0 && !(rumble_half_is_all_zero(rb) || rumble_half_is_neutral_carrier(rb)) && !(rumble_half_is_all_zero(rb + 4) || rumble_half_is_neutral_carrier(rb + 4))) {
        low = rumble_decode_half_to_u8(rb); high = rumble_decode_half_to_u8(rb + 4);
    }

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
    memcpy(precision_ev.precision, rb, sizeof(precision_ev.precision));

    g_ctx.clients[client_idx].rumble_active[sub_idx] = !neutral;
    g_ctx.clients[client_idx].rumble_seq[sub_idx]++;
}
