#include "gadget_wakeup.hpp"
#include "app_state.hpp"
#include "virtual_controller.hpp"
#include "switch2_native.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <endian.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <print>
#include <signal.h>
#include <sstream>
#include <span>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace ns;

constexpr const char* GADGET_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl";
constexpr const char* CONFIG_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1";
constexpr const char* GADGET_UDC_PATH = "/sys/kernel/config/usb_gadget/ns_ctrl/UDC";
constexpr const char* FFS_BASE_DIR = "/run/ns-pc-control/functionfs";
constexpr const char* FFS_INSTANCE_PREFIX = "ns_ctrl";

static std::string    g_saved_bt_mac;
static std::string    g_saved_bt_hci;
static std::string    g_switch2_wake_original_bt_mac;
static std::atomic<bool> g_bt_modified_for_wake{false};
static std::mutex     g_wake_bt_mtx;

// ===========================================================================
// String / validation helpers
// ===========================================================================

static std::string trim(std::string s) {
    s.erase(s.begin(),
            std::find_if(s.begin(), s.end(),
                         [](unsigned char ch) { return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch); }).base(),
            s.end());
    return s;
}

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string to_upper_no_space(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Extract printable hex byte pairs from a potentially decorated btmon line.
// Handles both compact "0A1B2C" strings and space-separated "0A 1B 2C" tokens.
static std::string to_upper_hex_bytes(std::string payload) {
    payload = trim(payload);

    // Fast path: already a compact all-hex string.
    std::string compact;
    compact.reserve(payload.size());
    for (char c : payload) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (!compact.empty() && compact.size() % 2 == 0
            && std::ranges::all_of(compact, [](char c) {
                   return std::isxdigit(static_cast<unsigned char>(c));
               })) {
        return compact;
    }

    // Slow path: extract individual two-char hex tokens from decorated output.
    std::ostringstream out;
    std::istringstream iss(payload);
    std::string tok;
    while (iss >> tok) {
        // Strip trailing punctuation (but not ':' which appears in MACs).
        while (!tok.empty()
                && std::ispunct(static_cast<unsigned char>(tok.back()))
                && tok.back() != ':') {
            tok.pop_back();
        }
        if (tok.size() == 2
                && std::isxdigit(static_cast<unsigned char>(tok[0]))
                && std::isxdigit(static_cast<unsigned char>(tok[1]))) {
            out << static_cast<char>(std::toupper(static_cast<unsigned char>(tok[0])))
                << static_cast<char>(std::toupper(static_cast<unsigned char>(tok[1])));
        }
    }
    return out.str();
}

static std::string bytes_to_hex(std::span<const uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

static bool valid_mac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (int i = 0; i < 17; ++i) {
        if (i % 3 == 2 ? mac[i] != ':' : !std::isxdigit(static_cast<unsigned char>(mac[i])))
            return false;
    }
    return true;
}

static bool valid_adv_hex(const std::string& adv) {
    return !adv.empty() && adv.size() % 2 == 0 && adv.size() <= 62
        && std::ranges::all_of(adv, [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c));
           });
}

static bool valid_hci(const std::string& hci) {
    return hci.size() >= 4
        && hci.starts_with("hci")
        && std::ranges::all_of(hci.begin() + 3, hci.end(), ::isdigit);
}

static bool command_exists(const char* cmd) {
    return std::system(std::format("command -v {} >/dev/null 2>&1", cmd).c_str()) == 0;
}

// ===========================================================================
// Gadget filesystem helpers
// ===========================================================================

bool hidg_nodes_ready() {
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (access(("/dev/hidg" + std::to_string(i)).c_str(), R_OK | W_OK) != 0)
            return false;
    }
    return true;
}

static bool mkdirs(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return !ec || ec.value() == EEXIST;
}

static bool write_file(const fs::path& p, const void* data, size_t len) {
    std::ofstream f(p, std::ios::binary);
    return f && f.write(static_cast<const char*>(data), len).good();
}

static bool write_file(const fs::path& p, const std::string& text) {
    return write_file(p, text.data(), text.size());
}

static std::string first_udc_name() {
    std::error_code ec;
    if (fs::exists("/sys/class/udc", ec)) {
        for (const auto& entry : fs::directory_iterator("/sys/class/udc", ec))
            return entry.path().filename().string();
    }
    return "";
}

namespace {

struct FfsPortState {
    int ep0_fd = -1;
    bool descriptors_written = false;
    bool host_enabled = false;
    bool host_suspended = false;
    uint8_t idle_rate = 0;
    uint8_t protocol = 1;
    std::deque<std::vector<uint8_t>> control_reports;

    int ep_in_fd = -1;   // ep1, HID interrupt IN
    int ep_out_fd = -1;  // ep2, HID interrupt OUT
    int ep_vendor_out_fd = -1; // ep3, S2 vendor bulk OUT
    int ep_vendor_in_fd  = -1; // ep4, S2 vendor bulk IN
    int ep_audio_out_fd  = -1; // ep5, S2 UAC1 playback OUT (Switch -> client)
    int ep_audio_in_fd   = -1; // ep6, S2 UAC1 microphone IN (client -> Switch)
    std::atomic<bool> io_running{false};
    std::atomic<bool> reader_exited{true};   // observed by stop to know when to stop signalling
    std::atomic<bool> writer_exited{true};
    std::atomic<bool> vendor_reader_exited{true};
    std::atomic<bool> vendor_writer_exited{true};
    std::atomic<bool> audio_reader_exited{true};
    std::atomic<bool> audio_writer_exited{true};
    bool input_write_seen = false;
    std::atomic<uint8_t> audio_playback_alt{0};
    std::atomic<uint8_t> audio_capture_alt{0};
    std::atomic<uint8_t> audio_playback_mute{0};
    std::atomic<uint8_t> audio_capture_mute{0};
    std::atomic<int16_t> audio_playback_volume{0};
    std::atomic<int16_t> audio_capture_volume{0};
    // Linear gain in unsigned Q16.16. Recomputed only when the host changes
    // UAC1 volume, so the 1 kHz audio path never calls pow()/exp().
    std::atomic<uint32_t> audio_playback_gain_q16{1u << 16};
    std::atomic<uint32_t> audio_capture_gain_q16{1u << 16};
    // Native S2 input reports are real-time state, not a reliable byte stream.
    // Keep a transport-side clock so timing is stamped when the report is
    // actually submitted to the USB endpoint, after any queued stale frames
    // have been coalesced.
    uint16_t s2_motion_tick = 0;
    uint64_t s2_motion_tick_fraction = 0;
    uint64_t s2_motion_last_write_us = 0;
    uint8_t s2_motion_last_report_id = 0;
    uint64_t s2_coalesced_input_reports = 0;
    std::thread reader_thread;               // blocking read(ep2) -> out_reports
    std::thread writer_thread;               // in_reports -> blocking write(ep1)
    std::thread vendor_reader_thread;        // blocking read(ep3) -> vendor_out_reports
    std::thread vendor_writer_thread;        // vendor_in_reports -> blocking write(ep4)
    std::thread audio_reader_thread;         // blocking read(ep5) -> console_audio_frames
    std::thread audio_writer_thread;         // microphone_frames -> blocking write(ep6)
    std::mutex out_mtx;
    std::deque<std::vector<uint8_t>> out_reports;  // host -> us (HID interrupt OUT)
    std::mutex in_mtx;
    std::condition_variable in_cv;
    std::deque<std::vector<uint8_t>> in_reports;   // us -> host (HID interrupt IN)
    std::mutex vendor_out_mtx;
    std::deque<std::vector<uint8_t>> vendor_out_reports;
    std::mutex vendor_in_mtx;
    std::condition_variable vendor_in_cv;
    std::deque<std::vector<uint8_t>> vendor_in_reports;
    std::mutex audio_out_mtx;
    std::condition_variable audio_out_cv;
    std::deque<std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES>> console_audio_frames;
    std::mutex audio_in_mtx;
    std::condition_variable audio_in_cv;
    std::deque<std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES>> microphone_frames;
};

std::array<FfsPortState, HID_PORT_COUNT> g_ffs_ports;

static bool functionfs_start_port_io(int id);
static void functionfs_stop_port_io(int id);

#pragma pack(push, 1)
struct HidDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wDescriptorLength;
};
#pragma pack(pop)

static void append_bytes(std::vector<uint8_t>& out, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

static void append_u16(std::vector<uint8_t>& out, uint16_t v) {
    uint16_t le = htole16(v);
    append_bytes(out, &le, sizeof(le));
}

static void append_u32(std::vector<uint8_t>& out, uint32_t v) {
    uint32_t le = htole32(v);
    append_bytes(out, &le, sizeof(le));
}

template <typename T>
static void append_obj(std::vector<uint8_t>& out, const T& obj) {
    append_bytes(out, &obj, sizeof(obj));
}

static std::string ffs_instance_name(int id) {
    return std::string(FFS_INSTANCE_PREFIX) + std::to_string(id);
}

static fs::path ffs_mount_dir(int id) {
    return fs::path(FFS_BASE_DIR) / ("port" + std::to_string(id));
}

static fs::path ffs_function_dir(int id) {
    return fs::path(GADGET_DIR) / "functions" / ("ffs." + ffs_instance_name(id));
}

static fs::path ffs_config_link(int id) {
    return fs::path(CONFIG_DIR) / ("ffs." + ffs_instance_name(id));
}

static bool path_is_mountpoint(const fs::path& p) {
    std::ifstream mounts("/proc/mounts");
    if (!mounts) return false;
    const std::string target = p.string();
    std::string src, mountpoint, type, rest;
    while (mounts >> src >> mountpoint >> type) {
        std::getline(mounts, rest);
        if (mountpoint == target && type == "functionfs") return true;
    }
    return false;
}

static bool gadget_uses_hori_identity() {
    return g_ctx.usb_controller_family == UsbControllerFamily::Hori;
}

static bool gadget_uses_switch2_identity() {
    return g_ctx.usb_controller_family == UsbControllerFamily::Switch2;
}

static constexpr int switch2_virtual_port_count() {
    // The device-recipient EP0 S2 handshake belongs to the USB device, not to
    // each FunctionFS interface. Keep exactly one native S2 controller.
    return 1;
}

static int legacy_hidg_node_count_for_family() {
    // A native S2 USB device is exposed as one controller only. Mixing S1 f_hid
    // interfaces into the same S2 device identity is not accepted reliably by
    // the console, so --s2 creates no legacy fallback nodes.
    return gadget_uses_switch2_identity() ? 0 : HID_PORT_COUNT;
}

static int functionfs_function_count_for_family() {
    // FunctionFS is now used only for native S2 controllers.
    // S1 and HORI go through the upstream/mainline f_hid /dev/hidg* path.
    if (gadget_uses_switch2_identity()) return switch2_virtual_port_count();
    return 0;
}

static int functionfs_virtual_port_count_for_family() {
    if (gadget_uses_switch2_identity()) return switch2_virtual_port_count();
    return 0;
}

static int s2_hid_endpoint_number_for_port(int port) {
    // One native Pro2 instance consumes two endpoint numbers:
    //   HID    IN/OUT = odd endpoint number
    //   vendor IN/OUT = even endpoint number
    return port * 2 + 1;
}

static int s2_vendor_endpoint_number_for_port(int port) {
    return port * 2 + 2;
}

static const char* gadget_id_vendor() {
    return gadget_uses_hori_identity() ? "0x0F0D" : "0x057e";
}

static const char* gadget_id_product() {
    if (gadget_uses_hori_identity()) return "0x0092";
    return gadget_uses_switch2_identity() ? "0x2069" : "0x2009";
}

static const char* gadget_bcd_device() {
    // Real Pro Controller 2 reports bcdDevice 4.00 (ndeadly descriptor dump).
    return gadget_uses_switch2_identity() ? "0x0400" : "0x0200";
}

static const char* gadget_product_string() {
    if (gadget_uses_hori_identity()) return "Legacy USB Gamepad";
    return gadget_uses_switch2_identity() ? "Switch 2 Pro Controller"
                                          : "Nintendo Switch Pro Controller";
}

static std::vector<uint8_t> ffs_report_descriptor(int id) {
    (void)id;
    if (gadget_uses_hori_identity()) {
        return std::vector<uint8_t>(LEGACY_REPORT_DESC, LEGACY_REPORT_DESC + sizeof(LEGACY_REPORT_DESC));
    }
    if (gadget_uses_switch2_identity()) {
        return std::vector<uint8_t>(S2_PRO_REPORT_DESC, S2_PRO_REPORT_DESC + S2_PRO_REPORT_DESC_SIZE);
    }
    return std::vector<uint8_t>(VIRTUAL_CONTROLLER_REPORT_DESC,
                                VIRTUAL_CONTROLLER_REPORT_DESC + VIRTUAL_CONTROLLER_REPORT_DESC_SIZE);
}

static HidDescriptor make_hid_descriptor(int id) {
    (void)id;
    HidDescriptor hid{};
    hid.bLength = sizeof(HidDescriptor);
    hid.bDescriptorType = 0x21;       // HID descriptor
    hid.bcdHID = htole16(0x0111);
    hid.bCountryCode = 0;
    hid.bNumDescriptors = 1;
    hid.bReportDescriptorType = 0x22; // Report descriptor
    if (gadget_uses_hori_identity()) {
        hid.wDescriptorLength = htole16(static_cast<uint16_t>(sizeof(LEGACY_REPORT_DESC)));
    } else if (gadget_uses_switch2_identity()) {
        hid.wDescriptorLength = htole16(static_cast<uint16_t>(S2_PRO_REPORT_DESC_SIZE));
    } else {
        hid.wDescriptorLength = htole16(static_cast<uint16_t>(VIRTUAL_CONTROLLER_REPORT_DESC_SIZE));
    }
    return hid;
}

static usb_interface_descriptor make_hid_interface_descriptor(uint8_t interface_number = 0) {
    usb_interface_descriptor intf{};
    intf.bLength = USB_DT_INTERFACE_SIZE;
    intf.bDescriptorType = USB_DT_INTERFACE;
    intf.bInterfaceNumber = interface_number;
    intf.bAlternateSetting = 0;
    intf.bNumEndpoints = 2;
    intf.bInterfaceClass = 0x03; // HID
    intf.bInterfaceSubClass = 0x00;
    intf.bInterfaceProtocol = 0x00;
    intf.iInterface = 1;
    return intf;
}

static usb_endpoint_descriptor_no_audio make_hid_endpoint_descriptor(int id, bool in, bool high_speed) {
    usb_endpoint_descriptor_no_audio ep{};
    ep.bLength = USB_DT_ENDPOINT_SIZE;
    ep.bDescriptorType = USB_DT_ENDPOINT;
    uint8_t ep_num = 0x01;
    if (gadget_uses_switch2_identity()) {
        ep_num = static_cast<uint8_t>(s2_hid_endpoint_number_for_port(id));
    }
    ep.bEndpointAddress = static_cast<uint8_t>((in ? USB_DIR_IN : USB_DIR_OUT) | ep_num);
    ep.bmAttributes = USB_ENDPOINT_XFER_INT;
    if (gadget_uses_hori_identity()) {
        ep.wMaxPacketSize = htole16(8);
    } else {
        ep.wMaxPacketSize = htole16(PRO_REPORT_SIZE); // 64 for S2 Pro/JC and modern per research
    }
    ep.bInterval = high_speed ? 4 : 4;
    return ep;
}

static void append_hid_function_descriptors(std::vector<uint8_t>& out, int id, bool high_speed, uint8_t interface_number = 0) {
    const auto intf = make_hid_interface_descriptor(interface_number);
    const auto hid = make_hid_descriptor(id);
    const auto ep_in = make_hid_endpoint_descriptor(id, true, high_speed);
    const auto ep_out = make_hid_endpoint_descriptor(id, false, high_speed);
    append_obj(out, intf);
    append_obj(out, hid);
    append_obj(out, ep_in);
    append_obj(out, ep_out);
}

static void append_s2_iad(std::vector<uint8_t>& out, uint8_t first_interface, uint8_t count,
                          uint8_t cls, uint8_t subcls, uint8_t proto) {
    out.push_back(0x08); // bLength
    out.push_back(0x0B); // Interface Association Descriptor
    out.push_back(first_interface);
    out.push_back(count);
    out.push_back(cls);
    out.push_back(subcls);
    out.push_back(proto);
    out.push_back(0x00);
}

static void append_s2_vendor_function_descriptors(std::vector<uint8_t>& out, int id, bool high_speed) {
    // Minimal native Pro Controller 2 function, following PicoSwitch2/ndeadly:
    // one HID interface for report 0x09/rumble 0x02 and one adjacent vendor
    // bulk interface for init/pairing/feature/memory commands.
    append_s2_iad(out, 0, 1, 0x03, 0x00, 0x00);
    append_hid_function_descriptors(out, id, high_speed, 0);

    append_s2_iad(out, 1, 1, 0xFF, 0x00, 0x00);
    usb_interface_descriptor vendor_intf{};
    vendor_intf.bLength = USB_DT_INTERFACE_SIZE;
    vendor_intf.bDescriptorType = USB_DT_INTERFACE;
    vendor_intf.bInterfaceNumber = 1;
    vendor_intf.bAlternateSetting = 0;
    vendor_intf.bNumEndpoints = 2;
    vendor_intf.bInterfaceClass = 0xFF;
    vendor_intf.bInterfaceSubClass = 0x00;
    vendor_intf.bInterfaceProtocol = 0x00;
    vendor_intf.iInterface = 0;
    append_obj(out, vendor_intf);

    // The real Pro Controller 2 is a full-speed device with 64-byte bulk
    // endpoints. The USB spec pins high-speed bulk to exactly 512, so the HS
    // variant must differ; the gadget also requests max_speed=full-speed to
    // keep the console on the hardware-faithful FS descriptors.
    const uint16_t bulk_mps = static_cast<uint16_t>(high_speed ? 512 : PRO_REPORT_SIZE);

    usb_endpoint_descriptor_no_audio vendor_out{};
    vendor_out.bLength = USB_DT_ENDPOINT_SIZE;
    vendor_out.bDescriptorType = USB_DT_ENDPOINT;
    vendor_out.bEndpointAddress = static_cast<uint8_t>(s2_vendor_endpoint_number_for_port(id));
    vendor_out.bmAttributes = USB_ENDPOINT_XFER_BULK;
    vendor_out.wMaxPacketSize = htole16(bulk_mps);
    vendor_out.bInterval = 0;
    append_obj(out, vendor_out);

    usb_endpoint_descriptor_no_audio vendor_in{};
    vendor_in.bLength = USB_DT_ENDPOINT_SIZE;
    vendor_in.bDescriptorType = USB_DT_ENDPOINT;
    vendor_in.bEndpointAddress = static_cast<uint8_t>(USB_DIR_IN | s2_vendor_endpoint_number_for_port(id));
    vendor_in.bmAttributes = USB_ENDPOINT_XFER_BULK;
    vendor_in.wMaxPacketSize = htole16(bulk_mps);
    vendor_in.bInterval = 0;
    append_obj(out, vendor_in);
}

static void append_s2_audio_function_descriptors(std::vector<uint8_t>& out) {
    // Exact USB Audio Class 1.0 topology exposed by the real Pro Controller 2:
    // AudioControl interface 2, headphone playback on interface 3 / ep 0x03,
    // and headset microphone capture on interface 4 / ep 0x83. Both streams
    // are synchronous stereo S16LE at 48 kHz with one 192-byte USB frame per ms.
    static constexpr uint8_t descriptors[] = {
        // IAD: interfaces 2..4 are one audio function.
        0x08, 0x0B, 0x02, 0x03, 0x01, 0x01, 0x00, 0x00,
        // Interface 2, AudioControl.
        0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
        // Class-specific AC header, total 71 bytes, streaming IFs 3 and 4.
        0x0A, 0x24, 0x01, 0x00, 0x01, 0x47, 0x00, 0x02, 0x03, 0x04,
        // USB streaming input terminal -> playback feature unit -> headphones.
        0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
        0x0A, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00,
        0x09, 0x24, 0x03, 0x03, 0x02, 0x03, 0x00, 0x02, 0x00,
        // Microphone terminal -> capture feature unit -> USB streaming output.
        0x0C, 0x24, 0x02, 0x04, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x03, 0x00, 0x00,
        0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x00, 0x05, 0x00,
        // Interface 3 alt 0: playback disabled.
        0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
        // Interface 3 alt 1: playback streaming.
        0x09, 0x04, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
        0x07, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00,
        0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,
        0x07, 0x05, 0x03, 0x0D, 0xC0, 0x00, 0x01,
        0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
        // Interface 4 alt 0: microphone disabled.
        0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
        // Interface 4 alt 1: microphone streaming. The real descriptor advertises
        // two PCM channels here even though its physical terminal is mono.
        0x09, 0x04, 0x04, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
        0x07, 0x24, 0x01, 0x06, 0x00, 0x01, 0x00,
        0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,
        0x07, 0x05, 0x83, 0x0D, 0xC0, 0x00, 0x01,
        0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    append_bytes(out, descriptors, sizeof(descriptors));
}

static std::vector<uint8_t> build_functionfs_descriptors(int id) {
    std::vector<uint8_t> out;
    append_u32(out, FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    const size_t length_pos = out.size();
    append_u32(out, 0); // patched below
    // ALL_CTRL_RECIP is required for S2: the console's identity handshake is
    // device-recipient vendor EP0 traffic (bmRequestType 0xC0/0x40), which
    // FunctionFS otherwise rejects with a STALL before userspace ever sees it
    // (the console then goes silent right after SET_CONFIGURATION).
    uint32_t ffs_flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;
    if (gadget_uses_switch2_identity()) ffs_flags |= FUNCTIONFS_ALL_CTRL_RECIP;
    append_u32(out, ffs_flags);
    if (gadget_uses_switch2_identity()) {
        // 30 descriptors: 9 HID/vendor descriptors plus the real controller's
        // 21 AudioControl/AudioStreaming descriptors. The kernel cross-checks
        // this count against the blob length and rejects mismatches with EINVAL.
        append_u32(out, 30); // FS
        append_u32(out, 30); // HS: same topology; gadget is capped at full speed
        append_s2_vendor_function_descriptors(out, id, false);
        append_s2_audio_function_descriptors(out);
        append_s2_vendor_function_descriptors(out, id, true);
        append_s2_audio_function_descriptors(out);
    } else {
        append_u32(out, 4); // FS: interface + HID + IN ep + OUT ep
        append_u32(out, 4); // HS: interface + HID + IN ep + OUT ep
        append_hid_function_descriptors(out, id, false);
        append_hid_function_descriptors(out, id, true);
    }
    const uint32_t len = htole32(static_cast<uint32_t>(out.size()));
    std::memcpy(out.data() + length_pos, &len, sizeof(len));
    return out;
}

static std::vector<uint8_t> build_functionfs_strings() {
    static constexpr char kInterfaceName[] = "NS-PC-Control HID";
    std::vector<uint8_t> out;
    append_u32(out, FUNCTIONFS_STRINGS_MAGIC);
    const size_t length_pos = out.size();
    append_u32(out, 0); // patched below
    append_u32(out, 1); // str_count
    append_u32(out, 1); // lang_count
    append_u16(out, 0x0409);
    append_bytes(out, kInterfaceName, sizeof(kInterfaceName));
    const uint32_t len = htole32(static_cast<uint32_t>(out.size()));
    std::memcpy(out.data() + length_pos, &len, sizeof(len));
    return out;
}

static bool write_all_fd(int fd, const uint8_t* data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, data + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd{fd, POLLOUT, 0};
                if (poll(&pfd, 1, 50) <= 0) return false;
                continue;
            }
            return false;
        }
        if (w == 0) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, 50) <= 0) return false;
            continue;
        }
        done += static_cast<size_t>(w);
    }
    return true;
}

static bool write_all_fd(int fd, const std::vector<uint8_t>& data) {
    return write_all_fd(fd, data.data(), data.size());
}

static void close_functionfs_ep0s() {
    for (int i = 0; i < HID_PORT_COUNT; ++i) functionfs_stop_port_io(i);
    for (auto& p : g_ffs_ports) {
        if (p.ep0_fd >= 0) close(p.ep0_fd);
        p.ep0_fd = -1;
        p.descriptors_written = false;
        p.host_enabled = false;
        p.host_suspended = false;
        p.control_reports.clear();
        p.idle_rate = 0;
        p.protocol = 1;
    }
    g_ctx.functionfs_transport_active.store(false, std::memory_order_relaxed);
}

static bool mount_functionfs_instance(int id) {
    std::error_code ec;
    fs::create_directories(ffs_mount_dir(id), ec);
    if (ec) return false;

    if (!path_is_mountpoint(ffs_mount_dir(id))) {
        if (mount(ffs_instance_name(id).c_str(), ffs_mount_dir(id).c_str(), "functionfs", 0, nullptr) != 0) {
            if (errno != EBUSY) return false;
        }
    }
    return true;
}

static bool prepare_functionfs_instance(int id) {
    if (!mount_functionfs_instance(id)) return false;

    auto& st = g_ffs_ports[id];
    if (st.ep0_fd >= 0) {
        close(st.ep0_fd);
        st.ep0_fd = -1;
        st.descriptors_written = false;
    }

    const fs::path ep0 = ffs_mount_dir(id) / "ep0";
    st.ep0_fd = open(ep0.c_str(), O_RDWR | O_NONBLOCK);
    if (st.ep0_fd < 0) return false;

    const auto descs = build_functionfs_descriptors(id);
    const auto strings = build_functionfs_strings();
    if (!write_all_fd(st.ep0_fd, descs) || !write_all_fd(st.ep0_fd, strings)) {
        close(st.ep0_fd);
        st.ep0_fd = -1;
        return false;
    }
    st.descriptors_written = true;
    return true;
}

static bool create_functionfs_function(int id) {
    fs::path func = ffs_function_dir(id);
    if (!mkdirs(func)) return false;
    if (!prepare_functionfs_instance(id)) return false;

    fs::path link_path = ffs_config_link(id);
    std::error_code ec;
    fs::remove(link_path, ec);
    return symlink(func.c_str(), link_path.c_str()) == 0;
}

static void unmount_functionfs_instances() {
    close_functionfs_ep0s();
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        const fs::path dir = ffs_mount_dir(i);
        if (path_is_mountpoint(dir)) umount2(dir.c_str(), MNT_DETACH);
    }
}

static bool read_ep0_payload(int fd, std::vector<uint8_t>& payload, size_t len) {
    payload.assign(len, 0);
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, payload.data() + done, len - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd{fd, POLLIN, 20};
                if (poll(&pfd, 1, 20) <= 0) return false;
                continue;
            }
            return false;
        }
        if (r == 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

// FunctionFS ep0 semantics: a wrong-direction I/O on a pending setup is how
// userspace requests a STALL. The status stage of an IN setup is completed by
// the data write; a zero-length OUT setup must be acked with a zero-length
// READ — writing there stalls the request (the console retries 4x, then
// silently abandons the controller).
static bool ep0_write_status(int fd) {
    for (int attempts = 0; attempts < 4; ++attempts) {
        ssize_t w = write(fd, "", 0);
        if (w >= 0) return true;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, 50) > 0) continue;
        }
        return false;
    }
    return false;
}

static bool ep0_read_status(int fd) {
    for (int attempts = 0; attempts < 4; ++attempts) {
        ssize_t r = read(fd, nullptr, 0);
        if (r >= 0) return true;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd{fd, POLLIN, 0};
            if (poll(&pfd, 1, 50) > 0) continue;
        }
        return false;
    }
    return false;
}

// Ack a setup with no data stage to transfer, honoring the setup's direction.
static bool ep0_ack_status(int fd, bool dir_in) {
    return dir_in ? ep0_write_status(fd) : ep0_read_status(fd);
}

// FunctionFS requests a control-pipe STALL by performing I/O in the wrong
// direction for the pending setup packet.
static bool ep0_stall_setup(int fd, bool dir_in) {
    return dir_in ? ep0_read_status(fd) : ep0_write_status(fd);
}

static bool ep0_write_data(int fd, const uint8_t* data, size_t len, size_t limit) {
    len = std::min(len, limit);
    if (len == 0) return ep0_write_status(fd);
    return write_all_fd(fd, data, len);
}

constexpr int16_t S2_UAC_VOLUME_MIN_256DB = static_cast<int16_t>(-64 * 256);
constexpr int16_t S2_UAC_VOLUME_MAX_256DB = 0;
constexpr int16_t S2_UAC_VOLUME_RES_256DB = 256;
constexpr int16_t S2_UAC_VOLUME_SILENCE = std::numeric_limits<int16_t>::min();
constexpr uint32_t S2_UAC_GAIN_Q16_UNITY = 1u << 16;

static uint32_t uac1_volume_to_gain_q16(int16_t volume_256db) {
    if (volume_256db == S2_UAC_VOLUME_SILENCE) return 0;
    const int clamped = std::clamp<int>(volume_256db,
                                        S2_UAC_VOLUME_MIN_256DB,
                                        S2_UAC_VOLUME_MAX_256DB);
    if (clamped == 0) return S2_UAC_GAIN_Q16_UNITY;
    const double db = static_cast<double>(clamped) / 256.0;
    const double linear = std::pow(10.0, db / 20.0);
    return static_cast<uint32_t>(std::clamp<long long>(
        std::llround(linear * static_cast<double>(S2_UAC_GAIN_Q16_UNITY)),
        0,
        S2_UAC_GAIN_Q16_UNITY));
}

static int16_t normalize_uac1_volume(int16_t volume_256db) {
    if (volume_256db == S2_UAC_VOLUME_SILENCE) return volume_256db;
    return static_cast<int16_t>(std::clamp<int>(volume_256db,
                                                S2_UAC_VOLUME_MIN_256DB,
                                                S2_UAC_VOLUME_MAX_256DB));
}

static void store_uac1_volume(std::atomic<int16_t>& volume,
                              std::atomic<uint32_t>& gain_q16,
                              int16_t requested_256db) {
    const int16_t normalized = normalize_uac1_volume(requested_256db);
    volume.store(normalized, std::memory_order_release);
    gain_q16.store(uac1_volume_to_gain_q16(normalized), std::memory_order_release);
}

static std::string uac1_volume_text(int16_t volume_256db) {
    if (volume_256db == S2_UAC_VOLUME_SILENCE) return "-inf dB";
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.2f dB",
                  static_cast<double>(volume_256db) / 256.0);
    return text;
}

static void queue_control_report(int id, uint16_t w_value, const std::vector<uint8_t>& payload) {
    if (id < 0 || id >= HID_PORT_COUNT || payload.empty()) return;
    auto report = payload;
    const uint8_t report_id = static_cast<uint8_t>(w_value & 0xFF);
    // HID SET_REPORT carries its report ID in wValue; the EP0 payload does
    // not reliably contain it. Always preserve that framing instead of
    // guessing from payload[0] (command IDs can equal the output report ID).
    if (report_id != 0) report.insert(report.begin(), report_id);
    auto& q = g_ffs_ports[id].control_reports;
    if (q.size() >= 16) q.pop_front();
    q.push_back(std::move(report));
}

static bool handle_s2_audio_control_request(FfsPortState& st, const usb_ctrlrequest& ctrl) {
    if (!gadget_uses_switch2_identity()) return false;
    const uint8_t bm = ctrl.bRequestType;
    if ((bm & USB_TYPE_MASK) != USB_TYPE_CLASS ||
        (bm & USB_RECIP_MASK) != USB_RECIP_INTERFACE) {
        return false;
    }

    const uint16_t index = le16toh(ctrl.wIndex);
    const uint8_t interface_number = static_cast<uint8_t>(index & 0xFFu);
    if (interface_number != 2) return false;

    const uint8_t entity = static_cast<uint8_t>(index >> 8);
    const uint16_t value = le16toh(ctrl.wValue);
    const uint8_t selector = static_cast<uint8_t>(value >> 8);
    const uint8_t channel = static_cast<uint8_t>(value & 0xFFu);
    const uint8_t req = ctrl.bRequest;
    const uint16_t length = le16toh(ctrl.wLength);
    const bool dir_in = (bm & USB_DIR_IN) != 0;

    std::atomic<uint8_t>* mute = nullptr;
    std::atomic<int16_t>* volume = nullptr;
    std::atomic<uint32_t>* gain_q16 = nullptr;
    const char* direction_name = nullptr;
    if (entity == 2) {
        mute = &st.audio_playback_mute;
        volume = &st.audio_playback_volume;
        gain_q16 = &st.audio_playback_gain_q16;
        direction_name = "playback";
    } else if (entity == 5) {
        mute = &st.audio_capture_mute;
        volume = &st.audio_capture_volume;
        gain_q16 = &st.audio_capture_gain_q16;
        direction_name = "microphone";
    } else {
        if (g_ctx.verbose) {
            std::println("[s2][audio-control] stall unknown feature unit entity={}", entity);
        }
        ep0_stall_setup(st.ep0_fd, dir_in);
        return true;
    }

    // The Nintendo descriptors expose mute+volume only on the master channel.
    // UAC1 requires unsupported channel/control combinations to STALL.
    if (channel != 0) {
        if (g_ctx.verbose) {
            std::println("[s2][audio-control] stall {} unsupported channel={}",
                         direction_name, channel);
        }
        ep0_stall_setup(st.ep0_fd, dir_in);
        return true;
    }

    constexpr uint8_t UAC_SET_CUR = 0x01;
    constexpr uint8_t UAC_GET_CUR = 0x81;
    constexpr uint8_t UAC_GET_MIN = 0x82;
    constexpr uint8_t UAC_GET_MAX = 0x83;
    constexpr uint8_t UAC_GET_RES = 0x84;
    constexpr uint8_t UAC_FU_MUTE = 0x01;
    constexpr uint8_t UAC_FU_VOLUME = 0x02;

    if (!dir_in && req == UAC_SET_CUR) {
        const size_t expected = selector == UAC_FU_MUTE ? 1u
                              : selector == UAC_FU_VOLUME ? 2u
                              : 0u;
        if (expected == 0 || length != expected) {
            if (g_ctx.verbose) {
                std::println("[s2][audio-control] stall {} SET_CUR selector={} length={}",
                             direction_name, selector, length);
            }
            ep0_stall_setup(st.ep0_fd, false);
            return true;
        }

        std::vector<uint8_t> payload;
        if (!read_ep0_payload(st.ep0_fd, payload, expected)) return true;

        if (selector == UAC_FU_MUTE) {
            const uint8_t next = payload[0] ? 1 : 0;
            mute->store(next, std::memory_order_release);
            if (g_ctx.verbose) {
                std::println("[s2][audio-control] {} mute={}", direction_name, next != 0);
            }
            return true;
        }

        const uint16_t raw = static_cast<uint16_t>(payload[0]) |
                             (static_cast<uint16_t>(payload[1]) << 8);
        const int16_t requested = static_cast<int16_t>(raw);
        store_uac1_volume(*volume, *gain_q16, requested);
        if (g_ctx.verbose) {
            const int16_t stored = volume->load(std::memory_order_acquire);
            std::println("[s2][audio-control] {} volume={}",
                         direction_name, uac1_volume_text(stored));
        }
        return true;
    }

    if (dir_in) {
        if (selector == UAC_FU_MUTE) {
            // UAC1 mute has only CUR; GET_MIN/MAX/RES must STALL.
            if (req != UAC_GET_CUR || length != 1) {
                if (g_ctx.verbose) {
                    std::println("[s2][audio-control] stall {} mute request={:#04x} length={}",
                                 direction_name, req, length);
                }
                ep0_stall_setup(st.ep0_fd, true);
                return true;
            }
            const uint8_t response = mute->load(std::memory_order_acquire) ? 1 : 0;
            ep0_write_data(st.ep0_fd, &response, sizeof(response), length);
            return true;
        }

        if (selector == UAC_FU_VOLUME) {
            if (length != 2 ||
                (req != UAC_GET_CUR && req != UAC_GET_MIN &&
                 req != UAC_GET_MAX && req != UAC_GET_RES)) {
                if (g_ctx.verbose) {
                    std::println("[s2][audio-control] stall {} volume request={:#04x} length={}",
                                 direction_name, req, length);
                }
                ep0_stall_setup(st.ep0_fd, true);
                return true;
            }

            int16_t response = 0;
            if (req == UAC_GET_CUR) {
                response = volume->load(std::memory_order_acquire);
            } else if (req == UAC_GET_MIN) {
                response = S2_UAC_VOLUME_MIN_256DB;
            } else if (req == UAC_GET_MAX) {
                response = S2_UAC_VOLUME_MAX_256DB;
            } else {
                response = S2_UAC_VOLUME_RES_256DB;
            }
            const uint16_t raw = htole16(static_cast<uint16_t>(response));
            ep0_write_data(st.ep0_fd,
                           reinterpret_cast<const uint8_t*>(&raw),
                           sizeof(raw),
                           length);
            return true;
        }
    }

    if (g_ctx.verbose) {
        std::println("[s2][audio-control] stall {} unsupported request={:#04x} selector={}",
                     direction_name, req, selector);
    }
    ep0_stall_setup(st.ep0_fd, dir_in);
    return true;
}

static void handle_functionfs_setup(int id, const usb_ctrlrequest& ctrl) {
    if (id < 0 || id >= HID_PORT_COUNT) return;
    auto& st = g_ffs_ports[id];
    const uint8_t bm = ctrl.bRequestType;
    const uint8_t req = ctrl.bRequest;
    const uint16_t value = le16toh(ctrl.wValue);
    const uint16_t length = le16toh(ctrl.wLength);
    const uint8_t desc_type = static_cast<uint8_t>(value >> 8);
    const bool dir_in = (bm & USB_DIR_IN) != 0;
    const int target_port = id;

    if (handle_s2_audio_control_request(st, ctrl)) return;

    if (gadget_uses_switch2_identity() && (bm & USB_TYPE_MASK) == USB_TYPE_VENDOR) {
        if (g_ctx.verbose)
            std::println("[s2] ep0 vendor request: bmRequestType={:#04x} bRequest={:#04x} wValue={:#06x} wIndex={:#06x} wLength={}",
                         bm, req, value, le16toh(ctrl.wIndex), length);
        std::vector<uint8_t> response;
        bool status_only = false;
        if (switch2_native_handle_ep0_request(id, ctrl, response, status_only)) {
            (void)status_only;
            if (!dir_in) {
                // Reading the data stage (or a zero-length read for wLength=0)
                // completes an OUT setup; writing here would STALL it.
                if (length > 0) {
                    std::vector<uint8_t> discard;
                    read_ep0_payload(st.ep0_fd, discard, length);
                } else {
                    ep0_read_status(st.ep0_fd);
                }
            } else {
                ep0_write_data(st.ep0_fd, response.data(), response.size(), length);
            }
            return;
        }
    }

    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && dir_in && req == USB_REQ_GET_DESCRIPTOR) {
        if (desc_type == 0x22) { // HID report descriptor
            const auto report = ffs_report_descriptor(target_port);
            ep0_write_data(st.ep0_fd, report.data(), report.size(), length);
            return;
        }
        if (desc_type == 0x21) { // HID descriptor
            const auto hid = make_hid_descriptor(target_port);
            ep0_write_data(st.ep0_fd, reinterpret_cast<const uint8_t*>(&hid), sizeof(hid), length);
            return;
        }
    }

    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
        if (dir_in && req == USB_REQ_GET_STATUS) {
            const uint8_t status[2]{};
            ep0_write_data(st.ep0_fd, status, sizeof(status), length);
            return;
        }
        if (dir_in && req == USB_REQ_GET_INTERFACE) {
            const uint8_t interface_number = static_cast<uint8_t>(le16toh(ctrl.wIndex) & 0xFFu);
            uint8_t alt_setting = 0;
            if (interface_number == 3) alt_setting = st.audio_playback_alt.load(std::memory_order_relaxed);
            else if (interface_number == 4) alt_setting = st.audio_capture_alt.load(std::memory_order_relaxed);
            ep0_write_data(st.ep0_fd, &alt_setting, 1, length);
            return;
        }
        if (!dir_in && req == USB_REQ_SET_INTERFACE) {
            const uint8_t interface_number = static_cast<uint8_t>(le16toh(ctrl.wIndex) & 0xFFu);
            const uint8_t alt_setting = static_cast<uint8_t>(value & 0xFFu);
            if ((interface_number == 3 || interface_number == 4) && alt_setting <= 1) {
                if (interface_number == 3) {
                    st.audio_playback_alt.store(alt_setting, std::memory_order_relaxed);
                    if (alt_setting == 0) {
                        std::lock_guard<std::mutex> lk(st.audio_out_mtx);
                        st.console_audio_frames.clear();
                        st.audio_out_cv.notify_all();
                    }
                } else {
                    st.audio_capture_alt.store(alt_setting, std::memory_order_relaxed);
                    if (alt_setting == 0) {
                        std::lock_guard<std::mutex> lk(st.audio_in_mtx);
                        st.microphone_frames.clear();
                    }
                    st.audio_in_cv.notify_all();
                }
                if (g_ctx.verbose) {
                    std::println("[s2][audio] USB interface {} alternate setting {}",
                                 interface_number, alt_setting);
                }
                ep0_ack_status(st.ep0_fd, false);
            } else {
                // Ignore unsupported alternate settings without mutating the
                // active stream state. FunctionFS/composite owns endpoint setup.
                ep0_ack_status(st.ep0_fd, false);
            }
            return;
        }
        if (!dir_in && length > 0) {
            std::vector<uint8_t> discard;
            read_ep0_payload(st.ep0_fd, discard, length);
            return;
        }
        if (dir_in && length > 0) {
            // Be conservative for rare standard IN requests that FunctionFS passes
            // through for this interface. Returning a short zeroed response is less
            // disruptive than leaving EP0 unanswered.
            std::vector<uint8_t> zeros(length, 0);
            ep0_write_data(st.ep0_fd, zeros.data(), zeros.size(), length);
            return;
        }
        // SET_INTERFACE / CLEAR_FEATURE / SET_FEATURE etc. are safe to ack here
        // because the composite core owns the real configuration state.
        ep0_ack_status(st.ep0_fd, dir_in);
        return;
    }

    if ((bm & USB_TYPE_MASK) == USB_TYPE_CLASS) {
        switch (req) {
            case 0x01: { // GET_REPORT
                std::vector<uint8_t> zeros(std::max<uint16_t>(1, length), 0);
                if ((value & 0xFF) != 0) zeros[0] = static_cast<uint8_t>(value & 0xFF);
                ep0_write_data(st.ep0_fd, zeros.data(), zeros.size(), length);
                return;
            }
            case 0x02: { // GET_IDLE
                const uint8_t idle = st.idle_rate;
                ep0_write_data(st.ep0_fd, &idle, 1, length);
                return;
            }
            case 0x03: { // GET_PROTOCOL
                const uint8_t proto = st.protocol;
                ep0_write_data(st.ep0_fd, &proto, 1, length);
                return;
            }
            case 0x09: { // SET_REPORT
                if (length > 0) {
                    std::vector<uint8_t> payload;
                    if (read_ep0_payload(st.ep0_fd, payload, length)) queue_control_report(target_port, value, payload);
                    return;
                }
                ep0_read_status(st.ep0_fd);
                return;
            }
            case 0x0A: // SET_IDLE
                st.idle_rate = static_cast<uint8_t>(value >> 8);
                ep0_read_status(st.ep0_fd);
                return;
            case 0x0B: // SET_PROTOCOL
                st.protocol = static_cast<uint8_t>(value & 0xFF);
                ep0_read_status(st.ep0_fd);
                return;
            default:
                ep0_ack_status(st.ep0_fd, dir_in);
                return;
        }
    }

    if (!dir_in && length > 0) {
        std::vector<uint8_t> discard;
        read_ep0_payload(st.ep0_fd, discard, length);
        return;
    }
    ep0_ack_status(st.ep0_fd, dir_in);
}

static void pump_functionfs_ep0_events(int id) {
    if (id < 0 || id >= HID_PORT_COUNT) return;
    int fd = g_ffs_ports[id].ep0_fd;
    if (fd < 0) return;

    for (int i = 0; i < 16; ++i) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;

        usb_functionfs_event events[8];
        ssize_t r = read(fd, events, sizeof(events));
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (r == 0) return;
        const size_t count = static_cast<size_t>(r) / sizeof(usb_functionfs_event);
        for (size_t e = 0; e < count; ++e) {
            auto refresh_s2_host_state = [] {
                if (!gadget_uses_switch2_identity()) return;
                bool any_awake = false;
                for (int p = 0; p < switch2_virtual_port_count(); ++p) {
                    any_awake = any_awake || (g_ffs_ports[p].host_enabled
                                               && !g_ffs_ports[p].host_suspended);
                }
                if (any_awake) mark_switch2_usb_host_resumed();
                else mark_switch2_usb_host_disconnected();
            };
            switch (events[e].type) {
                case FUNCTIONFS_SETUP:
                    handle_functionfs_setup(id, events[e].u.setup);
                    break;
                case FUNCTIONFS_DISABLE:
                case FUNCTIONFS_UNBIND:
                    if (g_ctx.verbose && g_ffs_ports[id].host_enabled)
                        std::println("[ffs] port {} host {}", id + 1,
                                     events[e].type == FUNCTIONFS_DISABLE ? "disabled interface" : "unbound gadget");
                    g_ffs_ports[id].host_enabled = false;
                    g_ffs_ports[id].host_suspended = false;
                    refresh_s2_host_state();
                    break;
                case FUNCTIONFS_ENABLE:
                    if (g_ctx.verbose && !g_ffs_ports[id].host_enabled)
                        std::println("[ffs] port {} host enabled interface (configuration set)", id + 1);
                    g_ffs_ports[id].host_enabled = true;
                    g_ffs_ports[id].host_suspended = false;
                    refresh_s2_host_state();
                    break;
                case FUNCTIONFS_BIND:
                    break;
                case FUNCTIONFS_SUSPEND:
                    // USB suspend/resume is device-wide even though each
                    // FunctionFS instance receives its own notification.
                    if (gadget_uses_switch2_identity()) {
                        for (int p = 0; p < switch2_virtual_port_count(); ++p)
                            g_ffs_ports[p].host_suspended = true;
                    } else {
                        g_ffs_ports[id].host_suspended = true;
                    }
                    refresh_s2_host_state();
                    break;
                case FUNCTIONFS_RESUME:
                    if (gadget_uses_switch2_identity()) {
                        for (int p = 0; p < switch2_virtual_port_count(); ++p)
                            g_ffs_ports[p].host_suspended = false;
                    } else {
                        g_ffs_ports[id].host_suspended = false;
                    }
                    refresh_s2_host_state();
                    break;
                default:
                    break;
            }
        }
    }
}

static void ffs_io_wake_handler(int) {}

static void ensure_ffs_io_signal_installed() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa{};
        sa.sa_handler = ffs_io_wake_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // deliberately no SA_RESTART: interrupted syscalls return EINTR
        sigaction(SIGUSR1, &sa, nullptr);
    });
}

static void ffs_reader_loop(int id) {
    FfsPortState& st = g_ffs_ports[id];
    std::vector<uint8_t> buf(HIDG_MAX_REPORT_SIZE);
    while (st.io_running.load(std::memory_order_relaxed)) {
        int fd = st.ep_out_fd;
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        ssize_t r = read(fd, buf.data(), buf.size());
        if (r > 0) {
            std::lock_guard<std::mutex> lk(st.out_mtx);
            if (st.out_reports.size() >= 64) st.out_reports.pop_front();
            st.out_reports.emplace_back(buf.begin(), buf.begin() + r);
            continue;
        }
        // r <= 0: interface not enabled yet, host disabled it, or the request
        // was cancelled (EINTR from the shutdown signal). Back off briefly so we
        // do not spin while the endpoint is down.
        if (r < 0 && errno == EINTR) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    st.reader_exited.store(true, std::memory_order_release);
}

static void ffs_vendor_reader_loop(int id) {
    FfsPortState& st = g_ffs_ports[id];
    // Bulk reads must cover a whole max packet; the HS descriptors declare
    // 512-byte bulk endpoints (commands themselves stay <= 64 bytes).
    std::vector<uint8_t> buf(512);
    while (st.io_running.load(std::memory_order_relaxed)) {
        int fd = st.ep_vendor_out_fd;
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        ssize_t r = read(fd, buf.data(), buf.size());
        if (r > 0) {
            bool dropped_oldest = false;
            size_t depth_after = 0;
            std::lock_guard<std::mutex> lk(st.vendor_out_mtx);
            if (st.vendor_out_reports.size() >= 64) {
                st.vendor_out_reports.pop_front();
                dropped_oldest = true;
            }
            st.vendor_out_reports.emplace_back(buf.begin(), buf.begin() + r);
            depth_after = st.vendor_out_reports.size();
            if (g_ctx.verbose && buf[0] == 0x01) {
                std::println("[s2][nfc][usb-read] t_us={} port={} bytes={} depth_after={} dropped_oldest={} raw={}",
                             now_us(), id, r, depth_after, dropped_oldest,
                             bytes_to_hex(std::span<const uint8_t>(buf.data(), static_cast<size_t>(r))));
            }
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    st.vendor_reader_exited.store(true, std::memory_order_release);
}

static void ffs_vendor_writer_loop(int id) {
    FfsPortState& st = g_ffs_ports[id];
    while (st.io_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> report;
        {
            std::unique_lock<std::mutex> lk(st.vendor_in_mtx);
            st.vendor_in_cv.wait_for(lk, std::chrono::milliseconds(20), [&] {
                return !st.vendor_in_reports.empty() || !st.io_running.load(std::memory_order_relaxed);
            });
            if (!st.io_running.load(std::memory_order_relaxed)) break;
            if (st.vendor_in_reports.empty()) continue;
            report = std::move(st.vendor_in_reports.front());
            st.vendor_in_reports.pop_front();
        }
        int fd = st.ep_vendor_in_fd;
        if (fd < 0 || report.empty()) {
            if (g_ctx.verbose && !report.empty() && report[0] == 0x01) {
                std::println(stderr,
                             "[s2][nfc][usb-write] port={} skipped fd={} report_len={} raw={}",
                             id, fd, report.size(), bytes_to_hex(report));
            }
            continue;
        }
        size_t written_total = 0;
        int write_errno = 0;
        while (written_total < report.size() && st.io_running.load(std::memory_order_relaxed)) {
            const ssize_t w = write(fd, report.data() + written_total, report.size() - written_total);
            if (w > 0) {
                written_total += static_cast<size_t>(w);
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            write_errno = w < 0 ? errno : EIO;
            break;
        }
        if (g_ctx.verbose && report[0] == 0x01) {
            if (written_total != report.size()) {
                std::println(stderr,
                             "[s2][nfc][usb-write] t_us={} port={} requested={} written={} complete=false errno={} ({}) raw={}",
                             now_us(), id, report.size(), written_total, write_errno,
                             write_errno != 0 ? std::strerror(write_errno) : "short write", bytes_to_hex(report));
            } else {
                std::println("[s2][nfc][usb-write] t_us={} port={} requested={} written={} complete=true raw={}",
                             now_us(), id, report.size(), written_total, bytes_to_hex(report));
            }
        }
    }
    st.vendor_writer_exited.store(true, std::memory_order_release);
}

static void apply_uac1_gain(uint8_t* pcm, size_t len, uint8_t muted, uint32_t gain_q16) {
    if (!pcm || len == 0) return;
    if (muted != 0 || gain_q16 == 0) {
        std::fill_n(pcm, len, uint8_t{0});
        return;
    }
    if (gain_q16 >= S2_UAC_GAIN_Q16_UNITY) return;

    // S16LE stereo, gain in Q16.16. This stays allocation-free and avoids any
    // floating-point/transcendental work in the 1 kHz USB audio loops.
    for (size_t i = 0; i + 1 < len; i += 2) {
        const uint16_t raw = static_cast<uint16_t>(pcm[i]) |
                             (static_cast<uint16_t>(pcm[i + 1]) << 8);
        const int32_t sample = static_cast<int16_t>(raw);
        int64_t product = static_cast<int64_t>(sample) * gain_q16;
        product = product >= 0
            ? (product + (1ll << 15)) / (1ll << 16)
            : (product - (1ll << 15)) / (1ll << 16);
        const int32_t scaled = std::clamp<int32_t>(static_cast<int32_t>(product),
                                                   -32768, 32767);
        const uint16_t encoded = static_cast<uint16_t>(static_cast<int16_t>(scaled));
        pcm[i] = static_cast<uint8_t>(encoded & 0xFFu);
        pcm[i + 1] = static_cast<uint8_t>(encoded >> 8);
    }
}

static void ffs_audio_reader_loop(int id) {
    FfsPortState& st = g_ffs_ports[id];
    std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
    while (st.io_running.load(std::memory_order_relaxed)) {
        if (st.audio_playback_alt.load(std::memory_order_relaxed) != 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const int fd = st.ep_audio_out_fd;
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const ssize_t r = read(fd, frame.data(), frame.size());
        if (r > 0) {
            mark_switch2_usb_activity();
            apply_uac1_gain(frame.data(), static_cast<size_t>(r),
                            st.audio_playback_mute.load(std::memory_order_acquire),
                            st.audio_playback_gain_q16.load(std::memory_order_acquire));
            // Isochronous playback is one 192-byte frame per millisecond. Keep
            // a fixed-size frame to avoid a heap allocation in the 1 kHz path;
            // if the UDC ever returns a short packet, pad the remainder with
            // silence so network packet timing stays exact.
            if (static_cast<size_t>(r) < frame.size()) {
                std::fill(frame.begin() + r, frame.end(), uint8_t{0});
            }
            {
                std::lock_guard<std::mutex> lk(st.audio_out_mtx);
                // Audio is real-time. Keep at most 8 ms and discard oldest data
                // rather than replaying stale sound after a scheduling/network stall.
                while (st.console_audio_frames.size() >= 8) st.console_audio_frames.pop_front();
                st.console_audio_frames.push_back(frame);
            }
            st.audio_out_cv.notify_one();
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    st.audio_reader_exited.store(true, std::memory_order_release);
}

static void ffs_audio_writer_loop(int id) {
    FfsPortState& st = g_ffs_ports[id];
    const std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> silence{};
    while (st.io_running.load(std::memory_order_relaxed)) {
        if (st.audio_capture_alt.load(std::memory_order_relaxed) != 1) {
            std::unique_lock<std::mutex> lk(st.audio_in_mtx);
            st.audio_in_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return !st.io_running.load(std::memory_order_relaxed)
                    || st.audio_capture_alt.load(std::memory_order_relaxed) == 1;
            });
            continue;
        }
        std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
        bool have_frame = false;
        {
            std::unique_lock<std::mutex> lk(st.audio_in_mtx);
            st.audio_in_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return !st.microphone_frames.empty() || !st.io_running.load(std::memory_order_relaxed);
            });
            if (!st.io_running.load(std::memory_order_relaxed)) break;
            if (!st.microphone_frames.empty()) {
                frame = st.microphone_frames.front();
                st.microphone_frames.pop_front();
                have_frame = true;
            }
        }

        const int fd = st.ep_audio_in_fd;
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (!have_frame) frame = silence;
        apply_uac1_gain(frame.data(), frame.size(),
                        st.audio_capture_mute.load(std::memory_order_acquire),
                        st.audio_capture_gain_q16.load(std::memory_order_acquire));
        const ssize_t w = write(fd, frame.data(), frame.size());
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    st.audio_writer_exited.store(true, std::memory_order_release);
}

// Blocking write(ep1) loop: drains in_reports and submits each report on the
// interrupt-IN endpoint, waiting for the host to poll it.
static void ffs_writer_loop(int id) {
    FfsPortState& st = g_ffs_ports[id];

    const auto retime_s2_motion_report = [&](std::vector<uint8_t>& report) {
        if (!gadget_uses_switch2_identity() || report.empty()) return;

        size_t motion_len_index = 0;
        size_t motion_data_index = 0;
        switch (report[0]) {
            case 0x07:
            case 0x08:
                motion_len_index = 16;
                motion_data_index = 17;
                break;
            case 0x09:
                motion_len_index = 15;
                motion_data_index = 16;
                break;
            default:
                return;
        }
        if (report.size() <= motion_len_index || report[motion_len_index] < 4
                || report.size() < motion_data_index + 2) {
            st.s2_motion_last_write_us = 0;
            st.s2_motion_tick_fraction = 0;
            st.s2_motion_last_report_id = report[0];
            return;
        }

        const uint64_t write_us = now_us();
        uint16_t elapsed_ticks = 3;
        if (st.s2_motion_last_write_us != 0
                && st.s2_motion_last_report_id == report[0]
                && write_us > st.s2_motion_last_write_us) {
            const uint64_t delta_us = write_us - st.s2_motion_last_write_us;
            const uint64_t scaled = st.s2_motion_tick_fraction + delta_us * 800ULL;
            elapsed_ticks = static_cast<uint16_t>(scaled / 1'000'000ULL);
            st.s2_motion_tick_fraction = scaled % 1'000'000ULL;
            if (elapsed_ticks == 0) elapsed_ticks = 1;

            // The currently implemented USB Joy-Con codec is the normal
            // single-interval layout. Avoid advertising a >15-tick catch-up
            // frame while still packing the normal layout; that mismatch is
            // interpreted as a discontinuity by motion consumers. USB is
            // normally polled fast enough that this clamp is only a recovery
            // path after scheduling stalls.
            elapsed_ticks = std::min<uint16_t>(elapsed_ticks, 15);
        } else {
            st.s2_motion_tick_fraction = 0;
        }

        st.s2_motion_tick = static_cast<uint16_t>(
            (st.s2_motion_tick + elapsed_ticks) & 0x0FFFu);
        const uint16_t timing = static_cast<uint16_t>(
            ((elapsed_ticks & 0x0Fu) << 12) | st.s2_motion_tick);
        report[motion_data_index] = static_cast<uint8_t>(timing & 0xFFu);
        report[motion_data_index + 1] = static_cast<uint8_t>((timing >> 8) & 0xFFu);
        st.s2_motion_last_write_us = write_us;
        st.s2_motion_last_report_id = report[0];
    };

    while (st.io_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> report;
        {
            std::unique_lock<std::mutex> lk(st.in_mtx);
            st.in_cv.wait_for(lk, std::chrono::milliseconds(20), [&] {
                return !st.in_reports.empty() || !st.io_running.load(std::memory_order_relaxed);
            });
            if (!st.io_running.load(std::memory_order_relaxed)) break;
            if (st.in_reports.empty()) continue;
            report = std::move(st.in_reports.front());
            st.in_reports.pop_front();
        }
        int fd = st.ep_in_fd;
        if (fd < 0 || report.empty()) continue;
        retime_s2_motion_report(report);
        // For S1/S2 command replies we keep the blocking semantics.  HORI is
        // latency-sensitive and has no host output path we must preserve, so its
        // endpoint is opened O_NONBLOCK above: if the host is not ready, drop
        // the stale frame exactly like the old /dev/hidg* O_NONBLOCK writer did.
        ssize_t w = write(fd, report.data(), report.size());
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (w == static_cast<ssize_t>(report.size()) && !st.input_write_seen) {
            st.input_write_seen = true;
            if (g_ctx.verbose)
                std::println("[ffs] port {} first HID IN report accepted by host", id + 1);
        }
        (void)w;
    }
    st.writer_exited.store(true, std::memory_order_release);
}

static bool functionfs_start_port_io(int id) {
    if (id < 0 || id >= HID_PORT_COUNT) return false;
    FfsPortState& st = g_ffs_ports[id];
    if (st.io_running.load(std::memory_order_relaxed)) return true;
    ensure_ffs_io_signal_installed();

    const int in_flags = O_WRONLY | (gadget_uses_hori_identity() ? O_NONBLOCK : 0);
    const bool s2_native = gadget_uses_switch2_identity();
    st.ep_in_fd  = open(functionfs_ep_in_path(id).c_str(),  in_flags);
    st.ep_out_fd = open(functionfs_ep_out_path(id).c_str(), O_RDONLY);
    if (s2_native) {
        st.ep_vendor_out_fd = open(functionfs_ep_vendor_out_path(id).c_str(), O_RDONLY);
        st.ep_vendor_in_fd  = open(functionfs_ep_vendor_in_path(id).c_str(),  O_WRONLY);
        st.ep_audio_out_fd  = open(functionfs_ep_audio_out_path(id).c_str(), O_RDONLY);
        st.ep_audio_in_fd   = open(functionfs_ep_audio_in_path(id).c_str(),  O_WRONLY);
    }
    const bool vendor_required = s2_native;
    if (st.ep_in_fd < 0 || st.ep_out_fd < 0
            || (vendor_required && (st.ep_vendor_out_fd < 0 || st.ep_vendor_in_fd < 0
                                     || st.ep_audio_out_fd < 0 || st.ep_audio_in_fd < 0))) {
        if (st.ep_in_fd  >= 0) { close(st.ep_in_fd);  st.ep_in_fd  = -1; }
        if (st.ep_out_fd >= 0) { close(st.ep_out_fd); st.ep_out_fd = -1; }
        if (st.ep_vendor_out_fd >= 0) { close(st.ep_vendor_out_fd); st.ep_vendor_out_fd = -1; }
        if (st.ep_vendor_in_fd  >= 0) { close(st.ep_vendor_in_fd);  st.ep_vendor_in_fd  = -1; }
        if (st.ep_audio_out_fd  >= 0) { close(st.ep_audio_out_fd);  st.ep_audio_out_fd  = -1; }
        if (st.ep_audio_in_fd   >= 0) { close(st.ep_audio_in_fd);   st.ep_audio_in_fd   = -1; }
        return false;
    }
    { std::lock_guard<std::mutex> lk(st.out_mtx); st.out_reports.clear(); }
    { std::lock_guard<std::mutex> lk(st.in_mtx);  st.in_reports.clear();  }
    { std::lock_guard<std::mutex> lk(st.vendor_out_mtx); st.vendor_out_reports.clear(); }
    { std::lock_guard<std::mutex> lk(st.vendor_in_mtx);  st.vendor_in_reports.clear();  }
    { std::lock_guard<std::mutex> lk(st.audio_out_mtx); st.console_audio_frames.clear(); }
    { std::lock_guard<std::mutex> lk(st.audio_in_mtx);  st.microphone_frames.clear();  }
    st.reader_exited.store(false, std::memory_order_relaxed);
    st.writer_exited.store(false, std::memory_order_relaxed);
    st.input_write_seen = false;
    st.s2_motion_tick = 0;
    st.s2_motion_tick_fraction = 0;
    st.s2_motion_last_write_us = 0;
    st.s2_motion_last_report_id = 0;
    st.s2_coalesced_input_reports = 0;
    st.vendor_reader_exited.store(!s2_native, std::memory_order_relaxed);
    st.vendor_writer_exited.store(!s2_native, std::memory_order_relaxed);
    st.audio_reader_exited.store(!s2_native, std::memory_order_relaxed);
    st.audio_writer_exited.store(!s2_native, std::memory_order_relaxed);
    st.audio_playback_alt.store(0, std::memory_order_relaxed);
    st.audio_capture_alt.store(0, std::memory_order_relaxed);
    st.audio_playback_mute.store(0, std::memory_order_relaxed);
    st.audio_capture_mute.store(0, std::memory_order_relaxed);
    st.audio_playback_volume.store(0, std::memory_order_relaxed);
    st.audio_capture_volume.store(0, std::memory_order_relaxed);
    st.audio_playback_gain_q16.store(S2_UAC_GAIN_Q16_UNITY, std::memory_order_relaxed);
    st.audio_capture_gain_q16.store(S2_UAC_GAIN_Q16_UNITY, std::memory_order_relaxed);
    st.io_running.store(true, std::memory_order_relaxed);
    st.reader_thread = std::thread(ffs_reader_loop, id);
    st.writer_thread = std::thread(ffs_writer_loop, id);
    if (s2_native) {
        switch2_native_reset_port(id);
        st.vendor_reader_thread = std::thread(ffs_vendor_reader_loop, id);
        st.vendor_writer_thread = std::thread(ffs_vendor_writer_loop, id);
        st.audio_reader_thread = std::thread(ffs_audio_reader_loop, id);
        st.audio_writer_thread = std::thread(ffs_audio_writer_loop, id);
    }
    return true;
}

static void functionfs_stop_port_io(int id) {
    if (id < 0 || id >= HID_PORT_COUNT) return;
    FfsPortState& st = g_ffs_ports[id];
    st.io_running.store(false, std::memory_order_relaxed);
    st.in_cv.notify_all();
    st.vendor_in_cv.notify_all();
    st.audio_in_cv.notify_all();
    st.audio_out_cv.notify_all();

    if (st.reader_thread.joinable()) {
        pthread_t h = st.reader_thread.native_handle();
        while (!st.reader_exited.load(std::memory_order_acquire)) {
            pthread_kill(h, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        st.reader_thread.join();
    }
    if (st.writer_thread.joinable()) {
        pthread_t h = st.writer_thread.native_handle();
        while (!st.writer_exited.load(std::memory_order_acquire)) {
            st.in_cv.notify_all();
            pthread_kill(h, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        st.writer_thread.join();
    }
    if (st.vendor_reader_thread.joinable()) {
        pthread_t h = st.vendor_reader_thread.native_handle();
        while (!st.vendor_reader_exited.load(std::memory_order_acquire)) {
            pthread_kill(h, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        st.vendor_reader_thread.join();
    }
    if (st.vendor_writer_thread.joinable()) {
        pthread_t h = st.vendor_writer_thread.native_handle();
        while (!st.vendor_writer_exited.load(std::memory_order_acquire)) {
            st.vendor_in_cv.notify_all();
            pthread_kill(h, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        st.vendor_writer_thread.join();
    }

    if (st.audio_reader_thread.joinable()) {
        pthread_t h = st.audio_reader_thread.native_handle();
        while (!st.audio_reader_exited.load(std::memory_order_acquire)) {
            pthread_kill(h, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        st.audio_reader_thread.join();
    }
    if (st.audio_writer_thread.joinable()) {
        pthread_t h = st.audio_writer_thread.native_handle();
        while (!st.audio_writer_exited.load(std::memory_order_acquire)) {
            st.audio_in_cv.notify_all();
            pthread_kill(h, SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        st.audio_writer_thread.join();
    }

    if (st.ep_in_fd  >= 0) { close(st.ep_in_fd);  st.ep_in_fd  = -1; }
    if (st.ep_out_fd >= 0) { close(st.ep_out_fd); st.ep_out_fd = -1; }
    if (st.ep_vendor_out_fd >= 0) { close(st.ep_vendor_out_fd); st.ep_vendor_out_fd = -1; }
    if (st.ep_vendor_in_fd  >= 0) { close(st.ep_vendor_in_fd);  st.ep_vendor_in_fd  = -1; }
    if (st.ep_audio_out_fd  >= 0) { close(st.ep_audio_out_fd);  st.ep_audio_out_fd  = -1; }
    if (st.ep_audio_in_fd   >= 0) { close(st.ep_audio_in_fd);   st.ep_audio_in_fd   = -1; }
    { std::lock_guard<std::mutex> lk(st.out_mtx); st.out_reports.clear(); }
    { std::lock_guard<std::mutex> lk(st.in_mtx);  st.in_reports.clear();  }
    { std::lock_guard<std::mutex> lk(st.vendor_out_mtx); st.vendor_out_reports.clear(); }
    { std::lock_guard<std::mutex> lk(st.vendor_in_mtx);  st.vendor_in_reports.clear();  }
    { std::lock_guard<std::mutex> lk(st.audio_out_mtx); st.console_audio_frames.clear(); }
    { std::lock_guard<std::mutex> lk(st.audio_in_mtx);  st.microphone_frames.clear();  }
}

} // namespace

bool functionfs_transport_active() {
    return g_ctx.functionfs_transport_active.load(std::memory_order_relaxed);
}

int functionfs_active_port_count() {
    return functionfs_virtual_port_count_for_family();
}

std::string functionfs_ep_in_path(int id) {
    return (ffs_mount_dir(id) / "ep1").string();
}

std::string functionfs_ep_out_path(int id) {
    return (ffs_mount_dir(id) / "ep2").string();
}

std::string functionfs_ep_vendor_out_path(int id) {
    return (ffs_mount_dir(id) / "ep3").string();
}

std::string functionfs_ep_vendor_in_path(int id) {
    return (ffs_mount_dir(id) / "ep4").string();
}

std::string functionfs_ep_audio_out_path(int id) {
    return (ffs_mount_dir(id) / "ep5").string();
}

std::string functionfs_ep_audio_in_path(int id) {
    return (ffs_mount_dir(id) / "ep6").string();
}

bool functionfs_nodes_ready() {
    if (!functionfs_transport_active()) return false;
    const int ports = functionfs_virtual_port_count_for_family();
    for (int i = 0; i < ports; ++i) {
        if (g_ffs_ports[i].ep0_fd < 0 || !g_ffs_ports[i].descriptors_written) return false;
        if (access(functionfs_ep_in_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (access(functionfs_ep_out_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (access(functionfs_ep_vendor_out_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (access(functionfs_ep_vendor_in_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (access(functionfs_ep_audio_out_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (access(functionfs_ep_audio_in_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (!functionfs_io_ready(i)) return false;
    }
    return true;
}

bool functionfs_poll_control_report(int id, std::vector<unsigned char>& out_report) {
    out_report.clear();
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT) return false;
    pump_functionfs_ep0_events(id);
    auto& q = g_ffs_ports[id].control_reports;
    if (q.empty()) return false;
    out_report = std::move(q.front());
    q.pop_front();
    return true;
}



bool functionfs_io_ready(int id) {
    if (id < 0 || id >= HID_PORT_COUNT) return false;
    const FfsPortState& st = g_ffs_ports[id];
    bool base = st.io_running.load(std::memory_order_relaxed)
        && st.ep_in_fd >= 0 && st.ep_out_fd >= 0;
    if (gadget_uses_switch2_identity())
        base = base && st.ep_vendor_out_fd >= 0 && st.ep_vendor_in_fd >= 0
                    && st.ep_audio_out_fd >= 0 && st.ep_audio_in_fd >= 0;
    return base;
}

bool functionfs_host_enabled(int id) {
    if (id < 0 || id >= HID_PORT_COUNT) return false;
    return g_ffs_ports[id].host_enabled;
}

bool functionfs_submit_input_report(int id, const uint8_t* data, size_t len) {
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT) return false;
    if (!data || len == 0) return false;
    FfsPortState& st = g_ffs_ports[id];
    if (!st.host_enabled || !functionfs_io_ready(id)) return false;
    {
        std::lock_guard<std::mutex> lk(st.in_mtx);
        const bool s2_realtime_input = gadget_uses_switch2_identity()
            && (data[0] == 0x07 || data[0] == 0x08 || data[0] == 0x09);
        if (s2_realtime_input) {
            // HID IN on the native S2 function is a stream of current input
            // state. Replaying a backlog of old gyro frames after a temporary
            // endpoint stall creates visible twitches/teleports. Keep only the
            // newest pending state; command replies use the separate vendor-IN
            // queue and are not coalesced here.
            auto it = st.in_reports.begin();
            while (it != st.in_reports.end()) {
                const bool queued_realtime = !it->empty()
                    && ((*it)[0] == 0x07 || (*it)[0] == 0x08 || (*it)[0] == 0x09);
                if (queued_realtime) {
                    it = st.in_reports.erase(it);
                    ++st.s2_coalesced_input_reports;
                } else {
                    ++it;
                }
            }
        } else if (st.in_reports.size() >= 8) {
            st.in_reports.pop_front();
        }
        st.in_reports.emplace_back(data, data + len);
    }
    st.in_cv.notify_one();
    return true;
}

bool functionfs_poll_output_report(int id, std::vector<unsigned char>& out_report) {
    out_report.clear();
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT) return false;
    FfsPortState& st = g_ffs_ports[id];
    std::lock_guard<std::mutex> lk(st.out_mtx);
    if (st.out_reports.empty()) return false;
    out_report.assign(st.out_reports.front().begin(), st.out_reports.front().end());
    st.out_reports.pop_front();
    return true;
}

void functionfs_drain_output(int id) {
    if (id < 0 || id >= HID_PORT_COUNT) return;
    FfsPortState& st = g_ffs_ports[id];
    std::lock_guard<std::mutex> lk(st.out_mtx);
    st.out_reports.clear();
}




bool functionfs_poll_vendor_report(int id, std::vector<unsigned char>& out_report) {
    out_report.clear();
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT) return false;
    FfsPortState& st = g_ffs_ports[id];
    std::lock_guard<std::mutex> lk(st.vendor_out_mtx);
    if (st.vendor_out_reports.empty()) return false;
    out_report.assign(st.vendor_out_reports.front().begin(), st.vendor_out_reports.front().end());
    st.vendor_out_reports.pop_front();
    return true;
}

bool functionfs_submit_vendor_report(int id, const uint8_t* data, size_t len) {
    const bool is_nfc = data != nullptr && len != 0 && data[0] == 0x01;
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT) {
        if (g_ctx.verbose && is_nfc)
            std::println(stderr,
                         "[s2][nfc][tx-queue] rejected: transport_active={} port={} valid_port={} len={}",
                         functionfs_transport_active(), id, id >= 0 && id < HID_PORT_COUNT, len);
        return false;
    }
    if (!data || len == 0) {
        if (g_ctx.verbose)
            std::println(stderr, "[s2][nfc][tx-queue] rejected null/empty vendor response port={} len={}", id, len);
        return false;
    }
    FfsPortState& st = g_ffs_ports[id];
    if (!st.host_enabled || !functionfs_io_ready(id) || st.ep_vendor_in_fd < 0) {
        if (g_ctx.verbose && is_nfc)
            std::println(stderr,
                         "[s2][nfc][tx-queue] rejected port={} host_enabled={} io_ready={} vendor_in_fd={} len={} raw={}",
                         id, st.host_enabled, functionfs_io_ready(id), st.ep_vendor_in_fd,
                         len, bytes_to_hex(std::span<const uint8_t>(data, len)));
        return false;
    }
    bool dropped_oldest = false;
    size_t depth_after = 0;
    {
        std::lock_guard<std::mutex> lk(st.vendor_in_mtx);
        if (st.vendor_in_reports.size() >= 16) {
            st.vendor_in_reports.pop_front();
            dropped_oldest = true;
        }
        st.vendor_in_reports.emplace_back(data, data + len);
        depth_after = st.vendor_in_reports.size();
    }
    if (g_ctx.verbose && is_nfc)
        std::println("[s2][nfc][tx-queue] accepted t_us={} port={} len={} depth_after={} dropped_oldest={} raw={}",
                     now_us(), id, len, depth_after, dropped_oldest,
                     bytes_to_hex(std::span<const uint8_t>(data, len)));
    st.vendor_in_cv.notify_one();
    return true;
}

bool functionfs_wait_console_audio(
        int id,
        std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES>& audio_frame,
        std::chrono::milliseconds timeout) {
    audio_frame.fill(0);
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT) return false;
    FfsPortState& st = g_ffs_ports[id];
    if (st.audio_playback_alt.load(std::memory_order_relaxed) != 1) return false;
    std::unique_lock<std::mutex> lk(st.audio_out_mtx);
    if (!st.audio_out_cv.wait_for(lk, timeout, [&] {
            return !st.console_audio_frames.empty()
                || !st.io_running.load(std::memory_order_relaxed)
                || st.audio_playback_alt.load(std::memory_order_relaxed) != 1;
        })) {
        return false;
    }
    if (st.console_audio_frames.empty()) return false;
    audio_frame = st.console_audio_frames.front();
    st.console_audio_frames.pop_front();
    return true;
}

bool functionfs_submit_microphone_audio(int id, const uint8_t* data, size_t len) {
    if (!functionfs_transport_active() || id < 0 || id >= HID_PORT_COUNT || !data || len == 0)
        return false;
    if (len % ns::S2_AUDIO_USB_FRAME_BYTES != 0) return false;
    FfsPortState& st = g_ffs_ports[id];
    if (!st.host_enabled || !functionfs_io_ready(id) || st.ep_audio_in_fd < 0
            || st.audio_capture_alt.load(std::memory_order_relaxed) != 1) return false;
    {
        std::lock_guard<std::mutex> lk(st.audio_in_mtx);
        for (size_t off = 0; off < len; off += ns::S2_AUDIO_USB_FRAME_BYTES) {
            while (st.microphone_frames.size() >= 8) st.microphone_frames.pop_front();
            std::array<uint8_t, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
            std::memcpy(frame.data(), data + off, frame.size());
            st.microphone_frames.push_back(frame);
        }
    }
    st.audio_in_cv.notify_one();
    return true;
}

static bool create_hid_function(int id) {
    fs::path func = fs::path(GADGET_DIR) / "functions" / ("hid.usb" + std::to_string(id));
    if (!mkdirs(func)
            || !write_file(func / "protocol", "0")
            || !write_file(func / "subclass", "0")) {
        return false;
    }

    const bool legacy_hori = gadget_uses_hori_identity();
    if (!write_file(func / "report_length", legacy_hori ? "8" : "64")) return false;

    fs::path desc_path = func / "report_desc";
    if (legacy_hori) {
        if (!write_file(desc_path, LEGACY_REPORT_DESC, sizeof(LEGACY_REPORT_DESC))) return false;
    } else {
        if (!write_file(desc_path, VIRTUAL_CONTROLLER_REPORT_DESC, VIRTUAL_CONTROLLER_REPORT_DESC_SIZE)) return false;
    }

    fs::path link_path = fs::path(CONFIG_DIR) / ("hid.usb" + std::to_string(id));
    std::error_code ec;
    fs::remove(link_path, ec);
    return symlink(func.c_str(), link_path.c_str()) == 0;
}

static bool hidg_nodes_ready_for_family() {
    const int count = legacy_hidg_node_count_for_family();
    for (int i = 0; i < count; ++i) {
        if (access(("/dev/hidg" + std::to_string(i)).c_str(), R_OK | W_OK) != 0)
            return false;
    }
    return true;
}

// ===========================================================================
// Wake config I/O
// ===========================================================================

static bool read_switch2_wakeup_config_file(const std::string& path,
                                             std::string& mac,
                                             std::string& adv,
                                             std::string& hci,
                                             std::string* original = nullptr) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line, orig;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if      (key == "mac")          mac  = to_lower(val);
        else if (key == "adv")          adv  = to_upper_no_space(val);
        else if (key == "hci")          hci  = val;
        else if (key == "original_mac") orig = to_lower(val);
    }
    if (hci.empty()) hci = "hci0";
    if (original) *original = valid_mac(orig) ? orig : "";
    return valid_mac(mac) && valid_adv_hex(adv) && valid_hci(hci);
}

bool load_switch2_wakeup_config(bool quiet_if_missing) {
    if (g_ctx.bluetooth_disabled) return false;
    (void)quiet_if_missing;

    std::string mac, adv, hci, orig;
    if (!read_switch2_wakeup_config_file(g_ctx.switch2_wakeup_config_path, mac, adv, hci, &orig)) {
        g_ctx.switch2_wake_config_loaded = false;
        return false;
    }
    g_ctx.switch2_wake_mac      = mac;
    g_ctx.switch2_wake_adv_hex  = adv;
    g_ctx.switch2_wake_hci_dev  = valid_hci(hci) ? hci : "hci0";
    g_switch2_wake_original_bt_mac = orig;
    g_ctx.switch2_wake_config_loaded = true;
    return true;
}

static bool save_switch2_wakeup_config(const std::string& mac,
                                        const std::string& adv,
                                        const std::string& hci_dev,
                                        const std::string& original) {
    std::error_code ec;
    fs::create_directories(fs::path(g_ctx.switch2_wakeup_config_path).parent_path(), ec);
    std::ofstream f(g_ctx.switch2_wakeup_config_path, std::ios::trunc);
    if (!f) return false;
    f << "# NS-PC-Control Switch 2 wake config\n"
      << "mac=" << to_lower(mac)                              << "\n"
      << "adv=" << to_upper_no_space(adv)                    << "\n"
      << "hci=" << (valid_hci(hci_dev) ? hci_dev : "hci0")  << "\n";
    if (valid_mac(original)) f << "original_mac=" << to_lower(original) << "\n";
    f.close();
    chmod(g_ctx.switch2_wakeup_config_path.c_str(), 0600);
    return true;
}

// ===========================================================================
// HCI raw-command argument builders
// ===========================================================================

static std::vector<std::string> adv_hex_to_cmd_args(const std::string& adv_hex) {
    std::string adv  = to_upper_no_space(adv_hex);
    size_t      bytes = adv.size() / 2;
    std::vector<std::string> args = { std::format("{:02X}", bytes) };
    for (size_t i = 0; i < 31; ++i)
        args.push_back(i < bytes ? adv.substr(i * 2, 2) : "00");
    return args;
}

static std::vector<std::string> mac_to_hci_little_endian_args(const std::string& mac) {
    if (!valid_mac(mac)) return {};
    std::string compact;
    compact.reserve(12);
    for (char c : mac) {
        if (c != ':') compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    std::vector<std::string> args;
    args.reserve(6);
    for (int i = 5; i >= 0; --i)
        args.push_back(compact.substr(static_cast<size_t>(i) * 2, 2));
    return args;
}

// ===========================================================================
// Shell command execution
// ===========================================================================

struct WakeCmdResult { int exit_code = -1; std::string output; };

static WakeCmdResult run_wake_command(const std::vector<std::string>& args,
                                       bool verbose, bool capture = false,
                                       int timeout_sec = 3) {
    WakeCmdResult res;
    if (args.empty()) return res;
    if (timeout_sec <= 0) timeout_sec = 3;

    if (g_ctx.bluetooth_disabled) {
        // Check if this command touches Bluetooth; skip it if so.
        bool is_bt = std::ranges::any_of(args, [](const std::string& a) {
            std::string al = to_lower(a);
            return al == "rfkill" || al == "btmgmt" || al == "hcitool"
                || al == "hciconfig" || al.find("bluetooth") != std::string::npos;
        });
        if (is_bt) {
            if (verbose) std::println("[exec] (Bluetooth stack access disabled) {}", args[0]);
            return res;
        }
    }

    std::string cmd = "timeout --kill-after=1s " + std::to_string(timeout_sec) + "s ";
    for (const auto& a : args) cmd += a + " ";
    cmd += "2>&1";
    if (verbose) std::println("[exec] {}", cmd);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return res;
    if (capture) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) res.output += buf;
    }
    int status  = pclose(pipe);
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

static bool wake_cmd_ok(const std::vector<std::string>& args, bool verbose) {
    return run_wake_command(args, verbose).exit_code == 0;
}

// ===========================================================================
// HCI adapter queries
// ===========================================================================

static std::string read_hci_address(const std::string& hci_dev) {
    std::ifstream f("/sys/class/bluetooth/" + hci_dev + "/address");
    if (!f) return {};
    std::string mac;
    std::getline(f, mac);
    return valid_mac(mac) ? to_lower(mac) : "";
}

static bool hci_exists(const std::string& hci_dev) {
    return valid_hci(hci_dev)
        && access(("/sys/class/bluetooth/" + hci_dev).c_str(), F_OK) == 0;
}

static std::string first_hci_from_sysfs_now() {
    std::error_code ec;
    if (fs::exists("/sys/class/bluetooth", ec)) {
        for (const auto& entry : fs::directory_iterator("/sys/class/bluetooth", ec)) {
            std::string name = entry.path().filename().string();
            if (valid_hci(name)) return name;
        }
    }
    return "";
}

static std::string first_hci_from_btmgmt_now() {
    WakeCmdResult r = run_wake_command({"btmgmt", "info"}, false, true, 2);
    if (r.exit_code != 0) return "";
    std::istringstream iss(r.output);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (!line.starts_with("hci")) continue;
        size_t colon = line.find(':');
        std::string cand = colon == std::string::npos ? line : line.substr(0, colon);
        if (valid_hci(cand)) return cand;
    }
    return "";
}

static bool wait_for_hci_ready(std::string& hci_dev, bool verbose, int tries = 30) {
    if (!valid_hci(hci_dev)) hci_dev = "hci0";
    for (int i = 0; i < tries; ++i) {
        if (hci_exists(hci_dev)) return true;
        std::string detected = first_hci_from_sysfs_now();
        if (!valid_hci(detected)) detected = first_hci_from_btmgmt_now();
        if (valid_hci(detected)) {
            if (detected != hci_dev && verbose)
                std::println("[wake] Bluetooth adapter reappeared as {}", detected);
            hci_dev = detected;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (verbose) std::println(stderr, "[wake] Timed out waiting for Bluetooth adapter");
    return false;
}

static std::string read_hci_address_via_btmgmt(const std::string& hci_dev) {
    WakeCmdResult r = run_wake_command({"btmgmt", "-i", hci_dev, "info"}, false, true, 2);
    if (r.exit_code != 0) return "";
    std::istringstream iss(r.output);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (!line.starts_with("addr ")) continue;
        std::istringstream ls(line);
        std::string key, mac;
        ls >> key >> mac;
        if (valid_mac(mac)) return to_lower(mac);
    }
    return "";
}

static std::string read_hci_effective_address(const std::string& hci_dev) {
    std::string mac = read_hci_address(hci_dev);
    return valid_mac(mac) ? mac : read_hci_address_via_btmgmt(hci_dev);
}

// ===========================================================================
// LE advertising control (no-drop helpers; preserve connected BT links)
// ===========================================================================

static void wake_disable_advertising_quiet(const std::string& hci_dev) {
    run_wake_command({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000A", "00"}, false);
}

static void wake_disable_le_scan_quiet(const std::string& hci_dev) {
    // LE Set Scan Enable: disabled, duplicates ignored.
    // This only clears controller LE state; it does not affect BR/EDR gamepad links.
    run_wake_command({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000C", "00", "00"}, false);
}

static void wake_clear_scan_response_quiet(const std::string& hci_dev, bool verbose) {
    // LE Set Scan Response Data: zero-length payload padded to 31 bytes.
    wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0009",
                 "00", "00", "00", "00", "00", "00", "00", "00",
                 "00", "00", "00", "00", "00", "00", "00", "00",
                 "00", "00", "00", "00", "00", "00", "00", "00",
                 "00", "00", "00", "00", "00", "00", "00", "00"}, verbose);
}

// ===========================================================================
// Bluetooth controller state save / restore
// ===========================================================================

void restore_bluetooth_controller_state(const std::string& hci, bool restart_bluez);

void finish_prepared_wake_controller(const std::string& hci_dev, bool restart_bluez) {
    if (!g_bt_modified_for_wake.load(std::memory_order_relaxed)) return;
    restore_bluetooth_controller_state(hci_dev, restart_bluez);
    g_saved_bt_mac.clear();
    g_saved_bt_hci.clear();
    g_bt_modified_for_wake.store(false, std::memory_order_relaxed);
}

static std::string paired_adapter_mac_from_bluez_store(const std::string& wake_mac) {
    std::error_code ec;
    if (!fs::exists("/var/lib/bluetooth", ec)) return "";
    std::string best;
    std::string wake_lc = to_lower(wake_mac);
    for (const auto& entry : fs::directory_iterator("/var/lib/bluetooth", ec)) {
        std::string name = to_lower(entry.path().filename().string());
        if (valid_mac(name) && name != wake_lc && (best.empty() || name < best))
            best = name;
    }
    return best;
}

static std::string original_bt_mac_for_runtime(const std::string& hci) {
    if (valid_mac(g_switch2_wake_original_bt_mac)) return g_switch2_wake_original_bt_mac;
    std::string mac, adv, cfg_hci, original;
    if (read_switch2_wakeup_config_file(g_ctx.switch2_wakeup_config_path,
                                        mac, adv, cfg_hci, &original)
            && valid_mac(original)) {
        g_switch2_wake_original_bt_mac = original;
        return original;
    }
    std::string paired = paired_adapter_mac_from_bluez_store(g_ctx.switch2_wake_mac);
    return valid_mac(paired) ? paired : read_hci_effective_address(hci);
}

void restore_bluetooth_controller_state(const std::string& hci, bool restart_bluez) {
    if (g_ctx.bluetooth_disabled) return;
    std::string current  = read_hci_effective_address(hci);
    std::string original = original_bt_mac_for_runtime(hci);
    wake_disable_advertising_quiet(hci);
    run_wake_command({"rfkill",   "unblock", "bluetooth"},         false);
    run_wake_command({"systemctl", "stop",   "bluetooth"},         false);
    run_wake_command({"btmgmt", "-i", hci, "power",   "off"},     false);
    run_wake_command({"btmgmt", "-i", hci, "privacy", "off"},     false);
    run_wake_command({"btmgmt", "-i", hci, "bredr",   "on"},      false);
    run_wake_command({"btmgmt", "-i", hci, "le",      "on"},      false);
    if (valid_mac(original) && current != original)
        run_wake_command({"btmgmt", "-i", hci, "public-addr", original}, false);
    run_wake_command({"btmgmt", "-i", hci, "power",   "on"},      false);
    if (restart_bluez) {
        run_wake_command({"systemctl", "restart", "bluetooth"},    false);
        run_wake_command({"btmgmt", "-i", hci, "power", "on"},    false);
    }
}

// ===========================================================================
// Stack reset / adapter preparation
// ===========================================================================

static bool reset_wake_bt_stack(std::string& hci_dev, bool verbose) {
    if (hci_dev.empty()) hci_dev = "hci0";
    if (verbose) std::println(stderr, "[wake] Resetting Bluetooth stack / hciuart");
    run_wake_command({"systemctl", "stop",  "bluetooth"},     verbose);
    run_wake_command({"rfkill",    "unblock", "bluetooth"},   verbose);
    wake_disable_advertising_quiet(hci_dev);
    run_wake_command({"hciconfig", hci_dev, "down"},          verbose);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    run_wake_command({"systemctl", "restart", "hciuart"},     verbose);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (!wait_for_hci_ready(hci_dev, verbose)) return false;
    run_wake_command({"hciconfig", hci_dev, "up"},            verbose);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    wake_disable_advertising_quiet(hci_dev);
    return true;
}

[[maybe_unused]] static bool prepare_wake_controller(std::string& hci_dev,
                                     const std::string& mac_lc, bool verbose) {
    if (hci_dev.empty()) hci_dev = "hci0";
    run_wake_command({"systemctl", "stop",    "bluetooth"}, false);
    run_wake_command({"rfkill",    "unblock", "bluetooth"}, false);

    if (!wait_for_hci_ready(hci_dev, verbose)) {
        reset_wake_bt_stack(hci_dev, verbose);
        if (!wait_for_hci_ready(hci_dev, verbose)) return false;
    }

    if (!g_bt_modified_for_wake.load()) {
        g_saved_bt_mac      = read_hci_effective_address(hci_dev);
        g_saved_bt_hci      = hci_dev;
        g_bt_modified_for_wake = true;
    }

    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (verbose) std::println("[wake] Preparing Bluetooth controller, attempt {}, device {}",
                                  attempt, hci_dev);
        wake_cmd_ok({"btmgmt", "-i", hci_dev, "power",   "off"}, verbose);
        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "privacy", "off"}, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "bredr",   "off"}, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "le",      "on"},  verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "public-addr", mac_lc}, verbose)) {
            reset_wake_bt_stack(hci_dev, verbose);
            continue;
        }
        // Changing public-addr can briefly make hci0 disappear. Wait before powering on.
        if (!wait_for_hci_ready(hci_dev, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "power", "on"}, verbose) ||
            !wake_cmd_ok({"hciconfig", hci_dev, "up"}, verbose)) {
            reset_wake_bt_stack(hci_dev, verbose);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::string current = read_hci_effective_address(hci_dev);
        if (current != mac_lc) {
            if (verbose)
                std::println(stderr, "[wake] Bluetooth controller address is {}, expected {}",
                             current.empty() ? "unknown" : current, mac_lc);
            reset_wake_bt_stack(hci_dev, verbose);
            continue;
        }
        if (verbose) std::println("[wake] Bluetooth controller prepared as {}", mac_lc);
        return true;
    }
    return false;
}

// ===========================================================================
// LE raw advertising (wake burst)
// ===========================================================================

static bool start_wake_raw_advertising(std::string hci_dev,
                                        const std::string& mac_lc,
                                        const std::string& adv_uc,
                                        int seconds, bool verbose) {
    if (!valid_hci(hci_dev) || !valid_mac(mac_lc) || !valid_adv_hex(adv_uc)) return false;
    auto mac_args = mac_to_hci_little_endian_args(mac_lc);
    if (mac_args.size() != 6) return false;

    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
    if (!wait_for_hci_ready(hci_dev, verbose, 8)) return false;
    // Bring the adapter up only if currently down — no down/up cycle so
    // connected controllers keep their BR/EDR link.
    run_wake_command({"hciconfig", hci_dev, "up"}, verbose, false, 2);

    wake_disable_advertising_quiet(hci_dev);
    wake_disable_le_scan_quiet(hci_dev);

    std::vector<std::string> set_random = {"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0005"};
    set_random.insert(set_random.end(), mac_args.begin(), mac_args.end());

    bool random_ok = wake_cmd_ok(set_random, verbose);
    if (!random_ok) {
        // BlueZ may have left LE scan/advertising active. Clean up and retry once.
        wake_disable_advertising_quiet(hci_dev);
        wake_disable_le_scan_quiet(hci_dev);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        random_ok = wake_cmd_ok(set_random, verbose);
    }
    if (!random_ok) {
        if (verbose)
            std::println(stderr, "[wake] LE Set Random Address failed for {}; "
                                 "preserving existing Bluetooth links", mac_lc);
        return false;
    }

    std::vector<std::string> adv_data_cmd = {"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0008"};
    auto adv_args = adv_hex_to_cmd_args(adv_uc);
    adv_data_cmd.insert(adv_data_cmd.end(), adv_args.begin(), adv_args.end());

    // LE Set Advertising Parameters:
    //   interval min/max: 20–40 ms
    //   type: ADV_NONCONN_IND (0x03)
    //   own address type: random (0x01) — Joy-Con 2 MAC in LE random address register
    //   peer address: unused zeroes
    //   channel map: all three channels (37/38/39)
    //   filter policy: allow all
    if (!wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0006",
                      "20", "00", "40", "00", "03", "01",
                      "00", "00", "00", "00", "00", "00", "00",
                      "07", "00"}, verbose)
            || !wake_cmd_ok(adv_data_cmd, verbose)) {
        wake_disable_advertising_quiet(hci_dev);
        return false;
    }

    wake_clear_scan_response_quiet(hci_dev, verbose);
    if (verbose)
        std::println("[wake] Enable no-drop random-address advertising as {} for {}s on {}",
                     mac_lc, seconds, hci_dev);

    if (!wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000A", "01"}, verbose))
        return false;

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (g_ctx.running.load(std::memory_order_relaxed)
               && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    wake_disable_advertising_quiet(hci_dev);
    return true;
}

static bool send_switch2_wake_advert_once(const std::string& mac,
                                           const std::string& adv_hex,
                                           int seconds, bool verbose,
                                           bool /*force_prepare*/ = false) {
    // Runtime/test wake intentionally uses the no-drop path.
    if (!valid_mac(mac) || !valid_adv_hex(adv_hex)) return false;
    seconds = std::clamp(seconds, 1, 10);

    std::string mac_lc   = to_lower(mac);
    std::string adv_uc   = to_upper_no_space(adv_hex);
    std::string hci_dev  = valid_hci(g_ctx.switch2_wake_hci_dev)
                              ? g_ctx.switch2_wake_hci_dev : "hci0";

    // Serialise concurrent wake attempts so they don't race on the HCI adapter.
    std::lock_guard<std::mutex> wake_lock(g_wake_bt_mtx);

    if (verbose) {
        std::println("[wake] Wake MAC: {}\n[wake] ADV bytes: {}\n[wake] Duration: {}s",
                     mac_lc, adv_uc.size() / 2, seconds);
        std::println("[wake] No-drop path: keep Pi public MAC/BlueZ/controller links alive; "
                     "advertise with Joy-Con 2 MAC as LE random address");
    }

    bool ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose);
    if (!ok)
        std::println(stderr, "[wake] no-drop wake advert failed; not resetting Bluetooth, "
                             "so connected controllers keep their link");
    wake_disable_advertising_quiet(hci_dev);
    return ok;
}

// ===========================================================================
// Wake advert dispatch (called from the main server path)
// ===========================================================================

static void switch2_wake_adv_worker(int burst_ms) {
    int seconds = std::max(1, (burst_ms + 999) / 1000);
    send_switch2_wake_advert_once(g_ctx.switch2_wake_mac,
                                  g_ctx.switch2_wake_adv_hex,
                                  seconds, g_ctx.verbose);
    g_ctx.switch2_wake_adv_running.store(false, std::memory_order_relaxed);
}

[[maybe_unused]] static void switch2_delayed_wake_check_worker(const char* reason) {
    std::string w_reason = reason ? reason : "client connected";
    for (int i = 0; i < 32 && g_ctx.running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!any_recent_client_active(now_us())) {
            g_ctx.switch2_delayed_wake_check_running.store(false);
            return;
        }
    }
    g_ctx.switch2_delayed_wake_check_running.store(false);
    if (!g_ctx.switch2_usb_host_connected.load()) {
        std::println("[wake] USB host is STILL quiet; launching Switch 2 wake advert");
        maybe_send_switch2_wake_advert(w_reason.c_str());
    }
}

void maybe_send_switch2_wake_advert(const char* reason) {
    if (!g_ctx.switch2_wake_adv_enabled || !g_ctx.switch2_wake_config_loaded) return;

    uint64_t now = now_us();
    // Only wake when the Switch hasn't recently sent HID OUT traffic.
    if (switch2_usb_host_recently_active(now)) return;
    if (g_ctx.switch2_wake_adv_running.exchange(true)) return;

    uint64_t last = g_ctx.switch2_last_wake_adv_us.load(std::memory_order_relaxed);
    if (last && elapsed_us_saturated(now, last) < SWITCH2_WAKE_ADV_COOLDOWN_US) {
        g_ctx.switch2_wake_adv_running.store(false);
        return;
    }

    g_ctx.switch2_last_wake_adv_us.store(now, std::memory_order_relaxed);
    if (g_ctx.verbose && reason && *reason) std::println("[wake] waking up Switch 2 ({})", reason);
    else                                    std::println("[wake] waking up Switch 2");
    std::thread(switch2_wake_adv_worker, SWITCH2_WAKE_ADV_BURST_MS).detach();
}

// ===========================================================================
// btmon log parsing for capture setup
// ===========================================================================

static std::string normalize_nintendo_adv_payload(const std::string& data_hex) {
    std::string data = to_upper_no_space(data_hex);
    if (data.empty()) return "";

    // Complete AD payload as captured on-air.
    if (data.starts_with("020106") && data.find("FF5305") != std::string::npos) return data;
    // Manufacturer data with Nintendo company ID, without AD flags/header (52 chars = 26 bytes).
    if (data.size() == 52 && data.starts_with("5305")) return "0201061BFF" + data;
    // btmon-decoded manufacturer body without company ID (48 chars = 24 bytes).
    if (data.size() == 48) return "0201061BFF5305" + data;
    return "";
}

static bool parse_nintendo_adv_from_btmon_log(const std::string& path,
                                               const std::string& preferred_mac,
                                               std::string& out_mac,
                                               std::string& out_adv) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line, cur_mac;
    std::string preferred        = to_lower(preferred_mac);
    bool        saw_nintendo_company = false;

    auto accept_candidate = [&](const std::string& raw_hex) -> bool {
        if (cur_mac.empty() || (!preferred.empty() && cur_mac != preferred)) return false;
        std::string adv = normalize_nintendo_adv_payload(raw_hex);
        if (!valid_adv_hex(adv)) return false;
        out_mac = cur_mac;
        out_adv = adv;
        return true;
    };

    while (std::getline(f, line)) {
        std::string lower = to_lower(line);

        // Track the most recently seen BT address.
        size_t ap = lower.find("address:");
        if (ap != std::string::npos) {
            size_t p = ap + 8;
            while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
            if (p + 17 <= line.size()) {
                std::string cand = to_lower(line.substr(p, 17));
                if (valid_mac(cand)) {
                    cur_mac              = cand;
                    saw_nintendo_company = false;
                }
            }
        }

        if (lower.find("nintendo")  != std::string::npos
                || lower.find("0x0553") != std::string::npos
                || lower.find("0x5305") != std::string::npos) {
            saw_nintendo_company = true;
        }

        // Older btmon: "Data[31]: XX XX …"
        size_t dp = lower.find("data[");
        if (dp != std::string::npos) {
            size_t p = line.find(':', dp);
            if (p != std::string::npos && accept_candidate(to_upper_hex_bytes(line.substr(p + 1))))
                return true;
            continue;
        }

        // Newer btmon: "Data: XX XX …" (following a Nintendo company line)
        size_t plain_data = lower.find("data:");
        if (plain_data != std::string::npos && lower.find("data length") == std::string::npos) {
            size_t p = line.find(':', plain_data);
            if (p != std::string::npos) {
                std::string hex = to_upper_hex_bytes(line.substr(p + 1));
                if (saw_nintendo_company && accept_candidate(hex)) return true;
                if (accept_candidate(hex))                         return true;
            }
        }
    }
    return false;
}

// ===========================================================================
// Wake advert capture (setup mode)
// ===========================================================================

enum class WakeCaptureResult {
    Captured,
    Timeout,
    Cancelled
};

static bool system_was_interrupted(int status) {
    if (status == -1) return false;

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        return sig == SIGINT || sig == SIGTERM || sig == SIGQUIT;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        return code == 130 || code == 131 || code == 143;
    }

    return false;
}

static bool prepare_wake_capture_adapter(std::string& hci, bool verbose) {
    if (!valid_hci(hci)) hci = "hci0";
    run_wake_command({"systemctl", "stop",    "bluetooth"}, verbose);
    run_wake_command({"rfkill",    "unblock", "bluetooth"}, verbose);
    if (!wait_for_hci_ready(hci, verbose, 12)) reset_wake_bt_stack(hci, verbose);
    if (!wait_for_hci_ready(hci, verbose, 12)) return false;

    // Take ownership of the adapter once; repeated power-cycles can leave
    // controller pairing broken until Bluetooth is restarted.
    run_wake_command({"btmgmt", "-i", hci, "power",   "off"}, verbose);
    run_wake_command({"btmgmt", "-i", hci, "privacy", "off"}, verbose);
    run_wake_command({"btmgmt", "-i", hci, "bredr",   "off"}, verbose);
    run_wake_command({"btmgmt", "-i", hci, "le",      "on"},  verbose);
    run_wake_command({"btmgmt", "-i", hci, "power",   "on"},  verbose);
    wait_for_hci_ready(hci, verbose, 12);
    wake_disable_advertising_quiet(hci);
    wake_disable_le_scan_quiet(hci);
    return true;
}

static WakeCaptureResult capture_switch2_wake_advert(int seconds, const std::string& preferred_mac,
                                                   std::string& out_mac, std::string& out_adv) {
    seconds = std::clamp(seconds, 5, 180);
    char log_path[128];
    std::snprintf(log_path, sizeof(log_path), "/tmp/ns_switch2_wake_%ld.log", (long)getpid());
    std::string hci = valid_hci(g_ctx.switch2_wake_hci_dev)
                        ? g_ctx.switch2_wake_hci_dev : "hci0";

    wake_disable_advertising_quiet(hci);
    wake_disable_le_scan_quiet(hci);

    // Attempt capture for each second
    for (int attempt = 1; attempt <= seconds && g_ctx.running.load(std::memory_order_relaxed); ++attempt) {
        // Run btmon for 1 second while HCI scanning is active
        std::ostringstream cmd;
        cmd << "sh -c 'rm -f " << log_path
            << "; mon=\"\""
            << "; cleanup() { hcitool -i " << hci << " cmd 0x08 0x000C 00 00 >/dev/null 2>&1 || true; "
            << "[ -n \"$mon\" ] && kill \"$mon\" >/dev/null 2>&1 || true; "
            << "[ -n \"$mon\" ] && wait \"$mon\" >/dev/null 2>&1 || true; }"
            << "; trap \"cleanup; exit 130\" INT TERM QUIT"
            << "; timeout --kill-after=1s 2s btmon -T > " << log_path << " 2>&1 & mon=$!"
            << "; sleep 0.1"
            << "; hcitool -i " << hci << " cmd 0x08 0x000B 01 04 00 04 00 00 00 >/dev/null 2>&1 || true"
            << "; hcitool -i " << hci << " cmd 0x08 0x000C 01 00 >/dev/null 2>&1 || true"
            << "; sleep 1"
            << "; cleanup"
            << "; exit 0'";
        int status = std::system(cmd.str().c_str());
        if (system_was_interrupted(status)) {
            std::println("");
            std::println("[wake] Setup cancelled.");
            wake_disable_le_scan_quiet(hci);
            unlink(log_path);
            g_ctx.running.store(false, std::memory_order_relaxed);
            return WakeCaptureResult::Cancelled;
        }

        // Check if captured
        if (parse_nintendo_adv_from_btmon_log(log_path, preferred_mac, out_mac, out_adv)) {
            std::println("");  // newline after progress
            wake_disable_le_scan_quiet(hci);
            unlink(log_path);
            return WakeCaptureResult::Captured;
        }

        // Show progress on same line using carriage return
        std::fflush(stdout);
        std::fprintf(stdout, "\r[wake] Scanning for HOME... (%d/%d)", attempt, seconds);
        std::fflush(stdout);
    }

    std::println("");  // newline after progress
    wake_disable_le_scan_quiet(hci);
    unlink(log_path);
    if (!g_ctx.running.load(std::memory_order_relaxed)) {
        std::println("[wake] Setup cancelled.");
        return WakeCaptureResult::Cancelled;
    }
    return WakeCaptureResult::Timeout;
}

// ===========================================================================
// HCI detection
// ===========================================================================

std::string first_hci_from_sysfs() { return first_hci_from_sysfs_now(); }

static std::string detect_wake_hci_for_setup() {
    std::string hci = first_hci_from_sysfs();
    if (valid_hci(hci)) return hci;

    WakeCmdResult r = run_wake_command({"btmgmt", "info"}, false, true);
    if (r.exit_code == 0) {
        std::istringstream iss(r.output);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.starts_with("hci")) continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string cand = line.substr(0, colon);
            if (valid_hci(cand)) return cand;
        }
    }
    return "hci0";
}

// ===========================================================================
// Interactive Switch 2 wake setup  (--wake)
// ===========================================================================

int run_switch2_wakeup_setup() {
    if (geteuid() != 0) {
        std::println(stderr, "[wake] -wake needs root because it controls Bluetooth. Run with sudo.");
        return 2;
    }
    for (const char* cmd : {"btmon", "btmgmt", "hcitool", "hciconfig", "rfkill", "systemctl"}) {
        if (!command_exists(cmd)) {
            std::println(stderr, "[wake] Missing command: {}. Install BlueZ tools.", cmd);
            return 2;
        }
    }

    g_ctx.switch2_wake_hci_dev = detect_wake_hci_for_setup();
    std::string orig_mac = paired_adapter_mac_from_bluez_store("");
    if (!valid_mac(orig_mac)) orig_mac = read_hci_effective_address(g_ctx.switch2_wake_hci_dev);
    g_switch2_wake_original_bt_mac = orig_mac;

    {
        std::string setup_hci = g_ctx.switch2_wake_hci_dev;
        reset_wake_bt_stack(setup_hci, g_ctx.verbose);
        if (!prepare_wake_capture_adapter(setup_hci, g_ctx.verbose)) {
            std::println(stderr, "[wake] Could not prepare Bluetooth adapter for wake capture.");
            restore_bluetooth_controller_state(setup_hci, true);
            return 1;
        }
        g_ctx.switch2_wake_hci_dev = setup_hci;
    }

    bool setup_bt_state_restored = false;
    auto restore_setup_bt_state = [&] {
        if (setup_bt_state_restored) return;
        restore_bluetooth_controller_state(g_ctx.switch2_wake_hci_dev, true);
        setup_bt_state_restored = true;
    };

    std::println("NS-PC-Control Switch 2 Joy-Con 2 wake setup");
    std::println("[wake] Config will be saved to: {}", g_ctx.switch2_wakeup_config_path);
    std::println("[wake] Bluetooth adapter registered for runtime wake: {}", g_ctx.switch2_wake_hci_dev);
    std::println("[wake] Put the Switch 2 to sleep and keep the Joy-Con 2 close to the Pi.");
    std::println("[wake] Now listening for up to 3 minutes — press HOME on the Joy-Con 2.");

    std::string cap_mac, cap_adv;
    WakeCaptureResult capture_result = capture_switch2_wake_advert(180, "", cap_mac, cap_adv);

    if (capture_result == WakeCaptureResult::Cancelled) {
        restore_setup_bt_state();
        return 130;
    }

    if (capture_result == WakeCaptureResult::Timeout) {
        std::println(stderr, "[wake] Could not capture the HOME wake advert. "
                             "Try again with the Joy-Con 2 closer to the Pi.");
        restore_setup_bt_state();
        return 1;
    }

    std::println("[wake] Captured wake MAC: {}\n[wake] Captured wake ADV: {}", cap_mac, cap_adv);

    if (!save_switch2_wakeup_config(cap_mac, cap_adv, g_ctx.switch2_wake_hci_dev, orig_mac)) {
        restore_setup_bt_state();
        return 1;
    }

    g_ctx.switch2_wake_mac          = to_lower(cap_mac);
    g_ctx.switch2_wake_adv_hex      = to_upper_no_space(cap_adv);
    g_ctx.switch2_wake_config_loaded = true;
    g_ctx.switch2_wake_adv_enabled   = true;

    restore_setup_bt_state();
    std::println("[wake] Setup complete. Runtime wake is armed for BT, UDP, and WebSocket clients.");
    return 0;
}

// ===========================================================================
// Runtime Bluetooth / wake initialisation
// ===========================================================================

void enter_switch2_wake_runtime_mode() {
    if (!g_ctx.switch2_wake_adv_enabled || !g_ctx.switch2_wake_config_loaded) return;
    // Runtime wake is a no-drop path: make hci0 available but never stop bluetoothd,
    // power-cycle hci0, or disable BR/EDR.
    std::string hci = valid_hci(g_ctx.switch2_wake_hci_dev)
                        ? g_ctx.switch2_wake_hci_dev : "hci0";
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
    wait_for_hci_ready(hci, g_ctx.verbose, 4);
    run_wake_command({"hciconfig", hci, "up"}, false, false, 2);
    g_ctx.switch2_wake_hci_dev = hci;
}

static void wait_for_bluetooth_runtime_ready(bool verbose) {
    (void)verbose;
    if (g_ctx.bluetooth_disabled) return;
    std::string hci = valid_hci(g_ctx.switch2_wake_hci_dev)
                        ? g_ctx.switch2_wake_hci_dev : "hci0";
    for (int i = 0; i < 20 && g_ctx.running.load(); ++i) {
        WakeCmdResult info = run_wake_command({"btmgmt", "-i", hci, "info"}, false, true);
        if (info.exit_code == 0) {
            size_t pos = info.output.find("current settings:");
            if (pos != std::string::npos) {
                size_t end     = info.output.find('\n', pos);
                std::string current = info.output.substr(
                    pos, end == std::string::npos ? std::string::npos : end - pos);
                if (current.find("powered") != std::string::npos) return;
            }
        }
        run_wake_command({"rfkill",    "unblock", "bluetooth"},     false);
        run_wake_command({"systemctl", "start",   "bluetooth"},     false);
        run_wake_command({"btmgmt", "-i", hci, "power", "on"},     false);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void enter_bluetooth_runtime_mode() {
    if (g_ctx.bluetooth_disabled) return;
    if (!g_ctx.switch2_wake_config_loaded) load_switch2_wakeup_config(true);

    std::string hci      = valid_hci(g_ctx.switch2_wake_hci_dev)
                              ? g_ctx.switch2_wake_hci_dev : "hci0";
    std::string current  = read_hci_effective_address(hci);
    std::string original = original_bt_mac_for_runtime(hci);

    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);

    // Only do a heavy restore if the adapter is still spoofed as the Joy-Con
    // MAC from a previous setup/test run. Normal startup should not restart
    // bluetoothd just because wake is configured.
    if (g_ctx.switch2_wake_config_loaded
            && valid_mac(current) && valid_mac(original)
            && current == to_lower(g_ctx.switch2_wake_mac)
            && current != original) {
        restore_bluetooth_controller_state(hci, true);
    } else {
        run_wake_command({"systemctl", "start", "bluetooth"}, false);
        run_wake_command({"btmgmt", "-i", hci, "power", "on"}, false);
        // Pairing/reconnect is owned by the BlueZ D-Bus manager in bluetooth_manager.cpp.
    }

    wait_for_bluetooth_runtime_ready(g_ctx.verbose);
}

// ===========================================================================
// Wake BT state accessors
// ===========================================================================

bool wake_bt_state_was_modified() { return g_bt_modified_for_wake.load(); }

void restore_wake_bt_state(bool restart_bluez) {
    if (g_ctx.bluetooth_disabled) return;
    for (int i = 0; i < 10 && g_ctx.switch2_wake_adv_running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string hci = !g_saved_bt_hci.empty() ? g_saved_bt_hci : g_ctx.switch2_wake_hci_dev;
    if (!valid_hci(hci)) hci = "hci0";

    wake_disable_advertising_quiet(hci);
    if (!wake_bt_state_was_modified()) return;

    if (g_bt_modified_for_wake.load() && !g_saved_bt_mac.empty())
        g_switch2_wake_original_bt_mac = g_saved_bt_mac;

    restore_bluetooth_controller_state(hci, restart_bluez);
    g_saved_bt_mac.clear();
    g_saved_bt_hci.clear();
    g_bt_modified_for_wake = false;
}

// ===========================================================================
// HID gadget output queue drain
// ===========================================================================

void drain_hid_output_queue(int fd) {
    if (fd < 0) return;
    uint8_t discard[HIDG_MAX_REPORT_SIZE];
    for (int i = 0; i < 32; ++i) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
        if (read(fd, discard, sizeof(discard)) <= 0)           break;
    }
}

// ===========================================================================
// USB gadget boot configuration check
// ===========================================================================

static void print_gadget_host_config_error() {
    std::println(stderr, "[gadget] USB gadget host configurations are missing or conflicting.");
    std::println(stderr, "[gadget] Make sure the raspberry pi is configured as a USB gadget, check docs for details.");
}

static bool setup_gadget_builtin(bool force, const char* reason) {
    auto nodes_ready = [&]() {
        if (gadget_uses_switch2_identity()) {
            return functionfs_nodes_ready() && hidg_nodes_ready_for_family();
        }
        return hidg_nodes_ready_for_family();
    };

    if (!force && nodes_ready()) return true;
    if (!force && g_ctx.gadget_setup_attempted.exchange(true)) {
        if (nodes_ready()) return true;
        force = true;
    }
    if (force) g_ctx.gadget_setup_attempted.store(true);

    if (geteuid() != 0) {
        std::println(stderr, "[gadget] requested USB gadget nodes are not ready and built-in setup needs root.\n"
                             "[gadget] Run: sudo ./ns-backend ...");
        return false;
    }

    const int ffs_count = functionfs_function_count_for_family();
    const int legacy_count = legacy_hidg_node_count_for_family();

    if (g_ctx.verbose) {
        if (gadget_uses_switch2_identity()) {
            std::println("[gadget] {}; creating one native S2 FunctionFS controller (single-controller mode)",
                         reason ? reason : "USB gadget not ready");
        } else if (gadget_uses_hori_identity()) {
            std::println("[gadget] {}; creating upstream-style 4-interface f_hid HORI gadget",
                         reason ? reason : "USB gadget not ready");
        } else {
            std::println("[gadget] {}; creating upstream-style 4-interface f_hid Switch 1 gadget",
                         reason ? reason : "USB gadget not ready");
        }
    }

    int dummy = 0;
    dummy = std::system("modprobe libcomposite >/dev/null 2>&1 || true"); (void)dummy;
    if (gadget_uses_switch2_identity()) {
        dummy = std::system("modprobe usb_f_fs >/dev/null 2>&1 || true"); (void)dummy;
    }
    dummy = std::system("mountpoint -q /sys/kernel/config "
                        "|| mount -t configfs none /sys/kernel/config >/dev/null 2>&1 || true"); (void)dummy;

    if (!fs::is_directory("/sys/kernel/config/usb_gadget")) {
        std::println(stderr, "[gadget] /sys/kernel/config/usb_gadget is unavailable.");
        return false;
    }

    if (fs::exists(GADGET_DIR)) {
        if (force || !nodes_ready()) { teardown_gadget(); std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
        else return true;
    }
    if (!mkdirs(GADGET_DIR)) return false;

    const fs::path gd = GADGET_DIR;
    const fs::path cd = CONFIG_DIR;
    if (!mkdirs(gd / "strings/0x409")
            || !mkdirs(cd)
            || !mkdirs(gd / "functions"))
        return false;

    const bool any_hori = gadget_uses_hori_identity();
    bool ok = write_file(gd / "bcdDevice",                  gadget_bcd_device())
           && write_file(gd / "bcdUSB",                     "0x0200")
           && write_file(gd / "idVendor",                   gadget_id_vendor())
           && write_file(gd / "idProduct",                  gadget_id_product())
           && write_file(gd / "bDeviceClass",               any_hori ? "0xFF" : "0xEF")
           && write_file(gd / "bDeviceSubClass",            any_hori ? "0xFF" : "0x02")
           && write_file(gd / "bDeviceProtocol",            any_hori ? "0xFF" : "0x01")
           && write_file(gd / "strings/0x409/serialnumber", any_hori ? "000000000001" : g_ctx.usb_serial)
           && write_file(gd / "strings/0x409/manufacturer", any_hori ? "NS Bridge" : "Nintendo")
           && write_file(gd / "strings/0x409/product",      gadget_product_string())
           && write_file(cd / "MaxPower",                   "500")
           // The real Pro Controller 2 configuration is self-powered (0xC0).
           // Keep the existing remote-wakeup attributes for Switch 1 and the
           // plain bus-powered descriptor for HORI mode.
           && write_file(cd / "bmAttributes",               any_hori ? "0x80"
                                                       : (gadget_uses_switch2_identity() ? "0xC0" : "0xA0"));
    if (!ok) return false;

    if (gadget_uses_switch2_identity()) {
        // The real Pro Controller 2 (and PicoSwitch2) enumerate at full speed;
        // 64-byte bulk endpoints are only legal there. Best-effort: older
        // kernels lack this attribute, and the HS descriptors carry valid
        // 512-byte bulk endpoints as the fallback.
        write_file(gd / "max_speed", "full-speed");
    }

    if (gadget_uses_switch2_identity()) switch2_native_init();
    for (int i = 0; i < ffs_count; ++i) {
        if (!create_functionfs_function(i)) return false;
    }
    for (int i = 0; i < legacy_count; ++i) {
        // Function directory names must remain unique even though /dev/hidgN
        // numbering is assigned densely by f_hid in creation/link order.
        const int hid_id = gadget_uses_switch2_identity()
            ? switch2_virtual_port_count() + i
            : i;
        if (!create_hid_function(hid_id)) return false;
    }
    g_ctx.functionfs_transport_active.store(ffs_count > 0, std::memory_order_relaxed);

    std::string UDC = first_udc_name();
    if (UDC.empty()) {
        print_gadget_host_config_error();
        return false;
    }
    if (!write_file(gd / "UDC", UDC)) return false;
    if (g_ctx.verbose) std::println("[gadget] Bound to UDC: {}", UDC);

    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < ffs_count; ++i) {
            if (access(functionfs_ep_in_path(i).c_str(), F_OK) != 0) all_seen = false;
            if (access(functionfs_ep_out_path(i).c_str(), F_OK) != 0) all_seen = false;
            if (access(functionfs_ep_vendor_out_path(i).c_str(), F_OK) != 0) all_seen = false;
            if (access(functionfs_ep_vendor_in_path(i).c_str(), F_OK) != 0) all_seen = false;
            if (access(functionfs_ep_audio_out_path(i).c_str(), F_OK) != 0) all_seen = false;
            if (access(functionfs_ep_audio_in_path(i).c_str(), F_OK) != 0) all_seen = false;
            chmod(functionfs_ep_in_path(i).c_str(), 0666);
            chmod(functionfs_ep_out_path(i).c_str(), 0666);
            chmod(functionfs_ep_vendor_out_path(i).c_str(), 0666);
            chmod(functionfs_ep_vendor_in_path(i).c_str(), 0666);
            chmod(functionfs_ep_audio_out_path(i).c_str(), 0666);
            chmod(functionfs_ep_audio_in_path(i).c_str(), 0666);
        }
        for (int i = 0; i < legacy_count; ++i) {
            const std::string node = "/dev/hidg" + std::to_string(i);
            if (access(node.c_str(), F_OK) != 0) all_seen = false;
            chmod(node.c_str(), 0666);
        }
        if (all_seen) {
            bool io_ok = true;
            for (int i = 0; i < ffs_count; ++i)
                if (!functionfs_start_port_io(i)) io_ok = false;
            if (!io_ok) {
                for (int i = 0; i < ffs_count; ++i) functionfs_stop_port_io(i);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (nodes_ready()) {
                if (g_ctx.verbose) {
                    if (gadget_uses_switch2_identity()) {
                        std::println("[gadget] Done. Exposed one native S2 FunctionFS controller");
                    } else {
                        std::println("[gadget] Done. Exposed {} upstream-style f_hid interface(s) (/dev/hidg*)",
                                     legacy_count);
                    }
                }
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void emergency_unbind_udc() {
    int fd = open(GADGET_UDC_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = write(fd, "\n", 1);
    (void)n;
    close(fd);
}

void teardown_gadget() {
    restore_wake_bt_state(false);
    clear_switch2_usb_activity();
    std::error_code ec;
    const bool had_gadget = fs::exists(GADGET_DIR, ec);
    if (!had_gadget && !functionfs_transport_active()) return;
    if (g_ctx.verbose) std::println("[gadget] Closing USB gadget...");

    if (had_gadget) write_file(fs::path(GADGET_DIR) / "UDC", "");
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        functionfs_stop_port_io(i);
    }
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        fs::remove(fs::path(CONFIG_DIR)  / ("hid.usb" + std::to_string(i)), ec);
        fs::remove(ffs_config_link(i), ec);
    }

    unmount_functionfs_instances();

    if (had_gadget) {
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            fs::remove(fs::path(GADGET_DIR) / "functions" / ("hid.usb" + std::to_string(i)), ec);
            fs::remove(ffs_function_dir(i), ec);
        }
        fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409", ec);
        fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1",               ec);
        fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409",             ec);
        fs::remove(GADGET_DIR,                                                        ec);
    }
    if (g_ctx.verbose) std::println("[gadget] USB gadget closed");
}

bool run_gadget_setup_if_needed(bool force, const char* reason) {
    return setup_gadget_builtin(force, reason);
}
