#include "switch2_native.hpp"
#include "gadget_wakeup.hpp"
#include "s2_enumeration.hpp"
#include "s2_native_command.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <print>
#include <string>
#include <openssl/evp.h>
#include <linux/usb/ch9.h>

using namespace ns;

std::string s2_hex(std::span<const uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

namespace {

constexpr uint32_t FACTORY_BASE = 0x13000u;
constexpr uint32_t FACTORY_SIZE = 0x160u;
constexpr uint32_t USER_MOTION_CAL_BASE = 0x1FC000u;
// The console reads user motion calibration as TWO 0x40-byte blocks
// (0x1FC000 and 0x1FC040); the completed-calibration record (magic B2 A1)
// lives at 0x1FC060 on a real unit. The previous 0x40 window cut the second
// block off, so those reads returned unmapped 0xFF instead of stored data.
constexpr size_t USER_MOTION_CAL_SIZE = 0x80u;
constexpr const char* USER_MOTION_CAL_PATH = "/var/lib/ns-pc-control/switch2_motion_calibration.bin";
constexpr std::array<uint8_t, 8> USER_MOTION_CAL_MAGIC = {'N', 'S', '2', 'C', 'A', 'L', 2, 0};
constexpr uint8_t FEATURE_BUTTONS = 0x01;
constexpr uint8_t FEATURE_STICKS  = 0x02;
constexpr uint8_t FEATURE_IMU     = 0x04;
constexpr uint8_t FEATURE_MOUSE   = 0x10;
constexpr uint8_t FEATURE_RUMBLE  = 0x20;
constexpr uint8_t FEATURE_MAG     = 0x80;
constexpr uint32_t DEFAULT_FEATURE_MASK = FEATURE_BUTTONS | FEATURE_STICKS | FEATURE_IMU | FEATURE_RUMBLE; // 0x27
// Deliberately-current emulated FW/DSP/BT-patch values validated by the Raw Gadget probe.
constexpr std::array<uint8_t, 12> EMULATED_FIRMWARE_VERSION = {
    0x02, 0x09, 0x63, 0x02, 0x0C, 0x09, 0x09, 0x00, 0x00, 0x09, 0x09, 0x00
};

struct NativeState {
    bool streaming = false;
    uint8_t selected_report = 0x09;
    uint32_t feature_mask = DEFAULT_FEATURE_MASK;
    uint32_t enabled_features = 0;
    uint8_t stage = 0;
    std::array<uint8_t, 16> ltk{};
    std::array<uint8_t, FACTORY_SIZE> factory{};
    std::array<uint8_t, USER_MOTION_CAL_SIZE> user_motion_cal{};
    std::array<uint8_t, 64> identity{};
    std::array<uint8_t, 16> ctrl_info{};
    std::array<uint8_t, 9> pairing_info{};
};

bool state_is_joycon(const NativeState& s) {
    return s.selected_report == 0x07 || s.selected_report == 0x08
        || s.factory[0x14] == 0x66 || s.factory[0x14] == 0x67;
}

std::array<NativeState, HID_PORT_COUNT> g_state;
std::once_flag g_init_once;
std::mutex g_mtx;

bool bench_stream_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NS_S2_BENCH_STREAM");
        return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

constexpr std::array<uint8_t, 16> kDeviceKeyB1 = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
    0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10
};

void fac(NativeState& s, uint32_t addr, const uint8_t* data, size_t len) {
    if (addr < FACTORY_BASE) return;
    const uint32_t off = addr - FACTORY_BASE;
    if (off + len <= s.factory.size()) std::memcpy(s.factory.data() + off, data, len);
}

void fac_list(NativeState& s, uint32_t addr, std::initializer_list<uint8_t> data) {
    std::array<uint8_t, 64> tmp{};
    size_t n = std::min(tmp.size(), data.size());
    std::copy_n(data.begin(), n, tmp.begin());
    fac(s, addr, tmp.data(), n);
}

void build_factory(NativeState& s, int port) {
    static constexpr uint8_t blk[40] = {
        0x01, 0xAD, 0xD9, 0x9A, 0x55, 0x56, 0x65, 0xA0,
        0x00, 0x0A, 0xA0, 0x00, 0x0A, 0xE2, 0x20, 0x0E,
        0xE2, 0x20, 0x0E, 0x9A, 0xAD, 0xD9, 0x9A, 0xAD,
        0xD9, 0x0A, 0xA5, 0x50, 0x0A, 0xA5, 0x50, 0x2F,
        0xF6, 0x62, 0x2F, 0xF6, 0x62, 0x0A, 0xFF, 0xFF
    };

    // Unwritten flash reads back erased (0xFF) on the real controller, not
    // zero. Regions that must read as zero are written explicitly below.
    s.factory.fill(0xFF);
    const uint8_t suffix = static_cast<uint8_t>(0x30 + (port % 10));
    const uint8_t mac_last = static_cast<uint8_t>(0x3C + port);
    const uint8_t serial[16] = {
        0x48, 0x45, 0x4A, 0x37, 0x31, 0x30, 0x30, 0x31,
        0x31, 0x32, 0x31, 0x32, 0x34, suffix, 0x00, 0x00
    };

    s.ctrl_info = {0x01, 0x01, 0x05, 0x00, 0x00, 0x00, 0x0C, 0x00,
                   0x00, 0x00, 0x9E, 0x2B, 0xAB, 0xAB, 0xA9, mac_last};
    s.pairing_info = {0x01, 0x04, 0x01, 0x9E, 0x2B, 0xAB, 0xAB, 0xA9, mac_last};

    fac_list(s, 0x13000, {0x01, 0x00});
    fac(s, 0x13002, serial, sizeof(serial));
    fac_list(s, 0x13012, {0x7E, 0x05}); // VID 057E
    fac_list(s, 0x13014, {0x69, 0x20}); // PID 2069
    fac_list(s, 0x13016, {0x01, 0x06, 0x01});
    fac_list(s, 0x13019, {0x23, 0x23, 0x23});
    fac_list(s, 0x1301C, {0xA0, 0xA0, 0xA0});
    fac_list(s, 0x1301F, {0xE6, 0xE6, 0xE6});
    fac_list(s, 0x13022, {0x32, 0x32, 0x32});
    // IMU factory calibration reference (real PC2_Gyro_*.pcapng capture):
    // one float temperature reference followed by three float gyro offsets.
    fac_list(s, 0x13040, {0x16, 0xF4, 0xD3, 0x41, 0x48, 0xCE, 0x85, 0xBA,
                         0xF1, 0x05, 0x71, 0xBA, 0x1F, 0x27, 0xCB, 0x3B});
    // 0x13060..0x1307F reads back erased (0xFF) on the real unit — covered by
    // the global 0xFF fill above. (Captured read: addr=0x13060 len=0x20.)
    fac(s, 0x13080, blk, sizeof(blk));
    fac_list(s, 0x130A8, {0xA3, 0xA7, 0x87, 0x35, 0x06, 0x5F, 0x1C, 0xC6, 0x5C});
    fac(s, 0x130C0, blk, sizeof(blk));
    fac_list(s, 0x130E8, {0xB1, 0xA8, 0x83, 0xB6, 0x35, 0x5E, 0x27, 0x26, 0x64});
    // Motion scale calibration block read by the console during motion init
    // AND during the gyro-calibration flow (sub=04 addr=0x13100 len=0x18):
    // twelve zero bytes then three floats (0.0816, 0.01106, 10.01 on the
    // reference unit). This region was previously left zero-filled, so the
    // console received 0.0 for all three factors.
    fac_list(s, 0x13100, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x2D, 0x10, 0xA7, 0x3D,
                          0xE7, 0x49, 0x35, 0x3C,
                          0xA4, 0x2D, 0x20, 0x41});
    fac_list(s, 0x13140, {0x00, 0xD7, 0xA3, 0xBC, 0x41, 0xD7, 0xA3, 0xBC, 0x41});

    s.identity.fill(0xFF);
    std::memcpy(s.identity.data(), s.factory.data(), 0x25);
}

void load_user_motion_calibration() {
    for (auto& s : g_state) s.user_motion_cal.fill(0xFF);

    std::ifstream in(USER_MOTION_CAL_PATH, std::ios::binary);
    if (!in) return;

    std::array<uint8_t, 8> magic{};
    in.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != USER_MOTION_CAL_MAGIC) return;

    for (auto& s : g_state) {
        in.read(reinterpret_cast<char*>(s.user_motion_cal.data()),
                static_cast<std::streamsize>(s.user_motion_cal.size()));
        if (!in) {
            // A truncated/corrupt file must not leave a partially restored record.
            for (auto& reset : g_state) reset.user_motion_cal.fill(0xFF);
            return;
        }
    }
}

bool save_user_motion_calibration() {
    namespace fs = std::filesystem;
    const fs::path path(USER_MOTION_CAL_PATH);
    const fs::path temporary = path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(USER_MOTION_CAL_MAGIC.data()),
                  static_cast<std::streamsize>(USER_MOTION_CAL_MAGIC.size()));
        for (const auto& s : g_state) {
            out.write(reinterpret_cast<const char*>(s.user_motion_cal.data()),
                      static_cast<std::streamsize>(s.user_motion_cal.size()));
        }
        out.flush();
        if (!out) return false;
    }

    fs::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    return true;
}

void init_all() {
    for (size_t i = 0; i < g_state.size(); ++i) {
        auto& s = g_state[i];
        build_factory(s, static_cast<int>(i));
        s.streaming = false;
        s.selected_report = 0x09;
        s.feature_mask = DEFAULT_FEATURE_MASK;
        s.enabled_features = 0;
        s.stage = 0;
        s.ltk.fill(0);
    }
    load_user_motion_calibration();
}

NativeState& state_for_port(int port) {
    if (port < 0 || port >= HID_PORT_COUNT) port = 0;
    std::call_once(g_init_once, init_all);
    return g_state[port];
}

constexpr int active_native_ports() {
    return 1;
}

void propagate_primary_runtime_state_locked(int src_port) {
    // The composite S2 gadget is normally initialised through vendor interface
    // 0.  Older code mirrored *every* command from whichever interface spoke
    // last, so a partially initialised secondary interface could wipe the IMU
    // or streaming state of all other controllers. Only the primary interface
    // is allowed to publish device-wide runtime state.
    if (src_port != 0) return;
    const NativeState& src = g_state[src_port];
    const int ports = active_native_ports();
    for (int p = 0; p < ports; ++p) {
        if (p == src_port) continue;
        g_state[p].streaming = src.streaming;
        // Each HID interface emits the report belonging to its own logical
        // controller identity, even though the console may only send one
        // device-level start-streaming command on vendor interface 0.
        g_state[p].selected_report = switch2_input_report_id_for_port(p);
        g_state[p].feature_mask = src.feature_mask;
        g_state[p].enabled_features = src.enabled_features;
        g_state[p].stage = std::max(g_state[p].stage, src.stage);
    }
}

void rev16(const uint8_t* in, uint8_t* out) {
    for (int i = 0; i < 16; ++i) out[i] = in[15 - i];
}

void set_ltk_from_a1(NativeState& s, const uint8_t* a1_wire) {
    uint8_t a1r[16], b1r[16];
    rev16(a1_wire, a1r);
    rev16(kDeviceKeyB1.data(), b1r);
    for (int i = 0; i < 16; ++i) s.ltk[i] = static_cast<uint8_t>(a1r[i] ^ b1r[i]);
}

void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { std::memset(out, 0, 16); return; }
    int out_len1 = 0, out_len2 = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_EncryptUpdate(ctx, out, &out_len1, in, 16);
    EVP_EncryptFinal_ex(ctx, out + out_len1, &out_len2);
    EVP_CIPHER_CTX_free(ctx);
    (void)out_len2;
}

void answer_challenge(NativeState& s, const uint8_t* a2_wire, uint8_t* b2_wire) {
    uint8_t a2r[16];
    rev16(a2_wire, a2r);
    aes128_ecb_encrypt(s.ltk.data(), a2r, b2_wire);
}

void mem_read(const NativeState& s, uint32_t addr, uint8_t len, uint8_t* out) {
    for (uint8_t i = 0; i < len; ++i) {
        const uint32_t a = addr + i;
        if (a >= FACTORY_BASE && a < FACTORY_BASE + FACTORY_SIZE) out[i] = s.factory[a - FACTORY_BASE];
        else if (a >= USER_MOTION_CAL_BASE && a < USER_MOTION_CAL_BASE + USER_MOTION_CAL_SIZE)
            out[i] = s.user_motion_cal[a - USER_MOTION_CAL_BASE];
        else if (a == 0x1FA000) out[i] = 0x00;
        else out[i] = 0xFF;
    }
}

bool mem_write_user_motion_cal(NativeState& s, uint32_t addr,
                               std::span<const uint8_t> data) {
    bool changed = false;
    for (size_t i = 0; i < data.size(); ++i) {
        const uint64_t a = static_cast<uint64_t>(addr) + i;
        if (a < USER_MOTION_CAL_BASE || a >= USER_MOTION_CAL_BASE + USER_MOTION_CAL_SIZE) continue;
        uint8_t& dst = s.user_motion_cal[static_cast<size_t>(a - USER_MOTION_CAL_BASE)];
        if (dst != data[i]) {
            dst = data[i];
            changed = true;
        }
    }
    return changed;
}

void update_stage(NativeState& s, uint8_t id, uint8_t sub) {
    uint8_t stage = 4;
    if (id == 0x15 && sub == 0x02) stage = 5;
    else if (id == 0x15 && sub == 0x03) stage = 6;
    else if (id == 0x02 && (sub == 0x04 || sub == 0x01)) stage = 7;
    else if (id == 0x11) stage = 8;
    else if (id == 0x0C && (sub == 0x06 || sub == 0x04)) stage = 9;
    else if (id == 0x03 && sub == 0x0A) stage = 10;
    if (stage > s.stage) s.stage = stage;
}

uint32_t read_le32_at(std::span<const uint8_t> c, size_t off) {
    if (c.size() < off + 4) return 0;
    return static_cast<uint32_t>(c[off])
        | (static_cast<uint32_t>(c[off + 1]) << 8)
        | (static_cast<uint32_t>(c[off + 2]) << 16)
        | (static_cast<uint32_t>(c[off + 3]) << 24);
}

void set_feature_state(NativeState& s, uint8_t sub, uint32_t mask, ControllerRuntime& rt) {
    switch (sub) {
        case 0x02: s.feature_mask = mask; break;                 // set mask
        case 0x03: s.feature_mask &= ~mask; s.enabled_features &= ~mask; break; // clear mask
        case 0x04: s.enabled_features |= (mask & s.feature_mask); break;        // enable
        case 0x05: s.enabled_features &= ~mask; break;             // disable
        default: break;
    }
    rt.imu_enabled = (s.enabled_features & FEATURE_IMU) != 0;
    rt.vibration_enabled = (s.enabled_features & FEATURE_RUMBLE) != 0;
}

} // namespace

void switch2_native_init() {
    std::call_once(g_init_once, init_all);
}

void switch2_native_set_port_pid(int port, uint8_t pid_lo) {
    std::lock_guard<std::mutex> lk(g_mtx);
    NativeState& s = state_for_port(port);
    // 0x13014/0x13015 relative to FACTORY_BASE; ep0 identity mirrors the
    // first 0x25 factory bytes, so refresh it too.
    s.factory[0x14] = pid_lo;
    s.factory[0x15] = 0x20;
    std::memcpy(s.identity.data(), s.factory.data(), 0x25);
    s.selected_report = pid_lo == 0x67 ? 0x07
        : (pid_lo == 0x66 ? 0x08 : 0x09);
    if (pid_lo == 0x66 || pid_lo == 0x67) {
        // A Joy-Con 2 advertises the native mouse feature. Keep it in the
        // default mask so the console can enable bit 0x10 even if it does not
        // send an explicit full-mask command first.
        s.feature_mask |= FEATURE_MOUSE;
    } else {
        s.feature_mask &= ~FEATURE_MOUSE;
        s.enabled_features &= ~FEATURE_MOUSE;
    }

    // A secondary native interface may acquire its Joy-Con/Pro identity after
    // the primary interface has already completed the device-wide feature
    // negotiation. Inherit that negotiated state without copying the primary
    // report ID over the secondary controller's own report format.
    if (port > 0 && port < active_native_ports()) {
        const NativeState& primary = state_for_port(0);
        s.streaming = primary.streaming;
        s.feature_mask = primary.feature_mask;
        s.enabled_features = primary.enabled_features;
        s.stage = std::max(s.stage, primary.stage);
    }
    reset_s2_motion_state(port);
}

void switch2_native_reset_port(int port) {
    std::lock_guard<std::mutex> lk(g_mtx);
    NativeState& s = state_for_port(port);
    s.streaming = false;
    s.selected_report = 0x09;
    s.feature_mask = DEFAULT_FEATURE_MASK;
    s.enabled_features = 0;
    s.stage = 0;
    s.ltk.fill(0);
    reset_s2_motion_state(port);
}

bool switch2_native_handle_ep0_request(int port,
                                      const usb_ctrlrequest& ctrl,
                                      std::vector<uint8_t>& response,
                                      bool& status_only) {
    response.clear();
    status_only = false;
    const uint8_t bm = ctrl.bRequestType;
    const uint8_t req = ctrl.bRequest;
    const uint16_t length = le16toh(ctrl.wLength);
    const bool vendor = (bm & USB_TYPE_MASK) == USB_TYPE_VENDOR;
    if (!vendor) return false;

    std::lock_guard<std::mutex> lk(g_mtx);
    NativeState& s = state_for_port(port);
    if (s.stage < 4) s.stage = 4;

    if ((bm & USB_DIR_IN) && req == 0x03) {
        const size_t n = std::min<size_t>(length, s.identity.size());
        response.assign(s.identity.begin(), s.identity.begin() + n);
        return true;
    }
    if ((bm & USB_DIR_IN) && req == 0x02) {
        const size_t n = std::min<size_t>(length, s.ctrl_info.size());
        response.assign(s.ctrl_info.begin(), s.ctrl_info.begin() + n);
        return true;
    }
    if (!(bm & USB_DIR_IN) && req == 0x04) {
        status_only = true;
        return true;
    }
    return false;
}

bool switch2_native_handle_vendor_command(int port,
                                          std::span<const uint8_t> c,
                                          std::vector<uint8_t>& response,
                                          ControllerRuntime& rt) {
    response.clear();
    const S2StreamingCommandValidation streaming_command =
        validate_s2_streaming_command(c);
    if (streaming_command.status == S2StreamingCommandStatus::Truncated) {
        request_switch2_reenumeration(
            "truncated mandatory native streaming command 0x03/0x0A");
        return false;
    }
    if (streaming_command.status == S2StreamingCommandStatus::UnsupportedReportId) {
        if (g_ctx.verbose) {
            std::println(stderr,
                         "[s2][stream] rejected unsupported report id 0x{:02x}",
                         streaming_command.report_id);
        }
        request_switch2_reenumeration(
            "invalid report ID in mandatory native streaming command 0x03/0x0A");
        return false;
    }
    if (c.size() < 8) {
        if (g_ctx.verbose)
            std::println(stderr, "[s2][native][rx] ignored short optional packet port={} len={} raw={}",
                         port, c.size(), s2_hex(c));
        return false;
    }

    std::lock_guard<std::mutex> lk(g_mtx);
    NativeState& s = state_for_port(port);
    switch2_enumeration_native_handshake();
    const uint8_t id = c[0];
    const uint8_t transport = c[2];
    const uint8_t sub = c[3];
    if (id == 0x0D && sub != 0x01) {
        if (g_ctx.verbose) {
            std::println(stderr,
                         "[s2][firmware] rejecting update stage sub=0x{:02x} len={}; "
                         "emulated firmware already reports current",
                         sub, c.size());
        }
        return false;
    }
    update_stage(s, id, sub);

    static std::atomic<uint64_t> nfc_trace_counter{0};
    const uint64_t nfc_trace = id == 0x01
        ? nfc_trace_counter.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (g_ctx.verbose && id == 0x01) {
        const size_t declared = c[5];
        const size_t actual = c.size() - 8;
        std::println("[s2][nfc][rx] trace={} t_us={} port={} id=0x{:02x} "
                     "direction=0x{:02x} transport=0x{:02x} sub=0x{:02x} "
                     "declared_data_len={} actual_data_len={} length_match={} "
                     "total_len={} header={} data={} raw={}",
                     nfc_trace, now_us(), port, id, c[1], transport, sub,
                     declared, actual, declared == actual, c.size(),
                     s2_hex(c.first(8)), s2_hex(c.subspan(8)), s2_hex(c));
    }

    // NFC Read Buffer replies are 630 bytes in the real USB capture.
    // Use dynamic storage so the complete vendor transfer is queued intact.
    std::vector<uint8_t> r(8 + 622, 0);
    r[0] = id;
    r[1] = 0x01;
    r[2] = transport;
    r[3] = sub;
    r[4] = 0x00;
    r[5] = 0xF8;
    r[6] = 0x00;
    r[7] = 0x00;
    uint8_t* d = r.data() + 8;
    size_t dl = 0;

    switch (id) {
        case 0x03: // init/select report
            if (sub == 0x0D) { d[0] = 0x01; dl = 4; }
            else if (sub == 0x03) { d[0] = 0x01; dl = 4; }
            else if (sub == 0x0A) {
                // validate_s2_streaming_command() above guarantees both the
                // payload byte and a project-supported report ID are present.
                s.selected_report = streaming_command.report_id;
                if (g_ctx.verbose) {
                    std::println("[s2][stream] port={} console selected report=0x{:02x} "
                                 "(requested=0x{:02x})",
                                 port, s.selected_report, streaming_command.report_id);
                }
                s.streaming = true;
                rt.full_report_enabled = true;
                rt.input_report_mode = s.selected_report;
                switch2_enumeration_streaming_validated(s.selected_report);
                dl = 0;
            }
            break;
        case 0x07:
            d[0] = 0x00; dl = 1;
            break;
        case 0x16:
            dl = 24;
            break;
        case 0x15: // pairing over USB
            if (sub == 0x01) {
                std::memcpy(d, s.pairing_info.data(), s.pairing_info.size()); dl = s.pairing_info.size();
            } else if (sub == 0x02) {
                d[0] = 0x01;
                if (c.size() >= 25) answer_challenge(s, c.data() + 9, d + 1);
                dl = 17;
            } else if (sub == 0x03) {
                d[0] = 0x01; dl = 1;
            } else if (sub == 0x04) {
                d[0] = 0x01;
                if (c.size() >= 25) set_ltk_from_a1(s, c.data() + 9);
                std::memcpy(d + 1, kDeviceKeyB1.data(), kDeviceKeyB1.size());
                dl = 17;
            }
            break;
        case 0x09: // LEDs
            if (sub == 0x07 && c.size() > 8) {
                g_ctx.console_player_leds[port].store(c[8], std::memory_order_relaxed);
            }
            dl = 0;
            break;
        case 0x0C: { // feature select
            // Feature-select replies use the 10 78 status observed on real
            // Joy-Con 2 hardware, rather than the generic 00 F8 command ACK.
            r[4] = 0x10;
            r[5] = 0x78;
            if (sub == 0x01) {
                const uint8_t f = c.size() > 8 ? c[8] : 0;
                const bool joycon = state_is_joycon(s);
                d[4] = (f & FEATURE_BUTTONS) ? 0x07 : 0x00;
                d[5] = (f & FEATURE_STICKS)  ? 0x07 : 0x00;
                d[6] = (f & FEATURE_IMU)     ? (joycon ? 0x03 : 0x01) : 0x00;
                d[7] = (f & FEATURE_MAG)     ? (joycon ? 0x03 : 0x01) : 0x00;
                d[8] = (f & FEATURE_MOUSE)   ? (joycon ? 0x03 : 0x01) : 0x00;
                d[9] = (f & FEATURE_RUMBLE)  ? 0x03 : 0x00;
                dl = 12;
            } else if (sub == 0x06) {
                std::memset(d, 0, 40);
                if (c.size() > 12) d[4] = c[12];
                dl = 40;
            } else {
                const uint32_t mask = read_le32_at(c, 8);
                const bool mouse_was_enabled = (s.enabled_features & FEATURE_MOUSE) != 0;
                set_feature_state(s, sub, mask, rt);
                const bool mouse_is_enabled = (s.enabled_features & FEATURE_MOUSE) != 0;
                if (g_ctx.verbose && mouse_was_enabled != mouse_is_enabled) {
                    std::println("[s2][mouse] console feature {} on port {}",
                                 mouse_is_enabled ? "enabled" : "disabled", port);
                }
                dl = 4;
            }
            if (g_ctx.verbose) {
                std::println("[s2][feature][tx] port={} sub=0x{:02x} "
                             "mask=0x{:08x} enabled=0x{:08x} response={}",
                             port, sub, read_le32_at(c, 8), s.enabled_features,
                             s2_hex(std::span<const uint8_t>(r.data(), 8 + dl)));
            }
            break;
        }
        case 0x02: { // memory
            const uint32_t addr = read_le32_at(c, 12);
            // The real Pro Controller 2 acks every 0x02 read/write with the
            // ordinary 00 F8 status (PC2_Gyro_*.pcapng), including the
            // data-bearing flash replies. The earlier 10 78 override did not
            // match the captured hardware behaviour.
            if (sub == 0x04) {
                uint8_t len = c.size() > 8 ? c[8] : 0;
                if (len > 0x50) len = 0x50;
                d[0] = len;
                if (c.size() >= 16) std::memcpy(d + 4, c.data() + 12, 4);
                mem_read(s, addr, len, d + 8);
                dl = 8 + len;
            } else if (sub == 0x01) {
                d[0] = 0x40;
                if (c.size() >= 16) std::memcpy(d + 4, c.data() + 12, 4);
                mem_read(s, addr, 0x40, d + 8);
                dl = 8 + 0x40;
            } else if (sub == 0x05) {
                if (c.size() >= 16) std::memcpy(d + 4, c.data() + 12, 4);
                // Captured gyro calibration writes use a 72-byte command payload:
                // four opaque bytes, destination address 0x1FC000, then the 64-byte
                // user motion-calibration record. Reset-to-default writes all 0xFF;
                // a completed calibration begins B2 A1 and is otherwise currently
                // opaque. Preserve it byte-for-byte and let the report path remain
                // independent until the record's axis semantics are understood.
                if (c.size() > 16) {
                    const size_t declared_data = c.size() > 5 && c[5] >= 8
                        ? static_cast<size_t>(c[5] - 8) : 0;
                    const size_t available_data = c.size() - 16;
                    const size_t write_len = std::min(declared_data, available_data);
                    if (write_len != 0 && mem_write_user_motion_cal(s, addr, c.subspan(16, write_len))) {
                        if (!save_user_motion_calibration() && g_ctx.verbose) {
                            std::fprintf(stderr, "[s2] warning: could not persist motion calibration to %s\n",
                                         USER_MOTION_CAL_PATH);
                        }
                    }
                }
                dl = 8;
            }
            break;
        }
        case 0x10: {
            std::memcpy(d, EMULATED_FIRMWARE_VERSION.data(), EMULATED_FIRMWARE_VERSION.size());
            dl = EMULATED_FIRMWARE_VERSION.size();
            break;
        }
        case 0x0D:
            dl = 0;
            break;
        case 0x0B:
            if (sub == 0x03) { static constexpr uint8_t b[] = {0xA5, 0x0E, 0x00, 0x00}; std::memcpy(d, b, sizeof(b)); dl = sizeof(b); }
            else if (sub == 0x04) { static constexpr uint8_t b[] = {0x34, 0x00, 0x83, 0x00}; std::memcpy(d, b, sizeof(b)); dl = sizeof(b); }
            break;
        case 0x11:
            if (sub == 0x01) { d[0] = 0x03; dl = 4; }
            else if (sub == 0x03) {
                // IMU parameter block from the reference Pro Controller 2
                // (cmd 0x11 sub 0x03, read during the gyro-calibration flow).
                static constexpr uint8_t r11_03[29] = {
                    0x01, 0x20, 0x03, 0x00, 0x00, 0x0A, 0xE8, 0x1C, 0x3B, 0x79,
                    0x7D, 0x8B, 0x3A, 0x0A, 0xE8, 0x9C, 0x42, 0x58, 0xA0, 0x0B,
                    0x42, 0x0A, 0xE8, 0x9C, 0x41, 0x58, 0xA0, 0x0B, 0x41};
                std::memcpy(d, r11_03, sizeof(r11_03)); dl = sizeof(r11_03);
            }
            break;
        case 0x01: // NFC
            if (sub == 0x0C) {
                // Reference Pro Controller 2 answers 01/0C with the ordinary
                // 00 F8 ack and version bytes 61 12 50 10 (PC2_Gyro_*.pcapng).
                static constexpr uint8_t nfc[] = {0x61, 0x12, 0x50, 0x10};
                std::memcpy(d, nfc, sizeof(nfc));
                dl = sizeof(nfc);
            } else if (sub == 0x03 || sub == 0x04 || sub == 0x05
                    || sub == 0x06 || sub == 0x08 || sub == 0x14 || sub == 0x15
                    || sub == 0x1E || sub == 0x20 || sub == 0x21) {
                // The native S2 NFC transport uses the vendor command channel,
                // not the legacy 0x21 HID-subcommand wrapper. Keep the exact
                // eight-byte command header above and append the tag payload
                // produced by the shared Amiibo state machine.
                const std::span<const uint8_t> nfc_data = c.subspan(8);
                const bool v3_amiibo = is_v3_amiibo_placed(port);
                dl = fill_nfc_response_payload(sub, nfc_data, d, port);
                if (dl == 0 && v3_amiibo)
                    r[1] = 0x04;

                // Data replies retain the ordinary 00 F8 ACK. Figure-v3
                // header-only acknowledgements use direction 0x04, as observed
                // on a genuine Pro Controller 2.
            }
            break;
        case 0x18:
            if (sub == 0x01) { static constexpr uint8_t v[] = {0, 0, 0x40, 0xF0, 0, 0, 0x60, 0}; std::memcpy(d, v, sizeof(v)); dl = sizeof(v); }
            else if (sub == 0x03) { d[0] = c.size() > 8 ? c[8] : 0; dl = 1; }
            break;
        default:
            dl = 0; // 0x06 shutdown, 0x0A vibration and unknowns: bare ACK.
            break;
    }

    // There is exactly one native S2 function. The helper remains a no-op so
    // the state transition stays explicit.
    propagate_primary_runtime_state_locked(port);
    if (g_ctx.verbose && id == 0x03 && sub == 0x0A && s.streaming)
        std::fprintf(stdout, "[s2] native port 0 streaming enabled\n");

    response.assign(r.begin(), r.begin() + 8 + dl);
    if (g_ctx.verbose && id == 0x01) {
        std::println("[s2][nfc][tx-build] trace={} t_us={} port={} sub=0x{:02x} "
                     "request_data_len={} response_direction=0x{:02x} "
                     "response_len={} response_payload_len={} ack={:02x}{:02x} "
                     "header={} payload={} raw={}",
                     nfc_trace, now_us(), port, sub, c.size() - 8,
                     response.size() > 1 ? response[1] : 0, response.size(),
                     response.size() >= 8 ? response.size() - 8 : 0,
                     response.size() > 4 ? response[4] : 0,
                     response.size() > 5 ? response[5] : 0,
                     s2_hex(std::span<const uint8_t>(response.data(), std::min<size_t>(8, response.size()))),
                     response.size() > 8 ? s2_hex(std::span<const uint8_t>(response.data() + 8, response.size() - 8)) : std::string{},
                     s2_hex(response));
    }
    return true;
}

bool switch2_native_streaming_enabled(int port) {
    if (bench_stream_enabled()) return true;
    std::lock_guard<std::mutex> lk(g_mtx);
    return state_for_port(port).streaming;
}

uint8_t switch2_native_selected_report(int port) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return state_for_port(port).selected_report;
}

uint32_t switch2_native_enabled_features(int port) {
    if (bench_stream_enabled()) return DEFAULT_FEATURE_MASK;
    std::lock_guard<std::mutex> lk(g_mtx);
    return state_for_port(port).enabled_features;
}

void switch2_native_note_hid_out(int port, std::span<const uint8_t> report) {
    (void)port;
    (void)report;
}
