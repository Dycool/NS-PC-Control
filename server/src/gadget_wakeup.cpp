#include "gadget_wakeup.hpp"
#include "app_state.hpp"
#include "virtual_controller.hpp"
#include "switch2_native.hpp"
#include "s2_uac1_audio.hpp"
#include "s2_rawgadget.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <endian.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <pthread.h>
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

// Raw fd write with bounded zero-progress protection, not std::ofstream.
// Kernel configfs attribute callbacks can return a short write followed by
// repeated zero-progress writes. Bound retries so setup fails instead of
// spinning forever.
static bool write_file(const fs::path& p, const void* data, size_t len) {
    int fd = open(p.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const auto* bytes = static_cast<const char*>(data);
    size_t done = 0;
    int zero_progress_retries = 0;
    bool ok = true;
    while (done < len) {
        ssize_t w = write(fd, bytes + done, len - done);
        if (w > 0) { done += static_cast<size_t>(w); zero_progress_retries = 0; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (++zero_progress_retries > 100) { ok = false; break; }
    }
    close(fd);
    return ok && done == len;
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

static bool gadget_uses_hori_identity() {
    return g_ctx.usb_controller_family == UsbControllerFamily::Hori;
}

static bool gadget_uses_switch2_identity() {
    return g_ctx.usb_controller_family == UsbControllerFamily::Switch2;
}

static bool s2_using_raw_gadget() {
    return gadget_uses_switch2_identity();
}

static constexpr int switch2_virtual_port_count() {
    return 1;
}

static int legacy_hidg_node_count_for_family() {
    // A native S2 USB device is exposed as one controller only. Mixing S1 f_hid
    // interfaces into the same S2 device identity is not accepted reliably by
    // the console, so --s2 creates no legacy fallback nodes.
    return gadget_uses_switch2_identity() ? 0 : HID_PORT_COUNT;
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

} // namespace

bool s2_gadget_transport_active() {
    return s2_rawgadget_transport_active();
}

bool s2_gadget_nodes_ready() {
    return s2_rawgadget_nodes_ready();
}

bool s2_gadget_poll_control_report(int /*id*/, std::vector<unsigned char>& out_report) {
    out_report.clear();
    return false;
}

bool s2_gadget_io_ready(int id) {
    return id == 0 && s2_rawgadget_io_ready();
}

bool s2_gadget_host_enabled(int id) {
    return id == 0 && s2_rawgadget_host_enabled();
}

bool s2_gadget_submit_input_report(int id, const uint8_t* data, size_t len) {
    return id == 0 && s2_rawgadget_submit_input_report(data, len);
}

bool s2_gadget_poll_output_report(int id, std::vector<unsigned char>& out_report) {
    out_report.clear();
    return id == 0 && s2_rawgadget_poll_output_report(out_report);
}

void s2_gadget_drain_output(int id) {
    if (id == 0) s2_rawgadget_drain_output();
}

bool s2_gadget_poll_vendor_report(int id, std::vector<unsigned char>& out_report) {
    out_report.clear();
    return id == 0 && s2_rawgadget_poll_vendor_report(out_report);
}

bool s2_gadget_submit_vendor_report(int id, const uint8_t* data, size_t len) {
    const bool is_nfc = data != nullptr && len != 0 && data[0] == 0x01;
    const bool ok = id == 0 && s2_rawgadget_submit_vendor_report(data, len);
    if (g_ctx.verbose && is_nfc) {
        std::println("[s2][nfc][tx-queue] {} t_us={} port={} len={} raw={}",
                     ok ? "accepted" : "rejected", now_us(), id, len,
                     bytes_to_hex(std::span<const uint8_t>(data, len)));
    }
    return ok;
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
    if (s2_using_raw_gadget()) {
        if (!force && s2_rawgadget_nodes_ready()) {
            if (!s2_uac1_audio_ready()) s2_uac1_audio_start();
            return true;
        }
        if (!force && g_ctx.gadget_setup_attempted.exchange(true)) {
            if (s2_rawgadget_nodes_ready()) {
                if (!s2_uac1_audio_ready()) s2_uac1_audio_start();
                return true;
            }
            force = true;
        }
        if (force) g_ctx.gadget_setup_attempted.store(true);
        if (geteuid() != 0) {
            std::println(stderr, "[gadget] requested USB gadget nodes are not ready and built-in setup needs root.\n"
                                 "[gadget] Run: sudo ./ns-backend ...");
            return false;
        }
        const bool ready = s2_rawgadget_setup(force, reason);
        if (ready && !s2_uac1_audio_start()) {
            s2_rawgadget_teardown();
            return false;
        }
        return ready;
    }

    if (s2_rawgadget_nodes_ready()) {
        s2_uac1_audio_stop();
        s2_rawgadget_teardown();
    }

    auto nodes_ready = [&]() { return hidg_nodes_ready_for_family(); };

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

    const int legacy_count = legacy_hidg_node_count_for_family();

    if (g_ctx.verbose) {
        if (gadget_uses_hori_identity()) {
            std::println("[gadget] {}; creating upstream-style 4-interface f_hid HORI gadget",
                         reason ? reason : "USB gadget not ready");
        } else {
            std::println("[gadget] {}; creating upstream-style 4-interface f_hid Switch 1 gadget",
                         reason ? reason : "USB gadget not ready");
        }
    }

    int dummy = 0;
    dummy = std::system("modprobe libcomposite >/dev/null 2>&1 || true"); (void)dummy;
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
           && write_file(cd / "bmAttributes",               any_hori ? "0x80" : "0xA0");
    if (!ok) return false;

    for (int i = 0; i < legacy_count; ++i) {
        if (!create_hid_function(i)) return false;
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
        for (int i = 0; i < legacy_count; ++i) {
            const std::string node = "/dev/hidg" + std::to_string(i);
            if (access(node.c_str(), F_OK) != 0) all_seen = false;
            chmod(node.c_str(), 0666);
        }
        if (all_seen && nodes_ready()) {
            if (g_ctx.verbose) {
                std::println("[gadget] Done. Exposed {} upstream-style f_hid interface(s) (/dev/hidg*)",
                             legacy_count);
            }
            return true;
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
    if (s2_rawgadget_nodes_ready() || s2_rawgadget_transport_active()) {
        s2_uac1_audio_stop();
        s2_rawgadget_teardown();
        if (s2_using_raw_gadget()) return;
    }
    std::error_code ec;
    const bool had_gadget = fs::exists(GADGET_DIR, ec);
    if (!had_gadget) return;
    if (g_ctx.verbose) std::println("[gadget] Closing USB gadget...");

    write_file(fs::path(GADGET_DIR) / "UDC", "");
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        fs::remove(fs::path(CONFIG_DIR) / ("hid.usb" + std::to_string(i)), ec);
    }
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        fs::remove(fs::path(GADGET_DIR) / "functions" / ("hid.usb" + std::to_string(i)), ec);
    }
    fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409", ec);
    fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1",               ec);
    fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409",             ec);
    fs::remove(GADGET_DIR,                                                        ec);
    if (g_ctx.verbose) std::println("[gadget] USB gadget closed");
}

bool run_gadget_setup_if_needed(bool force, const char* reason) {
    return setup_gadget_builtin(force, reason);
}
