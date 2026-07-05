#include "gadget_wakeup.hpp"
#include "app_state.hpp"
#include "virtual_controller.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <endian.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <print>
#include <signal.h>
#include <sstream>
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
    uint8_t idle_rate = 0;
    uint8_t protocol = 1;
    std::deque<std::vector<uint8_t>> control_reports;
};

std::array<FfsPortState, HID_PORT_COUNT> g_ffs_ports;

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

static std::vector<uint8_t> ffs_report_descriptor() {
    // FunctionFS always advertises the NFC-capable descriptor. Normal play still
    // emits 0x30/0x21/0x81 reports; 0x31 is only written when the console enters
    // NFC/IR mode. This removes the old f_hid report_length re-enumeration dance.
    return std::vector<uint8_t>(VIRTUAL_CONTROLLER_REPORT_DESC_NFC,
                                VIRTUAL_CONTROLLER_REPORT_DESC_NFC + VIRTUAL_CONTROLLER_REPORT_DESC_NFC_SIZE);
}

static HidDescriptor make_hid_descriptor() {
    HidDescriptor hid{};
    hid.bLength = sizeof(HidDescriptor);
    hid.bDescriptorType = 0x21;       // HID descriptor
    hid.bcdHID = htole16(0x0111);
    hid.bCountryCode = 0;
    hid.bNumDescriptors = 1;
    hid.bReportDescriptorType = 0x22; // Report descriptor
    hid.wDescriptorLength = htole16(static_cast<uint16_t>(VIRTUAL_CONTROLLER_REPORT_DESC_NFC_SIZE));
    return hid;
}

static usb_interface_descriptor make_hid_interface_descriptor() {
    usb_interface_descriptor intf{};
    intf.bLength = USB_DT_INTERFACE_SIZE;
    intf.bDescriptorType = USB_DT_INTERFACE;
    intf.bInterfaceNumber = 0; // FunctionFS/composite assigns the real number.
    intf.bAlternateSetting = 0;
    intf.bNumEndpoints = 2;
    intf.bInterfaceClass = 0x03; // HID
    intf.bInterfaceSubClass = 0x00;
    intf.bInterfaceProtocol = 0x00;
    intf.iInterface = 1;
    return intf;
}

static usb_endpoint_descriptor_no_audio make_hid_endpoint_descriptor(bool in, bool high_speed) {
    usb_endpoint_descriptor_no_audio ep{};
    ep.bLength = USB_DT_ENDPOINT_SIZE;
    ep.bDescriptorType = USB_DT_ENDPOINT;
    ep.bEndpointAddress = static_cast<uint8_t>((in ? USB_DIR_IN : USB_DIR_OUT) | 0x01);
    ep.bmAttributes = USB_ENDPOINT_XFER_INT;
    // Keep endpoint packets controller-like. FunctionFS accepts a 362-byte IN
    // transfer for report 0x31 and the UDC splits it into endpoint packets.
    ep.wMaxPacketSize = htole16(PRO_REPORT_SIZE);
    ep.bInterval = high_speed ? 4 : 4;
    return ep;
}

static void append_hid_function_descriptors(std::vector<uint8_t>& out, bool high_speed) {
    const auto intf = make_hid_interface_descriptor();
    const auto hid = make_hid_descriptor();
    const auto ep_in = make_hid_endpoint_descriptor(true, high_speed);
    const auto ep_out = make_hid_endpoint_descriptor(false, high_speed);
    append_obj(out, intf);
    append_obj(out, hid);
    append_obj(out, ep_in);
    append_obj(out, ep_out);
}

static std::vector<uint8_t> build_functionfs_descriptors() {
    std::vector<uint8_t> out;
    append_u32(out, FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    const size_t length_pos = out.size();
    append_u32(out, 0); // patched below
    append_u32(out, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    append_u32(out, 4); // FS: interface + HID + IN ep + OUT ep
    append_u32(out, 4); // HS: interface + HID + IN ep + OUT ep
    append_hid_function_descriptors(out, false);
    append_hid_function_descriptors(out, true);
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
    for (auto& p : g_ffs_ports) {
        if (p.ep0_fd >= 0) close(p.ep0_fd);
        p.ep0_fd = -1;
        p.descriptors_written = false;
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

    const auto descs = build_functionfs_descriptors();
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

static bool ep0_write_data(int fd, const uint8_t* data, size_t len, size_t limit) {
    len = std::min(len, limit);
    if (len == 0) return ep0_write_status(fd);
    return write_all_fd(fd, data, len);
}

static void queue_control_report(int id, uint16_t w_value, const std::vector<uint8_t>& payload) {
    if (id < 0 || id >= HID_PORT_COUNT || payload.empty()) return;
    auto report = payload;
    const uint8_t report_id = static_cast<uint8_t>(w_value & 0xFF);
    if (report_id != 0 && report.front() != report_id) report.insert(report.begin(), report_id);
    auto& q = g_ffs_ports[id].control_reports;
    if (q.size() >= 16) q.pop_front();
    q.push_back(std::move(report));
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

    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && dir_in && req == USB_REQ_GET_DESCRIPTOR) {
        if (desc_type == 0x22) { // HID report descriptor
            const auto report = ffs_report_descriptor();
            ep0_write_data(st.ep0_fd, report.data(), report.size(), length);
            return;
        }
        if (desc_type == 0x21) { // HID descriptor
            const auto hid = make_hid_descriptor();
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
            const uint8_t alt_setting = 0;
            ep0_write_data(st.ep0_fd, &alt_setting, 1, length);
            return;
        }
        if (!dir_in && length > 0) {
            std::vector<uint8_t> discard;
            read_ep0_payload(st.ep0_fd, discard, length);
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
        ep0_write_status(st.ep0_fd);
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
                std::vector<uint8_t> payload;
                if (length > 0 && read_ep0_payload(st.ep0_fd, payload, length)) {
                    queue_control_report(id, value, payload);
                }
                ep0_write_status(st.ep0_fd);
                return;
            }
            case 0x0A: // SET_IDLE
                st.idle_rate = static_cast<uint8_t>(value >> 8);
                ep0_write_status(st.ep0_fd);
                return;
            case 0x0B: // SET_PROTOCOL
                st.protocol = static_cast<uint8_t>(value & 0xFF);
                ep0_write_status(st.ep0_fd);
                return;
            default:
                ep0_write_status(st.ep0_fd);
                return;
        }
    }

    if (!dir_in && length > 0) {
        std::vector<uint8_t> discard;
        read_ep0_payload(st.ep0_fd, discard, length);
    }
    ep0_write_status(st.ep0_fd);
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
            switch (events[e].type) {
                case FUNCTIONFS_SETUP:
                    handle_functionfs_setup(id, events[e].u.setup);
                    break;
                case FUNCTIONFS_DISABLE:
                case FUNCTIONFS_UNBIND:
                    mark_switch2_usb_host_disconnected();
                    break;
                case FUNCTIONFS_ENABLE:
                case FUNCTIONFS_BIND:
                case FUNCTIONFS_SUSPEND:
                case FUNCTIONFS_RESUME:
                default:
                    break;
            }
        }
    }
}

} // namespace

bool functionfs_transport_active() {
    return g_ctx.functionfs_transport_active.load(std::memory_order_relaxed);
}

std::string functionfs_ep_in_path(int id) {
    return (ffs_mount_dir(id) / "ep1").string();
}

std::string functionfs_ep_out_path(int id) {
    return (ffs_mount_dir(id) / "ep2").string();
}

bool functionfs_nodes_ready() {
    if (!functionfs_transport_active()) return false;
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (g_ffs_ports[i].ep0_fd < 0 || !g_ffs_ports[i].descriptors_written) return false;
        if (access(functionfs_ep_in_path(i).c_str(), R_OK | W_OK) != 0) return false;
        if (access(functionfs_ep_out_path(i).c_str(), R_OK | W_OK) != 0) return false;
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

bool usb_transport_supports_nfc_reports() {
    return functionfs_transport_active()
        || g_ctx.nfc_gadget_active.load(std::memory_order_relaxed);
}

static bool create_hid_function(int id) {
    fs::path func = fs::path(GADGET_DIR) / "functions" / ("hid.usb" + std::to_string(id));
    // report_length sets both the write cap and the interrupt endpoint size in
    // f_hid, so it stays 64 for the modern gadget unless --amiibo needs the
    // 362-byte NFC report.
    const std::string report_length = g_ctx.legacy_mode ? "8" : std::to_string(active_hidg_report_length());
    if (!mkdirs(func)
            || !write_file(func / "protocol",      "0")
            || !write_file(func / "subclass",       "0")
            || !write_file(func / "report_length",  report_length))
        return false;

    fs::path desc_path = func / "report_desc";
    if (g_ctx.legacy_mode) {
        if (!write_file(desc_path, LEGACY_REPORT_DESC, sizeof(LEGACY_REPORT_DESC))) return false;
    } else {
        size_t desc_len = 0;
        const uint8_t* desc = active_report_descriptor(desc_len);
        if (!write_file(desc_path, desc, desc_len)) return false;
    }

    fs::path link_path = fs::path(CONFIG_DIR) / ("hid.usb" + std::to_string(id));
    std::error_code ec;
    fs::remove(link_path, ec);
    return symlink(func.c_str(), link_path.c_str()) == 0;
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
    const bool use_functionfs = !g_ctx.legacy_mode;
    auto nodes_ready = [&]() { return use_functionfs ? functionfs_nodes_ready() : hidg_nodes_ready(); };

    if (!force && nodes_ready()) return true;
    if (!force && g_ctx.gadget_setup_attempted.exchange(true)) {
        if (nodes_ready()) return true;
        // FunctionFS has more moving parts than f_hid (mounts + ep0 descriptors).
        // If a previous setup was interrupted half-way, rebuild it instead of
        // getting stuck forever with missing endpoints.
        if (use_functionfs) force = true;
        else return false;
    }
    if (force) g_ctx.gadget_setup_attempted.store(true);

    if (geteuid() != 0) {
        std::println(stderr, "[gadget] requested USB gadget nodes are not ready and built-in setup needs root.\n"
                             "[gadget] Run: sudo ./ns-backend ...");
        return false;
    }

    if (g_ctx.verbose) {
        std::println("[gadget] {}; creating built-in {}-interface {} gadget",
                     reason ? reason : "USB gadget not ready",
                     HID_PORT_COUNT,
                     g_ctx.legacy_mode ? "legacy 8-byte HID" : "FunctionFS Pro/NFC HID");
    }

    int dummy = 0;
    dummy = std::system("modprobe libcomposite >/dev/null 2>&1 || true"); (void)dummy;
    if (use_functionfs) {
        dummy = std::system("modprobe usb_f_fs >/dev/null 2>&1 || true");
        (void)dummy;
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
            || !mkdirs(g_ctx.legacy_mode ? cd / "strings/0x409" : cd)
            || !mkdirs(gd / "functions"))
        return false;

    // Modern FunctionFS keeps the same Nintendo VID/PID/product surface, but the
    // HID interface itself is owned by user space. That lets us answer HID report
    // descriptor requests and accept SET_REPORT control traffic directly.
    bool ok = write_file(gd / "bcdDevice",                  g_ctx.legacy_mode ? "0x0200" : "0x0210")
           && write_file(gd / "bcdUSB",                     "0x0200")
           && write_file(gd / "idVendor",                   g_ctx.legacy_mode ? "0x0F0D" : "0x057e")
           && write_file(gd / "idProduct",                  g_ctx.legacy_mode ? "0x0092" : "0x2009")
           && write_file(gd / "bDeviceClass",               g_ctx.legacy_mode ? "0xFF"   : "0x00")
           && write_file(gd / "bDeviceSubClass",            g_ctx.legacy_mode ? "0xFF"   : "0x00")
           && write_file(gd / "bDeviceProtocol",            g_ctx.legacy_mode ? "0xFF"   : "0x00")
           && write_file(gd / "strings/0x409/serialnumber", g_ctx.legacy_mode ? "000000000001" : g_ctx.usb_serial)
           && write_file(gd / "strings/0x409/manufacturer", "NS Bridge")
           && write_file(gd / "strings/0x409/product",      g_ctx.legacy_mode ? "Legacy USB Gamepad" : "Motion USB Gamepad")
           && write_file(cd / "MaxPower",                   "500")
           && write_file(cd / "bmAttributes",               g_ctx.legacy_mode ? "0x80" : "0xA0");
    if (g_ctx.legacy_mode) ok = ok && write_file(cd / "strings/0x409/configuration", "USB 4-Player Hub Config");
    if (!ok) return false;

    if (use_functionfs) {
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (!create_functionfs_function(i)) return false;
        }
        g_ctx.functionfs_transport_active.store(true, std::memory_order_relaxed);
        // FunctionFS always exposes the NFC report descriptor, so there is no
        // report_length edge to arm here. The atomic tracks actual staged tags.
        g_ctx.nfc_gadget_active.store(false, std::memory_order_relaxed);
    } else {
        g_ctx.functionfs_transport_active.store(false, std::memory_order_relaxed);
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (!create_hid_function(i)) return false;
        }
    }

    std::string UDC = first_udc_name();
    if (UDC.empty()) {
        print_gadget_host_config_error();
        return false;
    }
    if (!write_file(gd / "UDC", UDC)) return false;
    if (g_ctx.verbose) std::println("[gadget] Bound to UDC: {}", UDC);

    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (use_functionfs) {
                if (access(functionfs_ep_in_path(i).c_str(), F_OK) != 0) all_seen = false;
                if (access(functionfs_ep_out_path(i).c_str(), F_OK) != 0) all_seen = false;
                chmod(functionfs_ep_in_path(i).c_str(), 0666);
                chmod(functionfs_ep_out_path(i).c_str(), 0666);
            } else {
                char path[32];
                std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
                if (access(path, F_OK) != 0) all_seen = false;
                chmod(path, 0666);
            }
        }
        if (all_seen && nodes_ready()) {
            if (g_ctx.verbose) {
                if (use_functionfs)
                    std::println("[gadget] Done. Exposed {} FunctionFS HID interface(s) ({}/port*/ep1+ep2)",
                                 HID_PORT_COUNT, FFS_BASE_DIR);
                else
                    std::println("[gadget] Done. Exposed {} USB gamepad HID interface(s) (/dev/hidg0..{})",
                                 HID_PORT_COUNT, HID_PORT_COUNT - 1);
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
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
        fs::remove(GADGET_DIR,                                                          ec);
    }
    if (g_ctx.verbose) std::println("[gadget] USB gadget closed");
}

bool run_gadget_setup_if_needed(bool force, const char* reason) {
    return setup_gadget_builtin(force, reason);
}
