#include "bluetooth_manager.hpp"
#include "app_state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <ranges>
#include <format>
#include <string_view>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_manager_running{false};
std::atomic<bool> g_proactive_reconnect_enabled{true};
std::thread g_manager_thread;
constexpr const char* BT_RECONNECT_PAUSE_FILE = "/tmp/ns-pc-control-bt-reconnect-paused";

std::string lower_copy(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string path_to_mac(const std::string& path) {
    const std::string marker = "/dev_";
    size_t p = path.rfind(marker);
    if (p == std::string::npos) return {};
    std::string mac = path.substr(p + marker.size());
    std::ranges::replace(mac, '_', ':');
    std::ranges::transform(mac, mac.begin(), [](unsigned char c) { return std::toupper(c); });
    return mac;
}

bool command_exists(const char* name) {
    std::string cmd = std::format("command -v {} >/dev/null 2>&1", name);
    const int rc = std::system(cmd.c_str());
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

bool proc_modules_contains(const char* module) {
    std::ifstream in("/proc/modules");
    std::string line;
    const std::string prefix = std::format("{} ", module);
    while (std::getline(in, line)) {
        if (line.starts_with(prefix)) return true;
    }
    return false;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

int run_cmd(const std::string& cmd, bool verbose = false) {
    if (verbose) std::println("[bt] setup: {}", cmd);
    const std::string wrapped = std::format("if command -v chrt >/dev/null 2>&1; then chrt -o 0 /bin/sh -c {}; else /bin/sh -c {}; fi", shell_quote(cmd), shell_quote(cmd));
    const int rc = std::system(wrapped.c_str());
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 126;
}

std::string trim_copy(std::string_view s) {
    auto start = std::ranges::find_if_not(s, [](unsigned char c) { return std::isspace(c); });
    auto end = std::ranges::find_if_not(s | std::views::reverse, [](unsigned char c) { return std::isspace(c); }).base();
    return start < end ? std::string(start, end) : "";
}

bool ensure_ini_key(std::vector<std::string>& lines, std::string_view section, std::string_view key, std::string_view value) {
    std::string sec_header = std::format("[{}]", section);
    std::string wanted = std::format("{} = {}", key, value);
    auto sec_it = std::ranges::find_if(lines, [&](const auto& l) { return trim_copy(l) == sec_header; });
    if (sec_it == lines.end()) {
        lines.push_back("");
        lines.push_back(sec_header);
        lines.push_back(wanted);
        return true;
    }
    auto end_it = std::find_if(sec_it + 1, lines.end(), [](const auto& l) {
        auto t = trim_copy(l);
        return t.starts_with('[') && t.ends_with(']');
    });
    for (auto it = sec_it + 1; it != end_it; ++it) {
        std::string t = trim_copy(*it);
        if (t.starts_with('#')) t = trim_copy(t.substr(1));
        if (t.starts_with(key) && (t.size() == key.size() || std::isspace(t[key.size()]) || t[key.size()] == '=')) {
            if (trim_copy(*it) == wanted) return false;
            *it = wanted;
            return true;
        }
    }
    lines.insert(end_it, wanted);
    return true;
}

void configure_bluez_reconnect_policy(bool verbose) {
    if (geteuid() != 0) return;
    (void)run_cmd("mkdir -p /etc/bluetooth >/dev/null 2>&1", false);
    const char* conf = "/etc/bluetooth/main.conf";
    std::vector<std::string> lines;
    {
        std::ifstream in(conf);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    if (lines.empty()) {
        lines = {"[General]", "", "[Policy]"};
    }

    bool changed = false;
    changed |= ensure_ini_key(lines, "General", "FastConnectable", "true");
    changed |= ensure_ini_key(lines, "BR", "PageScanType", "1");
    changed |= ensure_ini_key(lines, "BR", "PageScanInterval", "128");
    changed |= ensure_ini_key(lines, "BR", "PageScanWindow", "48");
    changed |= ensure_ini_key(lines, "Policy", "ReconnectUUIDs", "00001124-0000-1000-8000-00805f9b34fb,00001812-0000-1000-8000-00805f9b34fb");
    changed |= ensure_ini_key(lines, "Policy", "ReconnectAttempts", "15");
    changed |= ensure_ini_key(lines, "Policy", "ReconnectIntervals", "1,1,1,2,2,2,4,4,8,8,16,16,32");
    changed |= ensure_ini_key(lines, "Policy", "AutoEnable", "true");

    if (changed) {
        std::ofstream out(conf, std::ios::trunc);
        if (!out) {
            if (verbose) std::println(stderr, "[bt] warning: could not update {}", conf);
        } else {
            for (const auto& line : lines) out << line << '\n';
            if (verbose) std::println("[bt] configured BlueZ fast reconnect policy");
            if (verbose) std::println("[bt] restarting bluetooth service to apply changes...");
            if (command_exists("systemctl")) {
                (void)run_cmd("systemctl restart bluetooth.service >/dev/null 2>&1", verbose);
            } else if (command_exists("service")) {
                (void)run_cmd("service bluetooth restart >/dev/null 2>&1", verbose);
            }
        }
    }

    if (command_exists("btmgmt")) {
        (void)run_cmd("btmgmt power on >/dev/null 2>&1", verbose);
        (void)run_cmd("btmgmt connectable on >/dev/null 2>&1", verbose);
        (void)run_cmd("btmgmt bondable on >/dev/null 2>&1", verbose);
        (void)run_cmd("btmgmt ssp on >/dev/null 2>&1", verbose);
        (void)run_cmd("btmgmt fast-conn on >/dev/null 2>&1", verbose);
    }
}

void runtime_setup_impl(bool verbose) {
    if (geteuid() != 0) {
        if (verbose) std::println(stderr, "[bt] setup skipped; run as root for rfkill/uhid/bluetooth.service prep");
        return;
    }
    if (command_exists("rfkill")) (void)run_cmd("rfkill unblock bluetooth >/dev/null 2>&1", verbose);
    if (!proc_modules_contains("uhid")) {
        if (run_cmd("modprobe uhid >/dev/null 2>&1", verbose) != 0) {
            std::println(stderr, "[bt] warning: uhid module not loaded; some BLE controllers may not expose input devices");
        }
    }
    if (command_exists("systemctl")) {
        (void)run_cmd("systemctl start bluetooth.service >/dev/null 2>&1", verbose);
    } else if (command_exists("service")) {
        (void)run_cmd("service bluetooth start >/dev/null 2>&1", verbose);
    }
    configure_bluez_reconnect_policy(verbose);
}

} // namespace

#ifdef NS_ENABLE_BLUEZ_DBUS

#include <sdbus-c++/sdbus-c++.h>

namespace {

constexpr const char* BLUEZ_SERVICE = "org.bluez";
constexpr const char* OBJECT_MANAGER_IFACE = "org.freedesktop.DBus.ObjectManager";
constexpr const char* ADAPTER_IFACE = "org.bluez.Adapter1";
constexpr const char* DEVICE_IFACE = "org.bluez.Device1";
constexpr const char* AGENT_MANAGER_IFACE = "org.bluez.AgentManager1";
constexpr const char* AGENT_IFACE = "org.bluez.Agent1";
constexpr const char* AGENT_PATH = "/com/ns_pc_control/agent";
constexpr const char* HID_UUID = "00001124-0000-1000-8000-00805f9b34fb";
constexpr const char* HOGP_UUID = "00001812-0000-1000-8000-00805f9b34fb";

constexpr auto DBUS_FAST_TIMEOUT = std::chrono::milliseconds(600);
constexpr auto DBUS_DISCOVERY_TIMEOUT = std::chrono::milliseconds(1200);
constexpr auto DBUS_CONNECT_TIMEOUT = std::chrono::milliseconds(2500);
constexpr auto DBUS_PAIR_TIMEOUT = std::chrono::seconds(10);

using ManagedObjects = std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>>;
using PropertyMap = std::map<std::string, sdbus::Variant>;

struct DeviceInfo {
    std::string path;
    std::string address;
    std::string name;
    std::string alias;
    std::string icon;
    uint32_t klass = 0;
    uint16_t appearance = 0;
    bool has_hid_uuid = false;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    bool blocked = false;

    std::string display_name() const {
        return !alias.empty() ? alias : (!name.empty() ? name : (!address.empty() ? address : path));
    }

    bool is_controller_like() const {
        if (has_hid_uuid || (appearance >= 960 && appearance <= 968) || (klass & 0x1f00u) == 0x0500u) return true;
        std::string s = lower_copy(display_name() + " " + icon);
        static constexpr std::array keywords = {"gamepad", "joystick", "controller", "xbox", "dualsense", "dualshock", "playstation", "8bitdo", "gulikit", "gamesir", "joy-con"};
        return std::ranges::any_of(keywords, [&](const char* kw) { return s.find(kw) != std::string::npos; });
    }

    bool looks_fresh_from_discovery() const {
        return !name.empty() || !alias.empty() || !icon.empty() || has_hid_uuid || klass != 0 || appearance != 0;
    }
};

template <typename T>
T get_prop(const PropertyMap& props, const char* key, T fallback = T{}) {
    auto it = props.find(key);
    if (it != props.end()) {
        try { return it->second.get<T>(); } catch (...) {}
    }
    return fallback;
}

class BluezManager {
public:
    BluezManager() = default;
    void run();
    static void disconnect_gamepads();
    void run_pairing_wizard();

private:
    bool pair_window_open = false;
    bool discovery_active = false;
    bool logged_ready = false;
    bool first_snapshot_done = false;
    std::atomic<bool> dbus_activity{false};
    Clock::time_point pair_window_end{};
    std::string adapter_path;
    std::set<std::string> logged_connected;
    std::set<std::string> pairing_attempted;

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> agent_object;

    bool connect_bus();
    void close_bus();
    std::unique_ptr<sdbus::IProxy> proxy(const std::string& path) {
        return sdbus::createProxy(*connection, sdbus::ServiceName{BLUEZ_SERVICE}, sdbus::ObjectPath{path});
    }
    bool register_agent();
    bool ensure_adapter();
    bool adapter_set_bool(const char* prop, bool value);
    bool start_discovery();
    bool stop_discovery();
    ManagedObjects managed_objects();
    std::vector<DeviceInfo> list_devices();
    bool set_trusted(const DeviceInfo& dev, bool trusted);
    bool call_device_method(const std::string& path, const std::string& method, std::chrono::milliseconds timeout, std::string_view success_err = "");
    bool pair_device(const DeviceInfo& dev);
    bool connect_device_once(const DeviceInfo& dev);
    bool disconnect_device(const DeviceInfo& dev);
    void open_pair_window(const char* reason);
    void close_pair_window();
    void tick();
    void note_connected(const DeviceInfo& dev);
};

bool BluezManager::connect_bus() {
    try {
        connection = sdbus::createSystemBusConnection();
        connection->setMethodCallTimeout(DBUS_FAST_TIMEOUT);
        connection->enterEventLoopAsync();
        return true;
    } catch (const std::exception& e) {
        std::println(stderr, "[bt] BlueZ D-Bus unavailable: {}", e.what());
    }
    close_bus();
    return false;
}

void BluezManager::close_bus() {
    agent_object.reset();
    if (connection) {
        try { connection->leaveEventLoop(); } catch (...) {}
        connection.reset();
    }
}

bool BluezManager::register_agent() {
    if (!connection) return false;
    try {
        agent_object = sdbus::createObject(*connection, sdbus::ObjectPath{AGENT_PATH});
        agent_object->addVTable(
            sdbus::registerMethod("Release").implementedAs([] {}),
            sdbus::registerMethod("RequestPinCode").implementedAs([](const sdbus::ObjectPath&) { return std::string{"0000"}; }),
            sdbus::registerMethod("RequestPasskey").implementedAs([](const sdbus::ObjectPath&) { return uint32_t{0}; }),
            sdbus::registerMethod("DisplayPinCode").implementedAs([](const sdbus::ObjectPath&, const std::string&) {}),
            sdbus::registerMethod("DisplayPasskey").implementedAs([](const sdbus::ObjectPath&, uint32_t, uint16_t) {}),
            sdbus::registerMethod("RequestConfirmation").implementedAs([](const sdbus::ObjectPath&, uint32_t) {}),
            sdbus::registerMethod("RequestAuthorization").implementedAs([](const sdbus::ObjectPath&) {}),
            sdbus::registerMethod("AuthorizeService").implementedAs([](const sdbus::ObjectPath&, const std::string&) {}),
            sdbus::registerMethod("Cancel").implementedAs([] {})
        ).forInterface(sdbus::InterfaceName{AGENT_IFACE});

        auto mgr = proxy("/org/bluez");
        try {
            mgr->callMethod("RegisterAgent").onInterface(AGENT_MANAGER_IFACE).withTimeout(DBUS_FAST_TIMEOUT).withArguments(sdbus::ObjectPath{AGENT_PATH}, std::string{"NoInputNoOutput"}).storeResultsTo();
        } catch (const sdbus::Error& e) {
            if (std::string(e.getName()) != "org.bluez.Error.AlreadyExists") throw;
        }
        mgr->callMethod("RequestDefaultAgent").onInterface(AGENT_MANAGER_IFACE).withTimeout(DBUS_FAST_TIMEOUT).withArguments(sdbus::ObjectPath{AGENT_PATH}).storeResultsTo();
        return true;
    } catch (const std::exception& e) {
        std::println(stderr, "[bt] pairing agent unavailable: {}", e.what());
    }
    agent_object.reset();
    return false;
}

ManagedObjects BluezManager::managed_objects() {
    ManagedObjects objects;
    if (!connection) return objects;
    try {
        proxy("/")->callMethod("GetManagedObjects").onInterface(OBJECT_MANAGER_IFACE).withTimeout(DBUS_FAST_TIMEOUT).storeResultsTo(objects);
    } catch (const sdbus::Error& e) {
        if (g_ctx.verbose) std::println(stderr, "[bt] GetManagedObjects failed: {}", e.getMessage());
    }
    return objects;
}

bool BluezManager::ensure_adapter() {
    auto objs = managed_objects();
    auto it = std::ranges::find_if(objs, [](const auto& pair) { return pair.second.contains(ADAPTER_IFACE); });
    if (it == objs.end()) {
        std::println(stderr, "[bt] no BlueZ adapter found. Is bluetooth.service running and hci0 present?");
        return false;
    }
    adapter_path = it->first;
    (void)adapter_set_bool("Powered", true);
    if (geteuid() == 0 && command_exists("btmgmt")) {
        (void)run_cmd("btmgmt connectable on >/dev/null 2>&1", g_ctx.verbose);
        (void)run_cmd("btmgmt fast-conn on >/dev/null 2>&1", g_ctx.verbose);
    }
    return true;
}

bool BluezManager::adapter_set_bool(const char* prop, bool value) {
    if (!connection || adapter_path.empty()) return false;
    try {
        proxy(adapter_path)->setProperty(prop).onInterface(ADAPTER_IFACE).toValue(value);
        return true;
    } catch (const sdbus::Error& e) {
        if (g_ctx.verbose) std::println(stderr, "[bt] adapter {}={} failed: {}", prop, value, e.getMessage());
        return false;
    }
}

bool BluezManager::start_discovery() {
    if (!connection || adapter_path.empty() || discovery_active) return true;
    try {
        proxy(adapter_path)->callMethod("StartDiscovery").onInterface(ADAPTER_IFACE).withTimeout(DBUS_DISCOVERY_TIMEOUT).storeResultsTo();
        discovery_active = true;
        return true;
    } catch (const sdbus::Error& e) {
        if (std::string(e.getName()) == "org.bluez.Error.InProgress") {
            discovery_active = true;
            return true;
        }
        if (g_ctx.verbose) std::println(stderr, "[bt] StartDiscovery failed: {}", e.getMessage());
        return false;
    }
}

bool BluezManager::stop_discovery() {
    if (!connection || adapter_path.empty() || !discovery_active) return true;
    discovery_active = false;
    try {
        proxy(adapter_path)->callMethod("StopDiscovery").onInterface(ADAPTER_IFACE).withTimeout(DBUS_DISCOVERY_TIMEOUT).storeResultsTo();
        return true;
    } catch (const sdbus::Error& e) {
        const std::string name = e.getName();
        if (name == "org.bluez.Error.Failed" || name == "org.bluez.Error.NotReady") return true;
        if (g_ctx.verbose) std::println(stderr, "[bt] StopDiscovery failed: {}", e.getMessage());
        return false;
    }
}

std::vector<DeviceInfo> BluezManager::list_devices() {
    std::vector<DeviceInfo> out;
    for (const auto& [obj_path, ifaces] : managed_objects()) {
        if (!ifaces.contains(DEVICE_IFACE)) continue;
        const auto& props = ifaces.at(DEVICE_IFACE);
        DeviceInfo dev{
            .path = obj_path,
            .address = get_prop<std::string>(props, "Address"),
            .name = get_prop<std::string>(props, "Name"),
            .alias = get_prop<std::string>(props, "Alias"),
            .icon = get_prop<std::string>(props, "Icon"),
            .klass = get_prop<uint32_t>(props, "Class"),
            .appearance = get_prop<uint16_t>(props, "Appearance"),
            .paired = get_prop<bool>(props, "Paired"),
            .trusted = get_prop<bool>(props, "Trusted"),
            .connected = get_prop<bool>(props, "Connected"),
            .blocked = get_prop<bool>(props, "Blocked")
        };
        for (const auto& uuid : get_prop<std::vector<std::string>>(props, "UUIDs")) {
            std::string u = lower_copy(uuid);
            if (u == HID_UUID || u == HOGP_UUID) dev.has_hid_uuid = true;
        }
        if (dev.address.empty()) dev.address = path_to_mac(dev.path);
        out.push_back(std::move(dev));
    }
    return out;
}

bool BluezManager::set_trusted(const DeviceInfo& dev, bool trusted) {
    if (!connection || dev.path.empty() || dev.trusted == trusted) return true;
    try {
        proxy(dev.path)->setProperty("Trusted").onInterface(DEVICE_IFACE).toValue(trusted);
        return true;
    } catch (const sdbus::Error& e) {
        if (g_ctx.verbose) std::println(stderr, "[bt] failed to trust {}: {}", dev.display_name(), e.getMessage());
        return false;
    }
}

bool BluezManager::call_device_method(const std::string& path, const std::string& method, std::chrono::milliseconds timeout, std::string_view success_err) {
    try {
        proxy(path)->callMethod(method).onInterface(DEVICE_IFACE).withTimeout(timeout).storeResultsTo();
        return true;
    } catch (const sdbus::Error& e) {
        if (!success_err.empty() && e.getName() == success_err) return true;
        if (g_ctx.verbose && e.getName() != "org.bluez.Error.NotReady") {
            std::println(stderr, "[bt] method {} failed on {}: {}", method, path_to_mac(path), e.getMessage());
        }
        return false;
    }
}

bool BluezManager::pair_device(const DeviceInfo& dev) {
    if (!connection || dev.path.empty()) return false;
    return call_device_method(dev.path, "Pair", DBUS_PAIR_TIMEOUT, "org.bluez.Error.AlreadyExists");
}

bool BluezManager::connect_device_once(const DeviceInfo& dev) {
    if (!connection || dev.path.empty() || dev.connected) return true;
    return call_device_method(dev.path, "Connect", DBUS_CONNECT_TIMEOUT, "org.bluez.Error.AlreadyConnected");
}

bool BluezManager::disconnect_device(const DeviceInfo& dev) {
    if (!connection || dev.path.empty() || !dev.connected) return true;
    return call_device_method(dev.path, "Disconnect", DBUS_CONNECT_TIMEOUT, "org.bluez.Error.NotConnected");
}

void BluezManager::open_pair_window(const char* reason) {
    pair_window_open = true;
    pair_window_end = Clock::now() + std::chrono::minutes(2);
    pairing_attempted.clear();
    (void)adapter_set_bool("Pairable", true);
    (void)adapter_set_bool("Discoverable", true);
    (void)start_discovery();
    std::println("[bt] pairing open for 2 minutes ({})", reason ? reason : "requested");
}

void BluezManager::close_pair_window() {
    if (!pair_window_open && !discovery_active) return;
    pair_window_open = false;
    (void)stop_discovery();
    (void)adapter_set_bool("Discoverable", false);
    (void)adapter_set_bool("Pairable", false);
    std::println("[bt] pairing closed; trusted controllers can still reconnect");
}

void BluezManager::note_connected(const DeviceInfo& dev) {
    if (!first_snapshot_done) {
        logged_connected.insert(dev.path);
        return;
    }
    if (logged_connected.insert(dev.path).second) {
        std::println("[bt] connected {} ({})", dev.display_name(), dev.address);
    }
}

void BluezManager::tick() {
    const auto now = Clock::now();
    if (!g_proactive_reconnect_enabled.load(std::memory_order_relaxed)) {
        close_pair_window();
        first_snapshot_done = true;
        return;
    }

    if (pair_window_open && now >= pair_window_end) {
        close_pair_window();
    }

    bool have_connected_controller = false;
    auto devs = list_devices() | std::views::filter([](const auto& d) { return !d.blocked && d.is_controller_like(); });
    for (const DeviceInfo& dev : devs) {
        if (dev.paired || dev.trusted) {
            if (!dev.trusted) (void)set_trusted(dev, true);
            if (dev.connected) {
                have_connected_controller = true;
                note_connected(dev);
            } else {
                logged_connected.erase(dev.path);
            }
            continue;
        }

        if (pair_window_open && dev.looks_fresh_from_discovery() && pairing_attempted.insert(dev.path).second) {
            (void)stop_discovery();
            std::println("[bt] pairing {} ({})", dev.display_name(), dev.address);
            if (pair_device(dev)) {
                DeviceInfo paired = dev;
                paired.paired = true;
                (void)set_trusted(paired, true);
                (void)connect_device_once(paired);
                std::println("[bt] paired {} ({})", dev.display_name(), dev.address);
            }
            if (pair_window_open) (void)start_discovery();
        }
    }

    if (!logged_ready) {
        std::println("[bt] Bluetooth controller manager ready");
        logged_ready = true;
    }
    (void)have_connected_controller;
    first_snapshot_done = true;
}

void BluezManager::run() {
    if (!connect_bus()) return;
    (void)register_agent();
    if (!ensure_adapter()) { close_bus(); return; }

    (void)adapter_set_bool("Pairable", false);
    (void)adapter_set_bool("Discoverable", false);

    while (g_manager_running.load(std::memory_order_relaxed)) {
        tick();
        const int wait_ms = pair_window_open || discovery_active ? 100 : 250;
        for (int waited = 0; waited < wait_ms && g_manager_running.load(std::memory_order_relaxed); waited += 25) {
            if (dbus_activity.exchange(false, std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    close_pair_window();
    close_bus();
}

void BluezManager::disconnect_gamepads() {
    BluezManager mgr;
    if (!mgr.connect_bus() || !mgr.ensure_adapter()) return;
    for (const auto& dev : mgr.list_devices() | std::views::filter([](const auto& d) { return d.connected && d.is_controller_like(); })) {
        std::println("[bt] disconnecting {} ({}) because Switch suspended", dev.display_name(), dev.address);
        (void)mgr.disconnect_device(dev);
    }
    mgr.close_bus();
}

void BluezManager::run_pairing_wizard() {
    if (!connect_bus()) return;
    (void)register_agent();
    if (!ensure_adapter()) { close_bus(); return; }

    (void)adapter_set_bool("Pairable", false);
    (void)adapter_set_bool("Discoverable", false);

    std::println("[bt] --pair specified; clearing all previously paired gamepads...");
    for (const auto& dev : list_devices() | std::views::filter([](const auto& d) { return d.paired || d.trusted; })) {
        try {
            std::println("[bt] removing paired device {} ({})", dev.display_name(), dev.address);
            proxy(adapter_path)->callMethod("RemoveDevice").onInterface(ADAPTER_IFACE).withTimeout(DBUS_FAST_TIMEOUT).withArguments(sdbus::ObjectPath{dev.path}).storeResultsTo();
        } catch (const sdbus::Error& e) {
            std::println(stderr, "[bt] failed to remove device {}: {}", dev.display_name(), e.getMessage());
        }
    }

    pair_window_open = true;
    pair_window_end = Clock::time_point::max();
    pairing_attempted.clear();
    (void)adapter_set_bool("Pairable", true);
    (void)adapter_set_bool("Discoverable", true);
    (void)start_discovery();

    std::println("\n==================================================");
    std::println("Bluetooth controller pairing wizard started.");
    std::println("Please put your controllers in pairing mode.");
    std::println("Press ENTER when done, or the wizard will exit after 4 pairings.");
    std::println("==================================================\n");

    std::atomic<bool> wizard_done{false};
    std::jthread stdin_thread([&wizard_done]() {
        std::string dummy;
        std::getline(std::cin, dummy);
        wizard_done.store(true, std::memory_order_relaxed);
    });

    int paired_count = 0;
    std::set<std::string> successfully_paired;

    while (!wizard_done.load(std::memory_order_relaxed) && paired_count < 4) {
        for (const DeviceInfo& dev : list_devices() | std::views::filter([](const auto& d) { return !d.blocked && d.is_controller_like(); })) {
            if (dev.paired || dev.trusted) {
                if (!dev.trusted) (void)set_trusted(dev, true);
                if (successfully_paired.insert(dev.path).second) {
                    paired_count++;
                    std::println("[bt] Successfully paired and trusted controller: {} ({}) [{}/4]", dev.display_name(), dev.address, paired_count);
                }
                continue;
            }

            if (dev.looks_fresh_from_discovery() && pairing_attempted.insert(dev.path).second) {
                (void)stop_discovery();
                std::println("[bt] pairing {} ({})", dev.display_name(), dev.address);
                if (pair_device(dev)) {
                    DeviceInfo paired = dev;
                    paired.paired = true;
                    (void)set_trusted(paired, true);
                    (void)connect_device_once(paired);
                    if (successfully_paired.insert(dev.path).second) {
                        paired_count++;
                        std::println("[bt] Successfully paired and trusted controller: {} ({}) [{}/4]", dev.display_name(), dev.address, paired_count);
                    }
                }
                (void)start_discovery();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::println("\nExiting pairing wizard...");
    close_pair_window();
    close_bus();
}

} // namespace

#else

namespace {
class BluezManager {
public:
    BluezManager() = default;
    void run() { std::println(stderr, "[bt] built without BlueZ D-Bus controller manager"); }
    static void disconnect_gamepads() {}
};
} // namespace

#endif

void bluetooth_manager_runtime_setup(bool verbose) {
    static std::atomic<bool> already_done{false};
    bool expected = false;
    if (!already_done.compare_exchange_strong(expected, true)) return;
    runtime_setup_impl(verbose);
}

void bluetooth_manager_start() {
    g_proactive_reconnect_enabled.store(true, std::memory_order_relaxed);
    (void)unlink(BT_RECONNECT_PAUSE_FILE);

    bool expected = false;
    if (!g_manager_running.compare_exchange_strong(expected, true)) return;
    if (g_manager_thread.joinable()) g_manager_thread.join();

    g_manager_thread = std::thread([] {
        BluezManager mgr;
        mgr.run();
        g_manager_running.store(false, std::memory_order_relaxed);
        if (g_ctx.verbose) std::println("[bt] Bluetooth controller manager stopped");
    });
}

void bluetooth_manager_set_proactive_reconnect_enabled(bool enabled) {
    const bool old = g_proactive_reconnect_enabled.exchange(enabled, std::memory_order_relaxed);
    if (enabled) {
        (void)unlink(BT_RECONNECT_PAUSE_FILE);
    } else {
        std::ofstream out(BT_RECONNECT_PAUSE_FILE);
        if (out) out << "1\n";
    }
    if (g_ctx.verbose && old != enabled) {
        std::println("[bt] Bluetooth reconnect {}", enabled ? "enabled" : "paused");
    }
}

void bluetooth_manager_stop() {
    g_manager_running.store(false, std::memory_order_relaxed);
    if (g_manager_thread.joinable()) g_manager_thread.join();
}

void bluetooth_manager_disconnect_connected_gamepads() {
    BluezManager::disconnect_gamepads();
}

bool run_bluetooth_pairing_wizard() {
#ifdef NS_ENABLE_BLUEZ_DBUS
    BluezManager mgr;
    mgr.run_pairing_wizard();
    return true;
#else
    std::println(stderr, "[bt] built without BlueZ D-Bus controller manager");
    return false;
#endif
}
