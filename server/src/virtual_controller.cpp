#include "virtual_controller.hpp"
#include "app_state.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <print>
#include <format>
#include <span>
#include <algorithm>
#include <cstring>

using namespace ns;

uint8_t pro_timer_from_us(uint64_t t_us) {
    // The byte after report ID 0x30/0x21 is a small controller timer.  Real
    // controllers advance it with time, not merely by "one per report"; using
    // a 5ms unit matches the 3x IMU sample spacing most software expects.
    return (uint8_t)((t_us / 5000ULL) & 0xFF);
}



// Vendor USB gamepad descriptor, same 64-byte input/output report
// descriptor previously written by setup_gadget.sh.
extern const uint8_t VIRTUAL_CONTROLLER_REPORT_DESC[203] = {
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

extern const uint8_t LEGACY_REPORT_DESC[85] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x01,0x75,0x01,
    0x95,0x0D,0x05,0x09,0x19,0x01,0x29,0x0D,0x81,0x02,0x95,0x03,0x81,0x01,0x05,0x01,
    0x25,0x07,0x46,0x3B,0x01,0x75,0x04,0x95,0x01,0x65,0x14,0x09,0x39,0x81,0x42,
    0x65,0x00,0x95,0x01,0x81,0x01,0x26,0xFF,0x00,0x46,0xFF,0x00,0x09,0x30,0x09,
    0x31,0x09,0x32,0x09,0x35,0x75,0x08,0x95,0x04,0x81,0x02,0x06,0x00,0xFF,0x09,
    0x20,0x75,0x08,0x95,0x01,0x81,0x02,0xC0
};

uint8_t CTRL_MAC_BE[4][6] = {
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA0},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA1},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA2},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA3},
};

std::string CTRL_SERIAL[4] = {
    "NSGP260606A0", "NSGP260606A1", "NSGP260606A2", "NSGP260606A3"
};

bool read_random_bytes(uint8_t* dst, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t r = read(fd, dst + off, len - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        close(fd);
        if (off == len) return true;
    }

    uint64_t seed = (uint64_t)now_us() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < len; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        dst[i] = (uint8_t)(seed & 0xFF);
    }
    return true;
}

void randomize_controller_identity() {
    uint8_t rnd[16]{};
    read_random_bytes(rnd, sizeof(rnd));

    // Locally administered unicast MACs. Keep 02:4E:53 ("NS") as a stable
    // virtual vendor prefix and randomize the low bytes so the host cannot
    // reuse cached calibration/association for the previous virtual controller.
    for (int i = 0; i < 4; ++i) {
        CTRL_MAC_BE[i][0] = 0x02;
        CTRL_MAC_BE[i][1] = 0x4E;
        CTRL_MAC_BE[i][2] = 0x53;
        CTRL_MAC_BE[i][3] = rnd[(i * 3 + 0) % sizeof(rnd)];
        CTRL_MAC_BE[i][4] = rnd[(i * 3 + 1) % sizeof(rnd)];
        CTRL_MAC_BE[i][5] = (uint8_t)(rnd[(i * 3 + 2) % sizeof(rnd)] + i);
        CTRL_SERIAL[i] = "NSGP";
        CTRL_SERIAL[i] += std::format("{:02X}{:02X}{:02X}{:02X}",
                      CTRL_MAC_BE[i][2], CTRL_MAC_BE[i][3], CTRL_MAC_BE[i][4], CTRL_MAC_BE[i][5]);
    }

    g_ctx.usb_serial = std::format("{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                  rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
}

constexpr size_t SPI_FLASH_SIZE = 0x10000;
uint8_t g_spi_flash[4][SPI_FLASH_SIZE];
bool g_spi_initialized[4] = {};

[[maybe_unused]] int16_t clamp_i16(int v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

[[maybe_unused]] void pack12(uint16_t val, uint8_t& b0, uint8_t& b1) {
    b0 = val & 0xFF;
    b1 = (b1 & 0xF0) | ((val >> 8) & 0x0F);
}

void init_spi_flash(int ctrl) {
    if (ctrl < 0 || ctrl >= 4 || g_spi_initialized[ctrl]) return;

    uint8_t* flash = g_spi_flash[ctrl];
    memset(flash, 0xFF, SPI_FLASH_SIZE);

    // Public synthetic SPI profile v2.
    // This intentionally contains no dump/private bytes.  It only builds a
    // coherent, boring factory profile from generic calibration constants.
    // Key points learned from compatibility testing:
    //   - report as USB gamepad type 0x03
    //   - no user IMU calibration magic at 0x8026
    //   - prefer factory calibration blocks instead of invented user blocks
    //   - keep 0x6098 as stick-model continuation, not IMU calibration
    flash[0x6012] = 0x03;
    flash[0x6013] = 0xA0;
    flash[0x601B] = 0x02;

    auto put_i16_le = [&](uint16_t addr, int16_t val) {
        flash[addr]     = (uint8_t)(val & 0xFF);
        flash[addr + 1] = (uint8_t)((uint16_t)val >> 8);
    };

    auto pack12_pair = [](uint8_t* dst, uint16_t x, uint16_t y) {
        x &= 0x0FFF;
        y &= 0x0FFF;
        dst[0] = (uint8_t)(x & 0xFF);
        dst[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
        dst[2] = (uint8_t)((y >> 4) & 0xFF);
    };

    // Factory stick calibration: neutral center with symmetric, conservative
    // range.  Do not set user-cal magic; forcing factory cal avoids a mixed
    // synthetic factory/user profile that some hosts handle badly.
    static constexpr uint16_t STICK_CENTER = 0x800;
    static constexpr uint16_t STICK_RANGE  = 0x600;

    uint8_t left_cal[9]{};
    uint8_t right_cal[9]{};
    pack12_pair(left_cal  + 0, STICK_RANGE,  STICK_RANGE);
    pack12_pair(left_cal  + 3, STICK_CENTER, STICK_CENTER);
    pack12_pair(left_cal  + 6, STICK_RANGE,  STICK_RANGE);
    pack12_pair(right_cal + 0, STICK_CENTER, STICK_CENTER);
    pack12_pair(right_cal + 3, STICK_RANGE,  STICK_RANGE);
    pack12_pair(right_cal + 6, STICK_RANGE,  STICK_RANGE);

    memcpy(flash + 0x603D, left_cal,  sizeof(left_cal));
    memcpy(flash + 0x6046, right_cal, sizeof(right_cal));

    // Explicitly erase user stick/IMU calibration magic areas.  The host should
    // use the factory blocks above rather than half-synthetic user cal.
    memset(flash + 0x8010, 0xFF, 0x30);
    flash[0x8026] = 0xFF;
    flash[0x8027] = 0xFF;
    memset(flash + 0x8028, 0xFF, 0x18);

    // Factory IMU calibration at 0x6020, 24 bytes:
    // accel offsets XYZ, accel scales XYZ, gyro offsets XYZ, gyro scales XYZ.
    // Values are generic and synthetic, not copied from any controller dump.
    static constexpr int16_t IMU_ACCEL_OFFSET = 0;
    static constexpr int16_t IMU_ACCEL_SCALE  = 0x4000;
    static constexpr int16_t IMU_GYRO_OFFSET  = 0;
    static constexpr int16_t IMU_GYRO_SCALE   = 0x343B;
    const int16_t imu_vals[12] = {
        IMU_ACCEL_OFFSET, IMU_ACCEL_OFFSET, IMU_ACCEL_OFFSET,
        IMU_ACCEL_SCALE,  IMU_ACCEL_SCALE,  IMU_ACCEL_SCALE,
        IMU_GYRO_OFFSET,  IMU_GYRO_OFFSET,  IMU_GYRO_OFFSET,
        IMU_GYRO_SCALE,   IMU_GYRO_SCALE,   IMU_GYRO_SCALE
    };
    for (int i = 0; i < 12; ++i)
        put_i16_le((uint16_t)(0x6020 + i * 2), imu_vals[i]);

    // 0x6080..0x6085: IMU horizontal offsets, three int16 values.
    put_i16_le(0x6080, 0);
    put_i16_le(0x6082, 0);
    put_i16_le(0x6084, 0);

    // 0x6086..0x60A9: stick model/parameter block.  Keep it coherent instead
    // of all-0xFF or random bytes.  Bytes 3..5 are commonly unpacked as two
    // packed 12-bit values: deadzone and range ratio.
    uint8_t stick_model[0x24]{};
    static constexpr uint16_t STICK_DEADZONE    = 0x0A0;
    static constexpr uint16_t STICK_RANGE_RATIO = 0x100;
    pack12_pair(stick_model + 3, STICK_DEADZONE, STICK_RANGE_RATIO);
    // Soft, generic model tail.  These are synthetic low-entropy defaults; they
    // are only here to avoid an empty/0xFF model continuation.
    for (size_t i = 6; i < sizeof(stick_model); ++i)
        stick_model[i] = (i & 1) ? 0x30 : 0x0F;
    memcpy(flash + 0x6086, stick_model, sizeof(stick_model));

    // Controller colors: synthetic per-slot colors with white buttons.
    static const uint8_t BODY_RGB[4][3] = {
        {0xE6, 0x00, 0x12},
        {0xFF, 0xCC, 0x00},
        {0x00, 0x64, 0xFF},
        {0x00, 0xC8, 0x53},
    };
    const uint8_t* body = BODY_RGB[ctrl];
    flash[0x6050] = body[0]; flash[0x6051] = body[1]; flash[0x6052] = body[2];
    flash[0x6053] = 0xFF;    flash[0x6054] = 0xFF;    flash[0x6055] = 0xFF;
    flash[0x6056] = body[0]; flash[0x6057] = body[1]; flash[0x6058] = body[2];
    flash[0x6059] = 0xFF;    flash[0x605A] = 0xFF;    flash[0x605B] = 0xFF;
    flash[0x605C] = 0x00;

    g_spi_initialized[ctrl] = true;
}

void set_identity_in_0x81(uint8_t* resp_81, int ctrl) {
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    // USB 0x81 MAC reply stores MAC little-endian in bytes 4..9, matching
    // Chromium's MacAddressReport/UnpackconsoleMacAddress handling.
    resp_81[4] = mac[5]; resp_81[5] = mac[4]; resp_81[6] = mac[3];
    resp_81[7] = mac[2]; resp_81[8] = mac[1]; resp_81[9] = mac[0];
}

size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl) {
    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = 0x81;
    out[1] = subtype;
    switch (subtype) {
    case 0x01: // request MAC/address/device type
        out[2] = 0x00; // padding
        out[3] = 0x03; // USB gamepad
        set_identity_in_0x81(out, ctrl);
        break;
    case 0x02: // USB handshake
    case 0x03: // set UART/baudrate
    case 0x04: // disable USB timeout; real devices may not ACK, but an ACK is accepted
    case 0x05: // enable USB timeout
    default:
        // Chromium and hid-Vendor only require report 0x81 subtype to advance
        // these USB-init steps. Keep remaining bytes zero, like a minimal ACK.
        break;
    }
    return PRO_REPORT_SIZE;
}

void build_get_device_info_response(uint8_t* out, int ctrl) {
    memset(out, 0, 36);

    // Subcmd 0x02 device-info reply.
    //
    //   majorVersion   = 0x03
    //   minorVersion   = 0x49
    //   controllerType = 0x03
    //   unknown00      = 0x02
    //   macAddress     = generated MAC, reversed/little-endian
    //   unknown01      = 0x01
    //   storedColors   = 0x02
    //
    // Important: MAC stays generated per virtual controller.
    out[0] = 0x03; // majorVersion
    out[1] = 0x49; // minorVersion
    out[2] = 0x03; // controllerType: 64-byte controller
    out[3] = 0x02; // unknown00

    const uint8_t* mac = CTRL_MAC_BE[ctrl];

    // Device info wants MAC reversed / little-endian.
    out[4] = mac[5];
    out[5] = mac[4];
    out[6] = mac[3];
    out[7] = mac[2];
    out[8] = mac[1];
    out[9] = mac[0];

    out[10] = 0x01; // unknown01
    out[11] = 0x02; // storedColors
}

void fill_neutral_controls(ProInputReport30& r) {
    r.conn_info = PRO_BAT_CON;
    // Real USB USB gamepad captures keep bit 0x80 set in the middle button byte
    // even at rest: neutral buttons are 00 80 00, not 00 00 00.  Keep this base
    // bit in both 0x30 and 0x21 snapshots because some console/game paths appear
    // to gate motion/precision-rumble capability on the complete controller state, not
    // merely on the IMU bytes.
    r.buttons[0] = 0x00;
    r.buttons[1] = 0x80;
    r.buttons[2] = 0x00;
    r.left_stick[0]  = 0x00; r.left_stick[1]  = 0x08; r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00; r.right_stick[1] = 0x08; r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT;
}

void fill_neutral_controls(ProInputReport21& r) {
    r.conn_info = PRO_BAT_CON;
    r.buttons[0] = 0x00;
    r.buttons[1] = 0x80;
    r.buttons[2] = 0x00;
    r.left_stick[0]  = 0x00; r.left_stick[1]  = 0x08; r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00; r.right_stick[1] = 0x08; r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT;
}

uint16_t axis8_to_12(uint8_t v) {
    // Match the fake calibration above: center 0x800 with about ±0x600 range.
    // Sending the full 0x000..0xFFF range can sit outside the advertised
    // calibration and some console paths appear to flatten/ignore the stick.
    if (v == 128) return 0x800;

    int32_t delta = (int32_t)v - 128;
    int32_t raw;
    if (delta > 0)
        raw = 0x800 + (delta * 0x600) / 127;
    else
        raw = 0x800 + (delta * 0x600) / 128;

    if (raw < 0x200) raw = 0x200;
    if (raw > 0xE00) raw = 0xE00;
    return (uint16_t)raw;
}

uint8_t invert_axis8_centered(uint8_t v) {
    // 0 and 255 should swap, but keep the protocol's exact neutral value
    // neutral.  A raw 255-v inversion turns 128 into 127, which creates a tiny
    // permanent off-center Y value.
    return v == 128 ? 128 : (uint8_t)(255 - v);
}

void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8) {
    // Input protocol uses 0 = up/left and 255 = down/right.  The console raw
    // stick format has Y in the opposite direction, so invert Y once here for
    // both sticks.
    uint16_t x = axis8_to_12(x8);
    uint16_t y = axis8_to_12(invert_axis8_centered(y8));
    out[0] = x & 0xFF;
    out[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    out[2] = (y >> 4) & 0xFF;
}

bool input_is_neutral(const HIDReport& r) {
    return r.buttons == 0 && r.hat == HAT_NEUTRAL &&
           r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
}

bool motion_is_neutral(const MotionReport& m) {
    return std::abs((int)m.ax) < 64 && std::abs((int)m.ay) < 64 && std::abs((int)m.az) < 64 &&
           std::abs((int)m.gx) < 64 && std::abs((int)m.gy) < 64 && std::abs((int)m.gz) < 64;
}

bool extended_is_neutral(const ExtendedHIDReport& r) {
    return input_is_neutral(r.input) && (!r.has_motion || motion_is_neutral(r.motion));
}

void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]) {
    bool up = false, down = false, left = false, right = false;
    switch (hat) {
        case HAT_N:  up = true; break;
        case HAT_NE: up = true; right = true; break;
        case HAT_E:  right = true; break;
        case HAT_SE: down = true; right = true; break;
        case HAT_S:  down = true; break;
        case HAT_SW: down = true; left = true; break;
        case HAT_W:  left = true; break;
        case HAT_NW: up = true; left = true; break;
        default: break;
    }
    if (down)  buttons[2] |= 0x01;
    if (up)    buttons[2] |= 0x02;
    if (right) buttons[2] |= 0x04;
    if (left)  buttons[2] |= 0x08;
}

void apply_input_controls_to_pro21(const ExtendedHIDReport& src, ProInputReport21& out) {
    // Subcommand replies (report 0x21) contain the same button/stick snapshot
    // fields as standard input reports.  Keep them in sync with the currently
    // held web/UDP input; otherwise frequent console output/subcommand traffic
    // injects neutral frames between normal 0x30 reports, which makes held
    // buttons such as R/ZR flicker in-game.
    out.conn_info = PRO_BAT_CON;
    memset(out.buttons, 0, sizeof(out.buttons));
    out.buttons[1] = 0x80; // real neutral USB Pro state is 00 80 00
    out.vibrator = PRO_VIBRATOR_REPORT;

    const HIDReport& in = src.input;
    if (in.buttons & BTN_Y)       out.buttons[0] |= 0x01;
    if (in.buttons & BTN_X)       out.buttons[0] |= 0x02;
    if (in.buttons & BTN_B)       out.buttons[0] |= 0x04;
    if (in.buttons & BTN_A)       out.buttons[0] |= 0x08;
    if (in.buttons & BTN_R)       out.buttons[0] |= 0x40;
    if (in.buttons & BTN_ZR)      out.buttons[0] |= 0x80;

    if (in.buttons & BTN_MINUS)   out.buttons[1] |= 0x01;
    if (in.buttons & BTN_PLUS)    out.buttons[1] |= 0x02;
    if (in.buttons & BTN_RSTICK)  out.buttons[1] |= 0x04;
    if (in.buttons & BTN_LSTICK)  out.buttons[1] |= 0x08;
    if (in.buttons & BTN_HOME)    out.buttons[1] |= 0x10;
    if (in.buttons & BTN_CAPTURE) out.buttons[1] |= 0x20;

    hat_to_pro_buttons(in.hat, out.buttons);
    if (in.buttons & BTN_L)       out.buttons[2] |= 0x40;
    if (in.buttons & BTN_ZL)      out.buttons[2] |= 0x80;

    pack_stick_12(out.left_stick,  in.lx, in.ly);
    pack_stick_12(out.right_stick, in.rx, in.ry);
}

void build_standard_report(const ExtendedHIDReport& src,
                                  const MotionReport motion_samples[3],
                                  bool has_motion,
                                  bool imu_enabled,
                                  uint8_t timer,
                                  ProInputReport30& out) {
    memset(&out, 0, sizeof(out));
    out.id = RID_INPUT_STANDARD;
    out.timer = timer;
    fill_neutral_controls(out);

    const HIDReport& in = src.input;
    if (in.buttons & BTN_Y)       out.buttons[0] |= 0x01;
    if (in.buttons & BTN_X)       out.buttons[0] |= 0x02;
    if (in.buttons & BTN_B)       out.buttons[0] |= 0x04;
    if (in.buttons & BTN_A)       out.buttons[0] |= 0x08;
    if (in.buttons & BTN_R)       out.buttons[0] |= 0x40;
    if (in.buttons & BTN_ZR)      out.buttons[0] |= 0x80;

    if (in.buttons & BTN_MINUS)   out.buttons[1] |= 0x01;
    if (in.buttons & BTN_PLUS)    out.buttons[1] |= 0x02;
    if (in.buttons & BTN_RSTICK)  out.buttons[1] |= 0x04;
    if (in.buttons & BTN_LSTICK)  out.buttons[1] |= 0x08;
    if (in.buttons & BTN_HOME)    out.buttons[1] |= 0x10;
    if (in.buttons & BTN_CAPTURE) out.buttons[1] |= 0x20;

    hat_to_pro_buttons(in.hat, out.buttons);
    if (in.buttons & BTN_L)       out.buttons[2] |= 0x40;
    if (in.buttons & BTN_ZL)      out.buttons[2] |= 0x80;

    pack_stick_12(out.left_stick,  in.lx, in.ly);
    pack_stick_12(out.right_stick, in.rx, in.ry);

    MotionReport imu[3]{};
    const bool has_imu = imu_enabled && has_motion && motion_samples;
    if (has_imu) {
        imu[0] = motion_samples[0];
        imu[1] = motion_samples[1];
        imu[2] = motion_samples[2];
    }

    if (g_ctx.verbose && (!input_is_neutral(in) || has_imu)) {
        static uint64_t last_log_us = 0;
        uint64_t t = now_us();
        if (t - last_log_us > 250000) {
            last_log_us = t;
            std::println("[input] lx={:3} ly={:3} rx={:3} ry={:3} | L={:02X} {:02X} {:02X} R={:02X} {:02X} {:02X} | motion={} samples={} ax={:6} ay={:6} az={:6} gx={:6} gy={:6} gz={:6}",
                        in.lx, in.ly, in.rx, in.ry,
                        out.left_stick[0], out.left_stick[1], out.left_stick[2],
                        out.right_stick[0], out.right_stick[1], out.right_stick[2],
                        has_imu ? "yes" : "no",
                        (unsigned)(has_imu ? 3 : 0),
                        (int)imu[2].ax, (int)imu[2].ay, (int)imu[2].az,
                        (int)imu[2].gx, (int)imu[2].gy, (int)imu[2].gz);
        }
    }


auto store_imu_sample = [](ProInputReport30& dst, int idx, const MotionReport& m) {
    if (idx == 0) {
        dst.accel_y_0 = m.ax;
        dst.accel_x_0 = m.ay;
        dst.accel_z_0 = m.az;

        dst.gyro_y_0  = m.gx;
        dst.gyro_x_0  = m.gy;
        dst.gyro_z_0  = m.gz;
    } else if (idx == 1) {
        dst.accel_y_1 = m.ax;
        dst.accel_x_1 = m.ay;
        dst.accel_z_1 = m.az;

        dst.gyro_y_1  = m.gx;
        dst.gyro_x_1  = m.gy;
        dst.gyro_z_1  = m.gz;
    } else {
        dst.accel_y_2 = m.ax;
        dst.accel_x_2 = m.ay;
        dst.accel_z_2 = m.az;

        dst.gyro_y_2  = m.gx;
        dst.gyro_x_2  = m.gy;
        dst.gyro_z_2  = m.gz;
    }
};

store_imu_sample(out, 0, imu[0]);
store_imu_sample(out, 1, imu[1]);
store_imu_sample(out, 2, imu[2]);
}

int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply) {
    std::ranges::fill(reply->reply_data, 0);
    reply->ack = 0x80;
    reply->subcmd_id = subcmd;

    switch (subcmd) {
    case CMD_BT_MANUAL_PAIRING: {
        // Public/synthetic pairing pages.  Keep the shape but do not embed any
        // private pairing material.
        reply->ack = 0x81;
        if (!cmd_data.empty() && cmd_data[0] == 0x02) {
            std::ranges::fill_n(reply->reply_data, 16, 0x00);
            return 16;
        }
        if (!cmd_data.empty() && cmd_data[0] == 0x03) {
            std::ranges::fill_n(reply->reply_data, 16, 0x00);
            return 16;
        }
        return 0;
    }

    case CMD_TRIGGER_BUTTONS:
        reply->ack = 0x83;
        reply->reply_data[0] = 0x00;
        return 1;

    case CMD_SET_SHIP_MODE:
        reply->ack = 0x80;
        return 0;

    case CMD_SET_NFC_IR_CONFIG:
        reply->ack = 0xA0;
        reply->reply_data[0] = 0x01;
        return 1;

    case CMD_SET_IMU_SENS:
        reply->ack = 0x80;
        return 0;

    case CMD_GET_DEVICE_INFO: {
        uint8_t info[36];
        build_get_device_info_response(info, rt.ctrl);
        reply->ack = 0x82;
        std::ranges::copy(info, reply->reply_data);
        return 36;
    }

    case CMD_SET_DATA_FORMAT:
        rt.full_report_enabled = true;
        reply->ack = 0x80;
        return 0;

    case CMD_SPI_FLASH_READ: {
        if (cmd_data.size() < 5) {
            reply->ack = 0x00;
            return 0;
        }
        uint32_t addr = ((uint32_t)cmd_data[0]) |
                        ((uint32_t)cmd_data[1] << 8) |
                        ((uint32_t)cmd_data[2] << 16) |
                        ((uint32_t)cmd_data[3] << 24);
        uint8_t size = cmd_data[4];
        if (size > 44) size = 44;

        reply->ack = 0x90;
        reply->reply_data[0] = cmd_data[0];
        reply->reply_data[1] = cmd_data[1];
        reply->reply_data[2] = cmd_data[2];
        reply->reply_data[3] = cmd_data[3];
        reply->reply_data[4] = size;

        uint8_t* flash = g_spi_flash[rt.ctrl];
        if (addr < SPI_FLASH_SIZE) {
            size_t to_copy = std::min((size_t)size, (size_t)(SPI_FLASH_SIZE - addr));
            memcpy(reply->reply_data + 5, flash + addr, to_copy);
            if (to_copy < size) memset(reply->reply_data + 5 + to_copy, 0xFF, size - to_copy);
        } else {
            memset(reply->reply_data + 5, 0xFF, size);
        }
        if (g_ctx.verbose) {
            std::print("[pro{}] SPI read addr=0x{:04X} size={}", rt.ctrl + 1, addr, size);
            if (addr == 0x6020 || addr == 0x8026 || addr == 0x8028 || addr == 0x6080 || addr == 0x6086 || addr == 0x6098) {
                std::print(" data=");
                for (uint8_t i = 0; i < size && i < 32 && addr + i < SPI_FLASH_SIZE; ++i)
                    std::print("{:02X}{}", flash[addr + i], (i + 1 < size && i + 1 < 32 && addr + i + 1 < SPI_FLASH_SIZE) ? " " : "");
            }
            std::println("");
        }
        return 5 + size;
    }

    case CMD_SET_PLAYER_LIGHTS:
        reply->ack = 0x80;
        return 0;

    case 0x33:
        reply->ack = 0x80;
        return 0;

    case CMD_ENABLE_IMU:
        rt.imu_enabled = (cmd_data.size() == 0) || cmd_data[0] != 0;
        reply->ack = 0x80;
        return 0;

    case CMD_ENABLE_VIBRATION:
        rt.vibration_enabled = (cmd_data.size() == 0) || cmd_data[0] != 0;
        reply->ack = 0x80;
        return 0;

    default:
        reply->ack = 0x80;
        return 0;
    }
}

bool rumble_half_is_all_zero(const uint8_t* f) {
    return f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0;
}

bool rumble_half_is_neutral_carrier(const uint8_t* f) {
    return f[0] == 0x00 && f[1] == 0x01 && f[2] == 0x40 && f[3] == 0x40;
}

struct DecodedPrecisionRumbleHalf {
    uint8_t low = 0;   // low-frequency/weak motor approximation
    uint8_t high = 0;  // high-frequency/strong motor approximation
};

uint8_t rumble_scale_capture_delta(int v) {
    v = (v * RUMBLE_GAIN_PERCENT) / 100;
    if (v > 0 && v < (int)RUMBLE_MIN_NONZERO) v = RUMBLE_MIN_NONZERO;
    return (uint8_t)std::clamp(v, 0, 255);
}

DecodedPrecisionRumbleHalf rumble_decode_half_precision_to_dual(const uint8_t* f) {
    DecodedPrecisionRumbleHalf out{};
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f))
        return out;

    // Precision rumble is not a simple dual-rumble packet: each 4-byte half
    // carries one actuator with low/high frequency components.  For SDL/Web
    // clients, preserve that split by decoding each half into weak+strong and
    // later combining left/right actuators with max().
    //
    // This approximation is capture-derived and intentionally conservative:
    // neutral carrier 00 01 40 40 maps to 0, but packets such as
    // 26 09 81 5D / B3 2F AF 52 / BB 9D 2D 40 produce meaningful low/high
    // values instead of being crushed into one generic strength.
    int high_delta = std::abs((int)(f[1] & 0x7F) - 0x01);
    int low_delta  = std::abs((int)(f[3] & 0x7F) - 0x40);

    // The first and third bytes mainly move with frequency/envelope.  They are
    // useful to keep tiny precision pulses alive after conversion to classic rumble.
    int high_env = std::abs((int)f[0] - 0x00) / 3;
    int low_env  = std::abs((int)(f[2] & 0x7F) - 0x40) / 2;

    out.high = rumble_scale_capture_delta(high_delta * 3 + high_env);
    out.low  = rumble_scale_capture_delta(low_delta  * 3 + low_env);
    return out;
}

uint8_t rumble_decode_half_to_u8(const uint8_t* f) {
    static const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};

    // Important: a 4-byte all-zero half-frame is an OFF motor half, not a
    // full-power rumble.  The old max-difference decoder turned 00 00 00 00
    // into 255 because it is far away from 00 01 40 40.  That made many
    // asymmetric/tiny Precision rumble packets become fake huge pulses.
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f))
        return 0;

    int max_diff = 0;
    int sum_diff = 0;
    for (int i = 0; i < 4; ++i) {
        int d = std::abs((int)f[i] - (int)neutral[i]);
        max_diff = std::max(max_diff, d);
        sum_diff += d;
    }

    // This is still a compact precision-rumble -> classic dual-rumble approximation,
    // but it preserves low magnitudes instead of clamping everything to >=80.
    // Low console pulses therefore get forwarded as low 1..79 values.
    int strength = max_diff * 2 + sum_diff / 4;
    strength = (strength * RUMBLE_GAIN_PERCENT) / 100;
    if (strength > 0 && strength < (int)RUMBLE_MIN_NONZERO)
        strength = RUMBLE_MIN_NONZERO;
    return (uint8_t)std::clamp(strength, 0, 255);
}

void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4 || len < 10)

        return;

    const uint8_t* rb = packet + 2;
    DecodedPrecisionRumbleHalf left  = rumble_decode_half_precision_to_dual(rb);
    DecodedPrecisionRumbleHalf right = rumble_decode_half_precision_to_dual(rb + 4);

    // Classic clients have only weak/strong motors, not left/right precision
    // actuators. Combine both console Actuators by frequency band.
    uint8_t low  = std::max(left.low,  right.low);
    uint8_t high = std::max(left.high, right.high);

    // Fallback for any odd non-neutral packet our precision split does not understand.
    if (low == 0 && high == 0 &&
        !(rumble_half_is_all_zero(rb) || rumble_half_is_neutral_carrier(rb)) &&
        !(rumble_half_is_all_zero(rb + 4) || rumble_half_is_neutral_carrier(rb + 4))) {
        low = rumble_decode_half_to_u8(rb);
        high = rumble_decode_half_to_u8(rb + 4);
    }

    bool neutral = (low == 0 && high == 0);

    if (neutral && !publish_neutral)
        return;

    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    if (neutral && !g_ctx.clients[client_idx].rumble_active[sub_idx]) {
        // The console sends the neutral carrier constantly. Forwarding every
        // neutral frame creates haptic spam.
        return;
    }

    RumblePacket& ev = g_ctx.clients[client_idx].rumble[sub_idx];
    ev.magic = RUMBLE_MAGIC;
    ev.subpad = (uint8_t)sub_idx;
    ev.low_freq = neutral ? 0 : low;
    ev.high_freq = neutral ? 0 : high;
    // The compatibility testing sends rumble at report cadence.  Keep pulses short so
    // small precision packets do not smear into a long full-power buzz on classic clients.
    ev.duration_10ms = neutral ? 0 : 1;

    PrecisionRumblePacket& precision_ev = g_ctx.clients[client_idx].precision_rumble[sub_idx];
    precision_ev.magic = PRECISION_RUMBLE_MAGIC;
    precision_ev.subpad = (uint8_t)sub_idx;
    precision_ev.low_freq = ev.low_freq;
    precision_ev.high_freq = ev.high_freq;
    precision_ev.duration_10ms = ev.duration_10ms;
    memcpy(precision_ev.precision, rb, sizeof(precision_ev.precision));

    g_ctx.clients[client_idx].rumble_active[sub_idx] = !neutral;
    g_ctx.clients[client_idx].rumble_seq[sub_idx]++;

    if (g_ctx.verbose) {
        std::println("[rumble] client={} pad={} low={} high={} duration={} neutral={} raw={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    client_idx + 1, sub_idx + 1, ev.low_freq, ev.high_freq,
                    ev.duration_10ms, neutral ? "yes" : "no",
                    rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
    }
}
