#include "bluetooth_manager.hpp"
#include "app_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <sstream>
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
std::atomic<bool> g_runtime_pair_window_requested{false};
std::thread g_manager_thread;
constexpr const char* BT_RECONNECT_PAUSE_FILE = "/tmp/ns-pc-control-bt-reconnect-paused";

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string path_to_mac(const std::string& path) {
    const std::string marker = "/dev_";
    const size_t p = path.rfind(marker);
    if (p == std::string::npos) return {};
    std::string mac = path.substr(p + marker.size());
    for (char& c : mac) {
        if (c == '_') c = ':';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return mac;
}

bool command_exists(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

bool proc_modules_contains(const char* module) {
    std::ifstream in("/proc/modules");
    std::string line;
    const std::string prefix = std::string(module) + " ";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) return true;
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
    // ns-backend can run under chrt -f 99. Keep service/setup commands on normal priority.
    const std::string wrapped = "if command -v chrt >/dev/null 2>&1; then chrt -o 0 /bin/sh -c " + shell_quote(cmd) +
                                "; else /bin/sh -c " + shell_quote(cmd) + "; fi";
    const int rc = std::system(wrapped.c_str());
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 126;
}

std::string trim_copy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool line_is_section(const std::string& line, const std::string& section) {
    const std::string t = trim_copy(line);
    return t == "[" + section + "]";
}

bool line_sets_key(const std::string& line, const std::string& key) {
    std::string t = trim_copy(line);
    if (!t.empty() && t[0] == '#') t = trim_copy(t.substr(1));
    if (t.rfind(key, 0) != 0) return false;
    if (t.size() == key.size()) return true;
    return std::isspace(static_cast<unsigned char>(t[key.size()])) || t[key.size()] == '=';
}

bool ensure_ini_key(std::vector<std::string>& lines, const std::string& section, const std::string& key, const std::string& value) {
    const std::string wanted = key + " = " + value;
    size_t sec = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (line_is_section(lines[i], section)) { sec = i; break; }
    }
    if (sec == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.push_back("[" + section + "]");
        lines.push_back(wanted);
        return true;
    }

    size_t end = lines.size();
    for (size_t i = sec + 1; i < lines.size(); ++i) {
        const std::string t = trim_copy(lines[i]);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') { end = i; break; }
    }

    for (size_t i = sec + 1; i < end; ++i) {
        if (!line_sets_key(lines[i], key)) continue;
        if (trim_copy(lines[i]) == wanted) return false;
        lines[i] = wanted;
        return true;
    }

    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(end), wanted);
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
    changed |= ensure_ini_key(lines, "Policy", "ReconnectUUIDs",
                              std::string("00001124-0000-1000-8000-00805f9b34fb") + "," + "00001812-0000-1000-8000-00805f9b34fb");
    changed |= ensure_ini_key(lines, "Policy", "ReconnectAttempts", "7");
    changed |= ensure_ini_key(lines, "Policy", "ReconnectIntervals", "1,1,2,4,8,16,32");
    changed |= ensure_ini_key(lines, "Policy", "AutoEnable", "true");

    if (changed) {
        std::ofstream out(conf, std::ios::trunc);
        if (!out) {
            if (verbose) std::println(stderr, "[bt] warning: could not update {}", conf);
        } else {
            for (const auto& line : lines) out << line << '\n';
            if (verbose) std::println("[bt] configured BlueZ fast reconnect policy");
        }
    }

    if (command_exists("btmgmt")) {
        // These are best-effort live adapter knobs. Some adapters/kernels do not support
        // fast-conn; keep it quiet unless verbose so normal users do not see scary noise.
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
    std::vector<std::string> uuids;
    uint32_t klass = 0;
    uint16_t appearance = 0;
    int16_t rssi = 0;
    bool has_rssi = false;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    bool blocked = false;

    std::string display_name() const {
        if (!alias.empty()) return alias;
        if (!name.empty()) return name;
        if (!address.empty()) return address;
        return path;
    }

    bool has_hid_uuid() const {
        for (const auto& uuid : uuids) {
            const std::string u = lower_copy(uuid);
            if (u == HID_UUID || u == HOGP_UUID) return true;
        }
        return false;
    }

    bool has_hid_appearance() const {
        return appearance >= 960 && appearance <= 968;
    }

    bool is_peripheral_class() const {
        return (klass & 0x1f00u) == 0x0500u;
    }

    bool is_controller_like() const {
        const std::string s = lower_copy(display_name() + " " + icon);
        return has_hid_uuid() || has_hid_appearance() || is_peripheral_class() ||
               s.find("gamepad") != std::string::npos ||
               s.find("joystick") != std::string::npos ||
               s.find("controller") != std::string::npos ||
               s.find("hid") != std::string::npos ||
               s.find("wireless controller") != std::string::npos ||
               s.find("xbox") != std::string::npos ||
               s.find("elite") != std::string::npos ||
               s.find("dualsense") != std::string::npos ||
               s.find("dualshock") != std::string::npos ||
               s.find("playstation") != std::string::npos ||
               s.find("8bitdo") != std::string::npos ||
               s.find("gulikit") != std::string::npos ||
               s.find("gamesir") != std::string::npos ||
               s.find("pro controller") != std::string::npos ||
               s.find("joy-con") != std::string::npos ||
               s.find("nintendo") != std::string::npos;
    }

    bool looks_fresh_from_discovery() const {
        return has_rssi || !name.empty() || !alias.empty() || !icon.empty() || !uuids.empty() || klass != 0 || appearance != 0;
    }
};

template <typename T>
bool read_prop(const PropertyMap& props, const char* key, T& out) {
    const auto it = props.find(key);
    if (it == props.end()) return false;
    try {
        out = it->second.get<T>();
        return true;
    } catch (const sdbus::Error&) {
        return false;
    } catch (...) {
        return false;
    }
}

class BluezManager {
public:
    explicit BluezManager(bool open_pair_window_on_start) : startup_pair_window(open_pair_window_on_start) {}
    void run();
    static void disconnect_gamepads();

private:
    bool startup_pair_window = false;
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
    std::unique_ptr<sdbus::IProxy> proxy(const std::string& path);
    bool register_agent();
    bool ensure_adapter();
    bool adapter_set_bool(const char* prop, bool value);
    bool start_discovery();
    bool stop_discovery();
    ManagedObjects managed_objects();
    std::vector<DeviceInfo> list_devices();
    bool set_trusted(const DeviceInfo& dev, bool trusted);
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
        // Keep the bus event loop alive for the BlueZ pairing agent. Device state is
        // intentionally polled at a low rate below instead of wiring fragile
        // version-specific signal registration APIs.
        connection->enterEventLoopAsync();
        return true;
    } catch (const sdbus::Error& e) {
        std::println(stderr, "[bt] BlueZ D-Bus unavailable: {}", e.getMessage());
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

std::unique_ptr<sdbus::IProxy> BluezManager::proxy(const std::string& path) {
    return sdbus::createProxy(*connection, sdbus::ServiceName{BLUEZ_SERVICE}, sdbus::ObjectPath{path});
}

bool BluezManager::register_agent() {
    if (!connection) return false;
    try {
        agent_object = sdbus::createObject(*connection, sdbus::ObjectPath{AGENT_PATH});
        agent_object->addVTable(
            sdbus::registerMethod("Release").implementedAs([] {}),
            sdbus::registerMethod("RequestPinCode").implementedAs(
                [](const sdbus::ObjectPath&) { return std::string{"0000"}; }),
            sdbus::registerMethod("RequestPasskey").implementedAs(
                [](const sdbus::ObjectPath&) { return uint32_t{0}; }),
            sdbus::registerMethod("DisplayPinCode").implementedAs(
                [](const sdbus::ObjectPath&, const std::string&) {}),
            sdbus::registerMethod("DisplayPasskey").implementedAs(
                [](const sdbus::ObjectPath&, uint32_t, uint16_t) {}),
            sdbus::registerMethod("RequestConfirmation").implementedAs(
                [](const sdbus::ObjectPath&, uint32_t) {}),
            sdbus::registerMethod("RequestAuthorization").implementedAs(
                [](const sdbus::ObjectPath&) {}),
            sdbus::registerMethod("AuthorizeService").implementedAs(
                [](const sdbus::ObjectPath&, const std::string&) {}),
            sdbus::registerMethod("Cancel").implementedAs([] {})
        ).forInterface(sdbus::InterfaceName{AGENT_IFACE});

        auto mgr = proxy("/org/bluez");
        try {
            mgr->callMethod("RegisterAgent")
                .onInterface(AGENT_MANAGER_IFACE)
                .withTimeout(DBUS_FAST_TIMEOUT)
                .withArguments(sdbus::ObjectPath{AGENT_PATH}, std::string{"NoInputNoOutput"})
                .storeResultsTo();
        } catch (const sdbus::Error& e) {
            if (std::string(e.getName()) != "org.bluez.Error.AlreadyExists") throw;
        }
        mgr->callMethod("RequestDefaultAgent")
            .onInterface(AGENT_MANAGER_IFACE)
            .withTimeout(DBUS_FAST_TIMEOUT)
            .withArguments(sdbus::ObjectPath{AGENT_PATH})
            .storeResultsTo();
        return true;
    } catch (const sdbus::Error& e) {
        std::println(stderr, "[bt] pairing agent unavailable: {}", e.getMessage());
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
        proxy("/")->callMethod("GetManagedObjects")
            .onInterface(OBJECT_MANAGER_IFACE)
            .withTimeout(DBUS_FAST_TIMEOUT)
            .storeResultsTo(objects);
    } catch (const sdbus::Error& e) {
        if (g_ctx.verbose) std::println(stderr, "[bt] GetManagedObjects failed: {}", e.getMessage());
    }
    return objects;
}

bool BluezManager::ensure_adapter() {
    adapter_path.clear();
    for (const auto& [obj_path, ifaces] : managed_objects()) {
        if (ifaces.find(ADAPTER_IFACE) != ifaces.end()) {
            adapter_path = static_cast<const std::string&>(obj_path);
            break;
        }
    }

    if (adapter_path.empty()) {
        std::println(stderr, "[bt] no BlueZ adapter found. Is bluetooth.service running and hci0 present?");
        return false;
    }

    (void)adapter_set_bool("Powered", true);
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
        proxy(adapter_path)->callMethod("StartDiscovery")
            .onInterface(ADAPTER_IFACE)
            .withTimeout(DBUS_DISCOVERY_TIMEOUT)
            .storeResultsTo();
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
        proxy(adapter_path)->callMethod("StopDiscovery")
            .onInterface(ADAPTER_IFACE)
            .withTimeout(DBUS_DISCOVERY_TIMEOUT)
            .storeResultsTo();
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
        const auto dev_iface = ifaces.find(DEVICE_IFACE);
        if (dev_iface == ifaces.end()) continue;

        const PropertyMap& props = dev_iface->second;
        DeviceInfo dev{};
        dev.path = static_cast<const std::string&>(obj_path);
        read_prop(props, "Address", dev.address);
        read_prop(props, "Name", dev.name);
        read_prop(props, "Alias", dev.alias);
        read_prop(props, "Icon", dev.icon);
        read_prop(props, "UUIDs", dev.uuids);
        read_prop(props, "Class", dev.klass);
        read_prop(props, "Appearance", dev.appearance);
        dev.has_rssi = read_prop(props, "RSSI", dev.rssi);
        read_prop(props, "Paired", dev.paired);
        read_prop(props, "Trusted", dev.trusted);
        read_prop(props, "Connected", dev.connected);
        read_prop(props, "Blocked", dev.blocked);
        if (dev.address.empty()) dev.address = path_to_mac(dev.path);
        out.push_back(std::move(dev));
    }
    return out;
}

bool BluezManager::set_trusted(const DeviceInfo& dev, bool trusted) {
    if (!connection || dev.path.empty()) return false;
    if (dev.trusted == trusted) return true;
    try {
        proxy(dev.path)->setProperty("Trusted").onInterface(DEVICE_IFACE).toValue(trusted);
        return true;
    } catch (const sdbus::Error& e) {
        if (g_ctx.verbose) std::println(stderr, "[bt] failed to trust {}: {}", dev.display_name(), e.getMessage());
        return false;
    }
}

bool BluezManager::pair_device(const DeviceInfo& dev) {
    if (!connection || dev.path.empty()) return false;
    try {
        proxy(dev.path)->callMethod("Pair")
            .onInterface(DEVICE_IFACE)
            .withTimeout(DBUS_PAIR_TIMEOUT)
            .storeResultsTo();
        return true;
    } catch (const sdbus::Error& e) {
        const std::string name = e.getName();
        if (name == "org.bluez.Error.AlreadyExists") return true;
        if (g_ctx.verbose) std::println(stderr, "[bt] pair failed for {} ({}): {}", dev.display_name(), dev.address, e.getMessage());
        return false;
    }
}

bool BluezManager::connect_device_once(const DeviceInfo& dev) {
    if (!connection || dev.path.empty() || dev.connected) return true;
    try {
        proxy(dev.path)->callMethod("Connect")
            .onInterface(DEVICE_IFACE)
            .withTimeout(DBUS_CONNECT_TIMEOUT)
            .storeResultsTo();
        return true;
    } catch (const sdbus::Error& e) {
        const std::string name = e.getName();
        if (name == "org.bluez.Error.AlreadyConnected") return true;
        if (name != "org.bluez.Error.NotReady" && g_ctx.verbose) {
            std::println(stderr, "[bt] connect failed for {} ({}): {}", dev.display_name(), dev.address, e.getMessage());
        }
        return false;
    }
}

bool BluezManager::disconnect_device(const DeviceInfo& dev) {
    if (!connection || dev.path.empty() || !dev.connected) return true;
    try {
        proxy(dev.path)->callMethod("Disconnect")
            .onInterface(DEVICE_IFACE)
            .withTimeout(DBUS_CONNECT_TIMEOUT)
            .storeResultsTo();
        return true;
    } catch (const sdbus::Error& e) {
        const std::string name = e.getName();
        if (name == "org.bluez.Error.NotConnected") return true;
        if (g_ctx.verbose) std::println(stderr, "[bt] disconnect failed for {}: {}", dev.display_name(), e.getMessage());
        return false;
    }
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

    if (g_runtime_pair_window_requested.exchange(false, std::memory_order_relaxed)) {
        if (startup_pair_window) {
            open_pair_window("Switch Change Grip/Order");
        }
    }

    if (pair_window_open && now >= pair_window_end) {
        close_pair_window();
    }

    bool have_connected_controller = false;
    for (const DeviceInfo& dev : list_devices()) {
        if (dev.blocked || !dev.is_controller_like()) continue;

        if (dev.paired || dev.trusted) {
            if (!dev.trusted) (void)set_trusted(dev, true);
            if (dev.connected) {
                have_connected_controller = true;
                note_connected(dev);
                continue;
            } else {
                logged_connected.erase(dev.path);
                if (!pair_window_open) {
                    continue;
                }
            }
        }

        if (!pair_window_open) continue;
        if (!dev.looks_fresh_from_discovery()) continue;
        if (!pairing_attempted.insert(dev.path).second) continue;

        (void)stop_discovery();
        std::println("[bt] pairing {} ({})", dev.display_name(), dev.address);
        if (pair_device(dev)) {
            DeviceInfo paired = dev;
            paired.paired = true;
            (void)set_trusted(paired, true);
            // Pair() often already establishes the HID profile. Connect once only as a finishing nudge;
            // never keep retrying sleeping controllers in the background.
            (void)connect_device_once(paired);
            std::println("[bt] paired {} ({})", dev.display_name(), dev.address);
        }
        if (pair_window_open) (void)start_discovery();
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

    if (startup_pair_window) open_pair_window("startup --pair");

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
    BluezManager mgr(false);
    if (!mgr.connect_bus()) return;
    if (!mgr.ensure_adapter()) { mgr.close_bus(); return; }
    for (const auto& dev : mgr.list_devices()) {
        if (!dev.connected || !dev.is_controller_like()) continue;
        std::println("[bt] disconnecting {} ({}) because Switch suspended", dev.display_name(), dev.address);
        (void)mgr.disconnect_device(dev);
    }
    mgr.close_bus();
}

} // namespace

#else

namespace {
class BluezManager {
public:
    explicit BluezManager(bool) {}
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

void bluetooth_manager_start(bool open_pair_window) {
    g_proactive_reconnect_enabled.store(true, std::memory_order_relaxed);
    g_runtime_pair_window_requested.store(false, std::memory_order_relaxed);
    (void)unlink(BT_RECONNECT_PAUSE_FILE);

    bool expected = false;
    if (!g_manager_running.compare_exchange_strong(expected, true)) return;
    if (g_manager_thread.joinable()) g_manager_thread.join();

    g_manager_thread = std::thread([open_pair_window] {
        BluezManager mgr(open_pair_window);
        mgr.run();
        g_manager_running.store(false, std::memory_order_relaxed);
        if (g_ctx.verbose) std::println("[bt] Bluetooth controller manager stopped");
    });
}

bool bluetooth_manager_request_pairing_window() {
    if (!g_manager_running.load(std::memory_order_relaxed)) return false;
    g_runtime_pair_window_requested.store(true, std::memory_order_relaxed);
    return true;
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
