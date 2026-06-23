#include "gadget_wakeup.hpp"
#include "app_state.hpp"
#include "virtual_controller.hpp"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <print>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <filesystem>
#include <vector>
#include <iostream>
#include <mutex>

namespace fs = std::filesystem;
using namespace ns;

constexpr const char* GADGET_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl";
constexpr const char* CONFIG_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1";

static std::string g_saved_bt_mac;
static std::string g_saved_bt_hci;
static std::string g_switch2_wake_original_bt_mac;
static std::atomic<bool> g_bt_modified_for_wake{false};
static std::mutex g_wake_bt_mtx;

bool hidg_nodes_ready() {
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (access(("/dev/hidg" + std::to_string(i)).c_str(), R_OK | W_OK) != 0) return false;
    }
    return true;
}

bool mkdirs(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return !ec || ec.value() == EEXIST;
}

bool write_file(const fs::path& p, const void* data, size_t len) {
    std::ofstream f(p, std::ios::binary);
    return f && f.write(static_cast<const char*>(data), len).good();
}

bool write_file(const fs::path& p, const std::string& text) {
    return write_file(p, text.data(), text.size());
}

std::string first_udc_name() {
    std::error_code ec;
    if (fs::exists("/sys/class/udc", ec)) {
        for (const auto& entry : fs::directory_iterator("/sys/class/udc", ec)) return entry.path().filename().string();
    }
    return "";
}

bool create_hid_function(int id) {
    fs::path func = fs::path(GADGET_DIR) / "functions" / ("hid.usb" + std::to_string(id));
    if (!mkdirs(func) || !write_file(func / "protocol", "0") || !write_file(func / "subclass", "0") ||
        !write_file(func / "report_length", g_ctx.legacy_mode ? "8" : "64")) return false;
    fs::path desc_path = func / "report_desc";
    if (g_ctx.legacy_mode) {
        if (!write_file(desc_path, LEGACY_REPORT_DESC, sizeof(LEGACY_REPORT_DESC))) return false;
    } else {
        if (!write_file(desc_path, VIRTUAL_CONTROLLER_REPORT_DESC, sizeof(VIRTUAL_CONTROLLER_REPORT_DESC))) return false;
    }
    fs::path link_path = fs::path(CONFIG_DIR) / ("hid.usb" + std::to_string(id));
    std::error_code ec;
    fs::remove(link_path, ec);
    return symlink(func.c_str(), link_path.c_str()) == 0;
}

std::string trim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    return s;
}

std::string to_lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string to_upper_no_space(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool valid_mac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (int i = 0; i < 17; ++i) if (i % 3 == 2 ? mac[i] != ':' : !std::isxdigit((unsigned char)mac[i])) return false;
    return true;
}

bool valid_adv_hex(const std::string& adv) {
    return !adv.empty() && adv.size() % 2 == 0 && adv.size() <= 62 && std::ranges::all_of(adv, [](char c) { return std::isxdigit((unsigned char)c); });
}

bool valid_hci(const std::string& hci) {
    return hci.size() >= 4 && hci.starts_with("hci") && std::ranges::all_of(hci.begin() + 3, hci.end(), ::isdigit);
}

bool command_exists(const char* cmd) {
    return std::system(std::format("command -v {} >/dev/null 2>&1", cmd).c_str()) == 0;
}

bool read_switch2_wakeup_config_file(const std::string& path, std::string& mac, std::string& adv, std::string& hci, std::string* original = nullptr) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line, orig;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (key == "mac") mac = to_lower(val);
        else if (key == "adv") adv = to_upper_no_space(val);
        else if (key == "hci") hci = val;
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
    g_ctx.switch2_wake_mac = mac;
    g_ctx.switch2_wake_adv_hex = adv;
    g_ctx.switch2_wake_hci_dev = valid_hci(hci) ? hci : "hci0";
    g_switch2_wake_original_bt_mac = orig;
    g_ctx.switch2_wake_config_loaded = true;
    return true;
}

bool save_switch2_wakeup_config(const std::string& mac, const std::string& adv, const std::string& hci_dev, const std::string& original) {
    std::error_code ec;
    fs::create_directories(fs::path(g_ctx.switch2_wakeup_config_path).parent_path(), ec);
    std::ofstream f(g_ctx.switch2_wakeup_config_path, std::ios::trunc);
    if (!f) return false;
    f << "# NS-PC-Control Switch 2 wake config\nmac=" << to_lower(mac) << "\nadv=" << to_upper_no_space(adv)
      << "\nhci=" << (valid_hci(hci_dev) ? hci_dev : "hci0") << "\n";
    if (valid_mac(original)) f << "original_mac=" << to_lower(original) << "\n";
    f.close();
    chmod(g_ctx.switch2_wakeup_config_path.c_str(), 0600);
    return true;
}

std::vector<std::string> adv_hex_to_cmd_args(const std::string& adv_hex) {
    std::string adv = to_upper_no_space(adv_hex);
    size_t bytes = adv.size() / 2;
    std::vector<std::string> args = { std::format("{:02X}", bytes) };
    for (size_t i = 0; i < 31; ++i) args.push_back(i < bytes ? adv.substr(i * 2, 2) : "00");
    return args;
}

struct WakeCmdResult { int exit_code = -1; std::string output; };

WakeCmdResult run_wake_command(const std::vector<std::string>& args, bool verbose, bool capture = false, int timeout_sec = 3) {
    WakeCmdResult res;
    if (args.empty()) return res;
    if (timeout_sec <= 0) timeout_sec = 3;
    if (g_ctx.bluetooth_disabled) {
        bool is_bt = false;
        for (const auto& a : args) {
            std::string al = a;
            std::transform(al.begin(), al.end(), al.begin(), [](unsigned char c){ return std::tolower(c); });
            if (al == "rfkill" || al == "bluetoothctl" || al == "btmgmt" || al == "hcitool" || al == "hciconfig" || al.find("bluetooth") != std::string::npos) {
                is_bt = true;
                break;
            }
        }
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
    int status = pclose(pipe);
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

bool wake_cmd_ok(const std::vector<std::string>& args, bool verbose) {
    return run_wake_command(args, verbose).exit_code == 0;
}

static std::string read_hci_address(const std::string& hci_dev) {
    std::ifstream f("/sys/class/bluetooth/" + hci_dev + "/address");
    if (!f) return {};
    std::string mac;
    std::getline(f, mac);
    return valid_mac(mac) ? to_lower(mac) : "";
}

void wake_disable_advertising_quiet(const std::string& hci_dev) {
    run_wake_command({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000A", "00"}, false);
}

void wake_clear_scan_response_quiet(const std::string& hci_dev, bool verbose) {
    // LE Set Scan Response Data: zero-length payload padded to 31 bytes.
    wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0009",
                 "00", "00", "00", "00", "00", "00", "00", "00", "00", "00", "00", "00",
                 "00", "00", "00", "00", "00", "00", "00", "00", "00", "00", "00", "00",
                 "00", "00", "00", "00", "00", "00", "00", "00"}, verbose);
}

void restore_bluetooth_controller_state(const std::string& hci, bool restart_bluez);

void finish_prepared_wake_controller(const std::string& hci_dev, bool restart_bluez) {
    if (!g_bt_modified_for_wake.load(std::memory_order_relaxed)) return;
    restore_bluetooth_controller_state(hci_dev, restart_bluez);
    g_saved_bt_mac.clear();
    g_saved_bt_hci.clear();
    g_bt_modified_for_wake.store(false, std::memory_order_relaxed);
}

std::string paired_adapter_mac_from_bluez_store(const std::string& wake_mac) {
    std::error_code ec;
    if (!fs::exists("/var/lib/bluetooth", ec)) return "";
    std::string best;
    std::string wake_lc = to_lower(wake_mac);
    for (const auto& entry : fs::directory_iterator("/var/lib/bluetooth", ec)) {
        std::string name = to_lower(entry.path().filename().string());
        if (valid_mac(name) && name != wake_lc && (best.empty() || name < best)) best = name;
    }
    return best;
}

std::string original_bt_mac_for_runtime(const std::string& hci) {
    if (valid_mac(g_switch2_wake_original_bt_mac)) return g_switch2_wake_original_bt_mac;
    std::string mac, adv, cfg_hci, original;
    if (read_switch2_wakeup_config_file(g_ctx.switch2_wakeup_config_path, mac, adv, cfg_hci, &original) && valid_mac(original)) {
        g_switch2_wake_original_bt_mac = original;
        return original;
    }
    std::string paired = paired_adapter_mac_from_bluez_store(g_ctx.switch2_wake_mac);
    return valid_mac(paired) ? paired : read_hci_address(hci);
}

void restore_bluetooth_controller_state(const std::string& hci, bool restart_bluez) {
    if (g_ctx.bluetooth_disabled) return;
    std::string current = read_hci_address(hci), original = original_bt_mac_for_runtime(hci);
    wake_disable_advertising_quiet(hci);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
    run_wake_command({"systemctl", "stop", "bluetooth"}, false);
    run_wake_command({"btmgmt", "-i", hci, "power", "off"}, false);
    run_wake_command({"btmgmt", "-i", hci, "privacy", "off"}, false);
    run_wake_command({"btmgmt", "-i", hci, "bredr", "on"}, false);
    run_wake_command({"btmgmt", "-i", hci, "le", "on"}, false);
    if (valid_mac(original) && current != original) run_wake_command({"btmgmt", "-i", hci, "public-addr", original}, false);
    run_wake_command({"btmgmt", "-i", hci, "power", "on"}, false);
    if (restart_bluez) {
        run_wake_command({"systemctl", "restart", "bluetooth"}, false);
        run_wake_command({"bluetoothctl", "power", "on"}, false);
        run_wake_command({"bluetoothctl", "agent", "NoInputNoOutput"}, false);
        run_wake_command({"bluetoothctl", "default-agent"}, false);
    }
}

bool reset_wake_bt_stack(std::string& hci_dev, bool verbose) {
    if (hci_dev.empty()) hci_dev = "hci0";
    if (verbose) std::println(stderr, "[wake] Resetting Bluetooth stack / hciuart");
    run_wake_command({"systemctl", "stop", "bluetooth"}, verbose);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, verbose);
    wake_disable_advertising_quiet(hci_dev);
    run_wake_command({"hciconfig", hci_dev, "down"}, verbose);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    run_wake_command({"systemctl", "restart", "hciuart"}, verbose);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    run_wake_command({"hciconfig", hci_dev, "up"}, verbose);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    wake_disable_advertising_quiet(hci_dev);
    return true;
}

bool prepare_wake_controller(std::string& hci_dev, const std::string& mac_lc, bool verbose) {
    if (hci_dev.empty()) hci_dev = "hci0";
    run_wake_command({"systemctl", "stop", "bluetooth"}, false);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
    if (!g_bt_modified_for_wake.load()) {
        g_saved_bt_mac = read_hci_address(hci_dev);
        g_saved_bt_hci = hci_dev;
        g_bt_modified_for_wake = true;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (verbose) std::println("[wake] Preparing Bluetooth controller, attempt {}, device {}", attempt, hci_dev);
        wake_cmd_ok({"btmgmt", "-i", hci_dev, "power", "off"}, verbose);
        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "privacy", "off"}, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "bredr", "off"}, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "le", "on"}, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "public-addr", mac_lc}, verbose) ||
            !wake_cmd_ok({"btmgmt", "-i", hci_dev, "power", "on"}, verbose) ||
            !wake_cmd_ok({"hciconfig", hci_dev, "up"}, verbose)) {
            reset_wake_bt_stack(hci_dev, verbose);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::string current = read_hci_address(hci_dev);
        if (!current.empty() && current != mac_lc) {
            reset_wake_bt_stack(hci_dev, verbose);
            continue;
        }
        if (verbose) std::println("[wake] Bluetooth controller prepared as {}", mac_lc);
        return true;
    }
    return false;
}

bool start_wake_raw_advertising(const std::string& hci_dev, const std::string& mac_lc, const std::string& adv_uc, int seconds, bool verbose) {
    wake_disable_advertising_quiet(hci_dev);
    std::vector<std::string> cmd = {"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0008"};
    auto adv_args = adv_hex_to_cmd_args(adv_uc);
    cmd.insert(cmd.end(), adv_args.begin(), adv_args.end());
    if (!wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0006", "20", "00", "40", "00", "03", "00", "00", "00", "00", "00", "00", "00", "00", "07", "00"}, verbose) ||
        !wake_cmd_ok(cmd, verbose)) {
        return false;
    }
    wake_clear_scan_response_quiet(hci_dev, verbose);
    if (verbose) std::println("[wake] Enable advertising as {} for {}s", mac_lc, seconds);
    if (!wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000A", "01"}, verbose)) return false;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (g_ctx.running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    wake_disable_advertising_quiet(hci_dev);
    return true;
}

bool send_switch2_wake_advert_once(const std::string& mac, const std::string& adv_hex, int seconds, bool verbose, bool force_prepare = false) {
    if (!valid_mac(mac) || !valid_adv_hex(adv_hex)) return false;
    if (seconds <= 0) seconds = 1;
    if (seconds > 10) seconds = 10;
    std::string mac_lc = to_lower(mac), adv_uc = to_upper_no_space(adv_hex);
    std::string hci_dev = valid_hci(g_ctx.switch2_wake_hci_dev) ? g_ctx.switch2_wake_hci_dev : "hci0";

    // Do not let two wake attempts fight over the same HCI controller.
    std::lock_guard<std::mutex> wake_lock(g_wake_bt_mtx);

    const bool bt_controller_active = any_client_source_active(InputSource::Bluetooth, now_us());
    const bool allow_full_prepare = force_prepare || !bt_controller_active;

    if (verbose) {
        std::println("[wake] Wake MAC: {}\n[wake] ADV bytes: {}\n[wake] Duration: {}s", mac_lc, adv_uc.size() / 2, seconds);
        if (allow_full_prepare) {
            std::println("[wake] Reliable path: prepare adapter as captured Joy-Con 2 MAC on {}", hci_dev);
        } else {
            std::println("[wake] Soft path: Bluetooth controller is connected; sending raw ADV without adapter reset/MAC spoof");
        }
    }

    bool ok = false;
    bool prepared_adapter = false;

    if (allow_full_prepare) {
        if (prepare_wake_controller(hci_dev, mac_lc, verbose)) {
            prepared_adapter = true;
            ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose);
        }
        if (!ok && force_prepare) {
            if (verbose) std::println(stderr, "[wake] Raw advertising failed. Trying one Bluetooth stack reset, then retrying once.");
            if (reset_wake_bt_stack(hci_dev, verbose) && prepare_wake_controller(hci_dev, mac_lc, verbose)) {
                prepared_adapter = true;
                ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose);
            }
        }
    } else {
        // Important: when a local BT controller is connected, do not stop bluetoothd,
        // power-cycle hci0, disable BR/EDR, or change the public address. Those actions
        // break the connected controller. This best-effort path only pokes the LE
        // advertising commands directly and then immediately disables advertising.
        ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose);
        if (!ok) {
            std::println(stderr, "[wake] soft wake advert failed while Bluetooth controller is connected; not resetting adapter to avoid disconnecting it");
        }
    }

    wake_disable_advertising_quiet(hci_dev);

    if (prepared_adapter && !force_prepare) {
        // Runtime wake borrowed the adapter for Joy-Con-MAC advertising; return it to
        // normal BlueZ/SDL controller mode before future local controllers connect.
        finish_prepared_wake_controller(hci_dev, true);
    }

    return ok;
}

void switch2_wake_adv_worker(int burst_ms) {
    int seconds = std::max(1, (burst_ms + 999) / 1000);
    send_switch2_wake_advert_once(g_ctx.switch2_wake_mac, g_ctx.switch2_wake_adv_hex, seconds, g_ctx.verbose, false);
    g_ctx.switch2_wake_adv_running.store(false, std::memory_order_relaxed);
}

void switch2_delayed_wake_check_worker(const char* reason) {
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
    (void)reason;
    if (!g_ctx.switch2_wake_adv_enabled || !g_ctx.switch2_wake_config_loaded || g_ctx.switch2_wake_adv_running.exchange(true)) return;
    uint64_t now = now_us();
    if (g_ctx.switch2_usb_host_connected.load() && !g_ctx.switch2_force_next_wake.load()) {
        g_ctx.switch2_wake_adv_running.store(false);
        return;
    }
    if (!g_ctx.switch2_force_next_wake.load()) {
        uint64_t last = g_ctx.switch2_last_wake_adv_us.load();
        if (last && now - last < SWITCH2_WAKE_ADV_COOLDOWN_US) {
            g_ctx.switch2_wake_adv_running.store(false);
            return;
        }
    }
    g_ctx.switch2_force_next_wake.store(false);
    g_ctx.switch2_last_wake_adv_us.store(now);
    std::println("[wake] waking up Switch 2");
    std::thread(switch2_wake_adv_worker, SWITCH2_WAKE_ADV_BURST_MS).detach();
}

bool parse_nintendo_adv_from_btmon_log(const std::string& path, const std::string& preferred_mac, std::string& out_mac, std::string& out_adv) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line, cur_mac, preferred = to_lower(preferred_mac);
    while (std::getline(f, line)) {
        std::string lower = to_lower(line);
        size_t ap = lower.find("address:");
        if (ap != std::string::npos) {
            size_t p = ap + 8;
            while (p < line.size() && std::isspace((unsigned char)line[p])) ++p;
            if (p + 17 <= line.size()) {
                std::string cand = to_lower(line.substr(p, 17));
                if (valid_mac(cand)) cur_mac = cand;
            }
        }
        size_t dp = lower.find("data[24]:");
        if (dp == std::string::npos) continue;
        size_t p = line.find(':', dp);
        if (p == std::string::npos) continue;
        std::string data = to_upper_no_space(line.substr(p + 1));
        if (data.size() != 48 || cur_mac.empty() || (!preferred.empty() && cur_mac != preferred)) continue;
        out_mac = cur_mac;
        out_adv = "0201061BFF5305" + data;
        if (valid_adv_hex(out_adv)) return true;
    }
    return false;
}

bool capture_switch2_wake_advert(int seconds, const std::string& preferred_mac, std::string& out_mac, std::string& out_adv) {
    char log_path[128];
    std::snprintf(log_path, sizeof(log_path), "/tmp/ns_switch2_wake_%ld.log", (long)getpid());
    std::ostringstream cmd;
    std::string hci = valid_hci(g_ctx.switch2_wake_hci_dev) ? g_ctx.switch2_wake_hci_dev : "hci0";
    cmd << "sh -c 'rm -f " << log_path << "; timeout " << seconds << " btmon -T > " << log_path << " 2>&1 & mon=$!; sleep 1; timeout "
        << std::max(1, seconds - 2) << " btmgmt -i " << hci << " find -l >/dev/null 2>&1 || true; kill $mon >/dev/null 2>&1; wait $mon >/dev/null 2>&1'";
    run_wake_command({"systemctl", "stop", "bluetooth"}, false);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
    run_wake_command({"btmgmt", "-i", hci, "power", "off"}, false);
    run_wake_command({"btmgmt", "-i", hci, "privacy", "off"}, false);
    run_wake_command({"btmgmt", "-i", hci, "bredr", "off"}, false);
    run_wake_command({"btmgmt", "-i", hci, "le", "on"}, false);
    run_wake_command({"btmgmt", "-i", hci, "power", "on"}, false);
    int dummy = std::system(cmd.str().c_str()); (void)dummy;
    bool ok = parse_nintendo_adv_from_btmon_log(log_path, preferred_mac, out_mac, out_adv);
    unlink(log_path);
    return ok;
}

void wait_for_enter(const char* prompt) {
    if (prompt && *prompt) std::print("{}", prompt);
    std::fflush(stdout);
    std::string d;
    std::getline(std::cin, d);
}

bool auto_find_joycon_for_setup(std::string& joycon_mac) {
    std::println("\n[wake] Step 1/4: Finding your Joy-Con 2 automatically.");
    std::println("[wake] Put the Joy-Con 2 very close to the Pi and hold the small SYNC button.");
    std::string adv;
    for (int attempt = 1; attempt <= 12; ++attempt) {
        std::println("[wake] Scanning for Nintendo/Joy-Con 2 advert... attempt {}/12", attempt);
        if (capture_switch2_wake_advert(10, joycon_mac, joycon_mac, adv)) {
            std::println("[wake] Found Nintendo controller: {}", joycon_mac);
            return true;
        }
        std::println("[wake] No Nintendo advert yet. Keep holding SYNC or press it again; retrying...");
    }
    return false;
}

std::string first_hci_from_sysfs() {
    std::error_code ec;
    if (fs::exists("/sys/class/bluetooth", ec)) {
        for (const auto& entry : fs::directory_iterator("/sys/class/bluetooth", ec)) {
            std::string name = entry.path().filename().string();
            if (valid_hci(name)) return name;
        }
    }
    return "";
}

std::string detect_wake_hci_for_setup() {
    std::string hci = first_hci_from_sysfs();
    if (valid_hci(hci)) return hci;
    WakeCmdResult r = run_wake_command({"btmgmt", "info"}, false, true);
    if (r.exit_code == 0) {
        std::istringstream iss(r.output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.starts_with("hci")) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string cand = line.substr(0, colon);
                    if (valid_hci(cand)) return cand;
                }
            }
        }
    }
    return "hci0";
}

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
    if (!valid_mac(orig_mac)) orig_mac = read_hci_address(g_ctx.switch2_wake_hci_dev);
    g_switch2_wake_original_bt_mac = orig_mac;

    {
        std::string setup_hci = g_ctx.switch2_wake_hci_dev;
        reset_wake_bt_stack(setup_hci, g_ctx.verbose);
        g_ctx.switch2_wake_hci_dev = setup_hci;
    }
    auto restore_setup_bt_state = [&] { restore_bluetooth_controller_state(g_ctx.switch2_wake_hci_dev, true); };

    std::println("NS-PC-Control Switch 2 Joy-Con 2 wake setup");
    std::println("[wake] Config will be saved to: {}", g_ctx.switch2_wakeup_config_path);
    std::println("[wake] Bluetooth adapter registered for runtime wake: {}", g_ctx.switch2_wake_hci_dev);

    std::string mac;
    if (!auto_find_joycon_for_setup(mac)) { restore_setup_bt_state(); return 1; }

    std::println("\n[wake] Step 2/4: Capture the Joy-Con 2 HOME wake advertisement.");
    std::string cap_mac, cap_adv;
    bool captured = false;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        std::println("[wake] HOME capture attempt {}/5.", attempt);
        wait_for_enter("[wake] Press Enter, then press HOME on the Joy-Con 2 immediately... ");
        if (capture_switch2_wake_advert(20, mac, cap_mac, cap_adv)) { captured = true; break; }
    }
    if (!captured) { restore_setup_bt_state(); return 1; }

    mac = cap_mac;
    std::println("[wake] Captured wake MAC: {}\n[wake] Captured wake ADV: {}", mac, cap_adv);
    wait_for_enter("[wake] Press Enter when the Joy-Con 2 is paired back AND the Switch 2 is asleep; setup will test wake... ");

    if (!save_switch2_wakeup_config(mac, cap_adv, g_ctx.switch2_wake_hci_dev, orig_mac)) { restore_setup_bt_state(); return 1; }

    g_ctx.switch2_wake_mac = to_lower(mac);
    g_ctx.switch2_wake_adv_hex = to_upper_no_space(cap_adv);
    g_ctx.switch2_wake_config_loaded = true;

    std::println("[wake] Step 4/4: Sending test wake advert with MAC spoofing...");
    if (!send_switch2_wake_advert_once(g_ctx.switch2_wake_mac, g_ctx.switch2_wake_adv_hex, 1, false, true)) { restore_setup_bt_state(); return 1; }
    restore_setup_bt_state();
    std::println("[wake] Test wake advert sent. If the Switch 2 woke up, setup is complete.");
    return 0;
}

void enter_switch2_wake_runtime_mode() {
    if (!g_ctx.switch2_wake_adv_enabled || !g_ctx.switch2_wake_config_loaded) return;
    run_wake_command({"systemctl", "stop", "bluetooth"}, false);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
    run_wake_command({"hciconfig", g_ctx.switch2_wake_hci_dev, "up"}, false);
    std::thread([] {
        std::string hci = g_ctx.switch2_wake_hci_dev;
        prepare_wake_controller(hci, g_ctx.switch2_wake_mac, g_ctx.verbose);
    }).detach();
}

void wait_for_bluetooth_runtime_ready(bool verbose) {
    if (g_ctx.bluetooth_disabled) return;
    (void)verbose;
    for (int i = 0; i < 20 && g_ctx.running.load(); ++i) {
        WakeCmdResult show = run_wake_command({"bluetoothctl", "show"}, false, true);
        if (show.exit_code == 0 && show.output.find("Powered: yes") != std::string::npos) return;
        run_wake_command({"rfkill", "unblock", "bluetooth"}, false);
        run_wake_command({"bluetoothctl", "power", "on"}, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void enter_bluetooth_runtime_mode() {
    if (g_ctx.bluetooth_disabled) return;
    if (!g_ctx.switch2_wake_config_loaded) load_switch2_wakeup_config(true);
    restore_bluetooth_controller_state(g_ctx.switch2_wake_hci_dev, true);
    wait_for_bluetooth_runtime_ready(g_ctx.verbose);
}

bool wake_bt_state_was_modified() { return g_bt_modified_for_wake.load(); }

void restore_wake_bt_state(bool restart_bluez) {
    if (g_ctx.bluetooth_disabled) return;
    for (int i = 0; i < 10 && g_ctx.switch2_wake_adv_running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::string hci = !g_saved_bt_hci.empty() ? g_saved_bt_hci : g_ctx.switch2_wake_hci_dev;
    if (!valid_hci(hci)) hci = "hci0";
    wake_disable_advertising_quiet(hci);
    if (!wake_bt_state_was_modified()) return;
    if (g_bt_modified_for_wake.load() && !g_saved_bt_mac.empty()) g_switch2_wake_original_bt_mac = g_saved_bt_mac;
    restore_bluetooth_controller_state(hci, restart_bluez);
    g_saved_bt_mac.clear();
    g_saved_bt_hci.clear();
    g_bt_modified_for_wake = false;
}

void drain_hid_output_queue(int fd) {
    if (fd < 0) return;
    uint8_t discard[PRO_REPORT_SIZE];
    for (int i = 0; i < 32; ++i) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
        if (read(fd, discard, sizeof(discard)) <= 0) break;
    }
}

bool check_and_enable_gadget_host() {
    std::string config_path = "/boot/firmware/config.txt";
    std::string cmdline_path = "/boot/firmware/cmdline.txt";
    if (!fs::exists(config_path)) {
        config_path = "/boot/config.txt";
    }
    if (!fs::exists(cmdline_path)) {
        cmdline_path = "/boot/cmdline.txt";
    }

    if (!fs::exists(config_path) || !fs::exists(cmdline_path)) {
        return false;
    }

    bool has_dwc2 = false;
    bool has_otg_active = false;
    std::vector<std::string> config_lines;
    {
        std::ifstream f(config_path);
        std::string line;
        while (std::getline(f, line)) {
            config_lines.push_back(line);
            size_t hash_pos = line.find("#");
            size_t dwc_pos = line.find("dtoverlay=dwc2");
            if (dwc_pos != std::string::npos && (hash_pos == std::string::npos || hash_pos > dwc_pos)) {
                has_dwc2 = true;
            }
            size_t otg_pos = line.find("otg_mode=1");
            if (otg_pos != std::string::npos && (hash_pos == std::string::npos || hash_pos > otg_pos)) {
                has_otg_active = true;
            }
        }
    }

    bool has_modules = false;
    std::string cmdline_content;
    {
        std::ifstream f(cmdline_path);
        if (std::getline(f, cmdline_content)) {
            if (cmdline_content.find("modules-load=dwc2,libcomposite") != std::string::npos) {
                has_modules = true;
            }
        }
    }

    if (has_dwc2 && !has_otg_active && has_modules) {
        return false;
    }

    std::println(stderr, "[gadget] USB gadget host configurations are missing or conflicting in boot settings.");
    std::println(stderr, "[gadget] Required changes:");
    if (has_otg_active) {
        std::println(stderr, "[gadget]   - Comment out 'otg_mode=1' in {} (conflicts with device/gadget mode)", config_path);
    }
    if (!has_dwc2) {
        std::println(stderr, "[gadget]   - Add 'dtoverlay=dwc2' to {}", config_path);
    }
    if (!has_modules) {
        std::println(stderr, "[gadget]   - Add 'modules-load=dwc2,libcomposite' to {}", cmdline_path);
    }

    if (isatty(STDIN_FILENO)) {
        std::print(stderr, "USB gadget mode not enabled. Enable and reboot? (y/N): ");
        std::fflush(stderr);
        std::string ans;
        if (std::getline(std::cin, ans)) {
            if (ans == "y" || ans == "Y") {
                int dummy;
                if (has_otg_active) {
                    std::string cmd = "sudo sed -i 's/^otg_mode=1/#otg_mode=1/g' " + config_path;
                    dummy = std::system(cmd.c_str());
                }
                if (!has_dwc2) {
                    std::string cmd = "echo \"dtoverlay=dwc2\" | sudo tee -a " + config_path + " >/dev/null";
                    dummy = std::system(cmd.c_str());
                }
                if (!has_modules) {
                    std::string cmd = "sudo sed -i 's/rootwait/rootwait modules-load=dwc2,libcomposite/' " + cmdline_path;
                    dummy = std::system(cmd.c_str());
                }
                (void)dummy;

                std::println(stderr, "[gadget] Rebooting system in 3 seconds to apply boot configurations...");
                std::this_thread::sleep_for(std::chrono::seconds(3));
                int rb = std::system("sudo reboot"); (void)rb;
                return true;
            }
        }
    } else {
        std::println(stderr, "[gadget] Run interactively or configure manually.");
    }
    return false;
}

bool setup_gadget_builtin(bool force, const char* reason) {
    if (!force && hidg_nodes_ready()) return true;
    if (!force && g_ctx.gadget_setup_attempted.exchange(true)) return hidg_nodes_ready();
    if (force) g_ctx.gadget_setup_attempted.store(true);
    if (geteuid() != 0) {
        std::println(stderr, "[gadget] requested /dev/hidg* nodes are not ready and built-in setup needs root.\n[gadget] Run: sudo ./ns-backend ...");
        return false;
    }
    if (g_ctx.verbose) {
        std::println("[gadget] {}; creating built-in {}-interface {} gadget", reason ? reason : "HID gadget not ready", HID_PORT_COUNT, g_ctx.legacy_mode ? "legacy 8-byte" : "64-byte motion");
    }
    int dummy1 = std::system("modprobe libcomposite >/dev/null 2>&1 || true"); (void)dummy1;
    int dummy2 = std::system("mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config >/dev/null 2>&1 || true"); (void)dummy2;
    if (!fs::is_directory("/sys/kernel/config/usb_gadget")) {
        std::println(stderr, "[gadget] /sys/kernel/config/usb_gadget is unavailable.");
        return false;
    }
    if (fs::exists(GADGET_DIR)) {
        if (force || !hidg_nodes_ready()) { teardown_gadget(); std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
        else return true;
    }
    if (!mkdirs(GADGET_DIR)) return false;
    fs::path gd = GADGET_DIR, cd = CONFIG_DIR;
    if (!mkdirs(gd / "strings/0x409") || !mkdirs(g_ctx.legacy_mode ? cd / "strings/0x409" : cd) || !mkdirs(gd / "functions")) return false;

    bool ok = write_file(gd / "bcdDevice", g_ctx.legacy_mode ? "0x0200" : "0x0210") &&
              write_file(gd / "bcdUSB", "0x0200") &&
              write_file(gd / "idVendor", g_ctx.legacy_mode ? "0x0F0D" : "0x057e") &&
              write_file(gd / "idProduct", g_ctx.legacy_mode ? "0x0092" : "0x2009") &&
              write_file(gd / "bDeviceClass", g_ctx.legacy_mode ? "0xFF" : "0x00") &&
              write_file(gd / "bDeviceSubClass", g_ctx.legacy_mode ? "0xFF" : "0x00") &&
              write_file(gd / "bDeviceProtocol", g_ctx.legacy_mode ? "0xFF" : "0x00") &&
              write_file(gd / "strings/0x409/serialnumber", g_ctx.legacy_mode ? "000000000001" : g_ctx.usb_serial) &&
              write_file(gd / "strings/0x409/manufacturer", "NS Bridge") &&
              write_file(gd / "strings/0x409/product", g_ctx.legacy_mode ? "Legacy USB Gamepad" : "Motion USB Gamepad") &&
              write_file(cd / "MaxPower", "500") &&
              write_file(cd / "bmAttributes", g_ctx.legacy_mode ? "0x80" : "0xA0");
    if (g_ctx.legacy_mode) ok = ok && write_file(cd / "strings/0x409/configuration", "USB 4-Player Hub Config");
    if (!ok) return false;

    for (int i = 0; i < HID_PORT_COUNT; ++i) { if (!create_hid_function(i)) return false; }
    std::string UDC = first_udc_name();
    if (UDC.empty()) {
        if (check_and_enable_gadget_host()) {
            std::exit(0);
        }
        std::println(stderr, "[gadget] No UDC found. Check dtoverlay=dwc2 in /boot/config.txt.");
        return false;
    }
    if (!write_file(gd / "UDC", UDC)) return false;
    if (g_ctx.verbose) std::println("[gadget] Bound to UDC: {}", UDC);

    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            char path[32]; std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
            if (access(path, F_OK) != 0) all_seen = false;
            chmod(path, 0666);
        }
        if (all_seen && hidg_nodes_ready()) {
            std::println("[gadget] Done. Exposed {} USB gamepad HID interface(s) (/dev/hidg0..{})", HID_PORT_COUNT, HID_PORT_COUNT - 1);
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
    if (!fs::exists(GADGET_DIR, ec)) return;
    std::println("[gadget] Closing USB gadget...");
    write_file(fs::path(GADGET_DIR) / "UDC", "");
    for (int i = 0; i < 4; ++i) {
        fs::remove(fs::path(CONFIG_DIR) / ("hid.usb" + std::to_string(i)), ec);
        fs::remove(fs::path(GADGET_DIR) / "functions" / ("hid.usb" + std::to_string(i)), ec);
    }
    fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409", ec);
    fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1", ec);
    fs::remove("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409", ec);
    fs::remove(GADGET_DIR, ec);
    std::println("[gadget] USB gadget closed");
}

bool run_gadget_setup_if_needed(bool force, const char* reason) { return setup_gadget_builtin(force, reason); }

bool run_revert_gadget_host() {
    std::string config_path = "/boot/firmware/config.txt";
    std::string cmdline_path = "/boot/firmware/cmdline.txt";
    if (!fs::exists(config_path)) {
        config_path = "/boot/config.txt";
    }
    if (!fs::exists(cmdline_path)) {
        cmdline_path = "/boot/cmdline.txt";
    }

    if (!fs::exists(config_path) || !fs::exists(cmdline_path)) {
        std::println(stderr, "[gadget] Boot configuration files not found. Revert skipped.");
        return false;
    }

    bool needs_revert = false;
    {
        std::ifstream f(config_path);
        std::string line;
        while (std::getline(f, line)) {
            size_t hash_pos = line.find("#");
            size_t dwc_pos = line.find("dtoverlay=dwc2");
            if (dwc_pos != std::string::npos && (hash_pos == std::string::npos || hash_pos > dwc_pos)) {
                needs_revert = true;
                break;
            }
            size_t otg_pos = line.find("otg_mode=1");
            if (otg_pos != std::string::npos && hash_pos != std::string::npos && hash_pos < otg_pos) {
                needs_revert = true;
                break;
            }
        }
    }

    if (!needs_revert) {
        std::ifstream f(cmdline_path);
        std::string content;
        if (std::getline(f, content)) {
            if (content.find("modules-load=dwc2,libcomposite") != std::string::npos) {
                needs_revert = true;
            }
        }
    }

    if (!needs_revert) {
        std::println(stderr, "[gadget] Boot configurations are already in their default/reverted state. No action taken.");
        return true;
    }

    std::println(stderr, "[gadget] Reverting USB gadget host configurations...");
    int dummy;
    // 1. Remove "dtoverlay=dwc2" from config.txt
    std::string cmd1 = "sudo sed -i '/dtoverlay=dwc2/d' " + config_path;
    dummy = std::system(cmd1.c_str());

    // 2. Uncomment otg_mode=1 if it was commented out by the app
    std::string cmd_otg = "sudo sed -i 's/#otg_mode=1/otg_mode=1/g' " + config_path;
    dummy = std::system(cmd_otg.c_str());

    // 3. Remove the loaded modules from cmdline.txt
    std::string cmd2 = "sudo sed -i 's/modules-load=dwc2,libcomposite//g' " + cmdline_path;
    dummy = std::system(cmd2.c_str());
    std::string cmd3 = "sudo sed -i 's/  / /g' " + cmdline_path;
    dummy = std::system(cmd3.c_str());
    (void)dummy;

    std::println(stderr, "[gadget] Rebooting system in 3 seconds to apply reverted boot configurations...");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    int rb = std::system("sudo reboot"); (void)rb;
    return true;
}

