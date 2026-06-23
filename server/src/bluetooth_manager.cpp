#include "bluetooth_manager.hpp"
#include "app_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <print>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_manager_running{false};
std::atomic<bool> g_proactive_reconnect_enabled{true};
std::thread g_manager_thread;
constexpr const char* BT_RECONNECT_PAUSE_FILE = "/tmp/ns-pc-control-bt-reconnect-paused";

[[maybe_unused]] std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

[[maybe_unused]] std::string mac_to_path_addr(std::string s) {
    for (char& c : s) {
        if (c == ':') c = '_';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

[[maybe_unused]] std::string path_to_mac(const std::string& path) {
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

bool looks_like_mac(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (s[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

bool command_exists(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
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

bool env_is_true(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s = lower_copy(v);
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool env_is_false(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s = lower_copy(v);
    return s == "0" || s == "false" || s == "no" || s == "off";
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
    // ns-backend is often launched with chrt -f 99. Do not let apt/systemctl/modprobe
    // inherit realtime priority; that can make boot/service recovery ugly.
    const std::string wrapped = "if command -v chrt >/dev/null 2>&1; then chrt -o 0 /bin/sh -c " + shell_quote(cmd) +
                                "; else /bin/sh -c " + shell_quote(cmd) + "; fi";
    int rc = std::system(wrapped.c_str());
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 126;
}

bool package_installed(const char* pkg) {
    std::string cmd = "dpkg-query -W -f='${Status}' ";
    cmd += pkg;
    cmd += " 2>/dev/null | grep -q 'install ok installed'";
    return run_cmd(cmd) == 0;
}

bool write_uhid_modules_load(bool verbose) {
    const char* path = "/etc/modules-load.d/uhid.conf";
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line == "uhid") return true;
        }
    }
    std::ofstream out(path, std::ios::app);
    if (!out) {
        if (verbose) std::println(stderr, "[bt] setup: could not write {}; uhid will not persist after reboot", path);
        return false;
    }
    out << "uhid\n";
    if (verbose) std::println("[bt] setup: made uhid persistent in {}", path);
    return true;
}

void runtime_setup_impl(bool verbose) {
    if (geteuid() != 0) {
        if (verbose) std::println(stderr, "[bt] setup: not root; cannot auto-install packages, unblock rfkill, or load uhid");
        return;
    }

    // Runtime packages only. libsystemd-dev/pkg-config are build-time dependencies: if this
    // binary is already running, installing them now cannot change how it was compiled.
    std::vector<const char*> packages = {"bluez", "bluetooth", "rfkill"};
    std::vector<const char*> missing;
    if (command_exists("dpkg-query")) {
        for (const char* pkg : packages) {
            if (!package_installed(pkg)) missing.push_back(pkg);
        }
    }

    const bool skip_auto_apt = env_is_true("NS_BACKEND_SKIP_AUTO_APT") || env_is_false("NS_BACKEND_AUTO_APT");
    if (!missing.empty()) {
        if (!command_exists("apt-get")) {
            std::println(stderr, "[bt] setup: missing packages but apt-get was not found");
        } else if (skip_auto_apt) {
            std::ostringstream oss;
            for (const char* pkg : missing) oss << ' ' << pkg;
            std::println(stderr, "[bt] setup: missing packages:{}", oss.str());
        } else {
            std::ostringstream install;
            install << "DEBIAN_FRONTEND=noninteractive apt-get install -y";
            for (const char* pkg : missing) install << ' ' << pkg;
            std::println("[bt] setup: installing missing runtime Bluetooth packages");
            (void)run_cmd("DEBIAN_FRONTEND=noninteractive apt-get update", verbose);
            if (run_cmd(install.str(), true) != 0) {
                std::println(stderr, "[bt] setup: apt install failed; continuing with whatever is available");
            }
        }
    }

    if (command_exists("rfkill")) {
        (void)run_cmd("rfkill unblock bluetooth >/dev/null 2>&1", verbose);
    }

    if (!proc_modules_contains("uhid")) {
        if (run_cmd("modprobe uhid >/dev/null 2>&1", verbose) == 0) {
            if (verbose) std::println("[bt] setup: loaded uhid");
        } else {
            std::println(stderr, "[bt] setup: failed to load uhid; Xbox BLE controllers may connect without creating input devices");
        }
    }
    (void)write_uhid_modules_load(verbose);

    if (command_exists("systemctl")) {
        (void)run_cmd("systemctl start bluetooth.service >/dev/null 2>&1", verbose);
    } else if (command_exists("service")) {
        (void)run_cmd("service bluetooth start >/dev/null 2>&1", verbose);
    }
}

#ifndef NS_ENABLE_BLUEZ_DBUS

// Fallback for builds without libsystemd/sd-bus. This preserves old behavior,
// but D-Bus builds are preferred because they avoid parsing bluetoothctl text.
static std::atomic<bool> g_fallback_stop{false};

static void run_shell_manager(bool pair_window) {
    std::string script = std::string("initial_pair_window=") + (pair_window ? "1" : "0") + R"BT(
set +e
bt_timeout() { secs="$1"; shift; if command -v timeout >/dev/null 2>&1; then timeout "$secs" "$@"; else "$@"; fi; }
bt_quiet() { secs="$1"; shift; bt_timeout "$secs" "$@" >/dev/null 2>&1; }
is_gamepad_name() { case "$1" in *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*|*DualSense*|*DualShock*|*PLAYSTATION*|*Controller*|*Gamepad*) return 0 ;; *) return 1 ;; esac; }
scan_pid=""
pair_open=0
pair_end=0
seen_connected=" "
pause_file="/tmp/ns-pc-control-bt-reconnect-paused"
cleanup() {
    [ -n "$scan_pid" ] && kill "$scan_pid" 2>/dev/null || true
    [ -n "$agent_pid" ] && kill "$agent_pid" 2>/dev/null || true
    bt_quiet 4s bluetoothctl scan off
    bt_quiet 4s bluetoothctl discoverable off
    bt_quiet 4s bluetoothctl pairable off
}
start_scan() {
    [ -n "$scan_pid" ] && kill "$scan_pid" 2>/dev/null || true
    (printf "scan on\n"; while :; do sleep 3600; done) | bluetoothctl >/dev/null 2>&1 & scan_pid=$!
}
stop_scan() {
    [ -n "$scan_pid" ] && kill "$scan_pid" 2>/dev/null || true
    scan_pid=""
    bt_quiet 4s bluetoothctl scan off
}
open_pair_window() {
    reason="$1"
    pair_open=1
    pair_end=$(( $(date +%s) + 120 ))
    bt_quiet 4s bluetoothctl pairable on
    bt_quiet 4s bluetoothctl discoverable on
    start_scan
    printf '[bt] pairing window open for 2 minutes (%s)\n' "$reason"
}
close_pair_window_now() {
    [ "$pair_open" = "1" ] || return 0
    pair_open=0
    stop_scan
    bt_quiet 4s bluetoothctl discoverable off
    bt_quiet 4s bluetoothctl pairable off
    printf '[bt] pairing window closed; trusted reconnect helper stays active\n'
}
close_pair_window_if_due() {
    [ "$pair_open" = "1" ] || return 0
    [ "$(date +%s)" -lt "$pair_end" ] && return 0
    close_pair_window_now
}
mark_seen_connected() { seen_connected="$seen_connected$1 "; }
was_seen_connected() { case "$seen_connected" in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
mark_disconnected() { seen_connected="$(printf '%s' "$seen_connected" | sed "s/ $1 / /g")"; }

trap cleanup INT TERM EXIT
bt_quiet 4s bluetoothctl power on
(printf "agent NoInputNoOutput\ndefault-agent\n"; while :; do sleep 3600; done) | bluetoothctl >/dev/null 2>&1 & agent_pid=$!
printf '[bt] BlueZ fallback trusted-controller reconnect helper active (bluetoothctl); D-Bus manager not compiled in\n'
if [ "$initial_pair_window" = "1" ]; then
    open_pair_window "startup --pair"
else
    bt_quiet 4s bluetoothctl pairable off
    bt_quiet 4s bluetoothctl discoverable off
fi

while :; do
    close_pair_window_if_due
    if [ -e "$pause_file" ]; then
        close_pair_window_now
        sleep 4
        continue
    fi
    tmp="/tmp/ns-pc-control-bt-devices.$$"
    bluetoothctl devices 2>/dev/null > "$tmp" || true
    while read -r tag mac name_rest; do
        [ "$tag" = "Device" ] || continue
        name="$name_rest"
        is_gamepad_name "$name" || continue
        info="$(bt_timeout 3s bluetoothctl info "$mac" 2>/dev/null || true)"
        if printf '%s\n' "$info" | grep -q 'Connected: yes'; then
            if printf '%s\n' "$info" | grep -q 'ServicesResolved: no'; then
                printf '[bt] stale connected state for %s (%s), reconnecting\n' "$name" "$mac"
                bt_quiet 4s bluetoothctl disconnect "$mac"; sleep 1; bt_quiet 5s bluetoothctl connect "$mac"
            elif printf '%s\n' "$info" | grep -q 'Paired: yes\|Trusted: yes'; then
                if ! was_seen_connected "$mac"; then
                    printf '[bt] connected %s (%s)\n' "$name" "$mac"
                    open_pair_window "trusted controller connected"
                fi
                mark_seen_connected "$mac"
            fi
            continue
        fi

        mark_disconnected "$mac"
        if printf '%s\n' "$info" | grep -q 'Paired: yes\|Trusted: yes'; then
            bt_quiet 4s bluetoothctl trust "$mac"
            stop_scan
            if bt_quiet 5s bluetoothctl connect "$mac"; then
                printf '[bt] reconnected %s (%s)\n' "$name" "$mac"
                mark_seen_connected "$mac"
                open_pair_window "trusted controller connected"
            elif [ "$pair_open" = "1" ]; then
                start_scan
            fi
            continue
        fi

        [ "$pair_open" = "1" ] || continue
        printf '%s\n' "$info" | grep -q 'RSSI:' || continue
        stop_scan
        if bt_quiet 12s bluetoothctl pair "$mac"; then
            bt_quiet 4s bluetoothctl trust "$mac"
            if bt_quiet 8s bluetoothctl connect "$mac"; then
                printf '[bt] paired %s (%s)\n' "$name" "$mac"
                mark_seen_connected "$mac"
            fi
        fi
        [ "$pair_open" = "1" ] && start_scan
    done < "$tmp"
    rm -f "$tmp"
    sleep 4
done
)BT";

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", script.c_str(), (char*)nullptr);
        _exit(127);
    }

    int status = 0;
    while (g_manager_running.load(std::memory_order_relaxed)) {
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(-pid, SIGTERM);
    for (int i = 0; i < 10 && waitpid(pid, &status, WNOHANG) != pid; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
}

#endif

} // namespace

#ifdef NS_ENABLE_BLUEZ_DBUS

#include <systemd/sd-bus.h>

namespace {

constexpr const char* BLUEZ_SERVICE = "org.bluez";
constexpr const char* OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";
constexpr const char* PROPERTIES = "org.freedesktop.DBus.Properties";
constexpr const char* ADAPTER_IFACE = "org.bluez.Adapter1";
constexpr const char* DEVICE_IFACE = "org.bluez.Device1";
constexpr const char* AGENT_MANAGER_IFACE = "org.bluez.AgentManager1";
constexpr const char* AGENT_IFACE = "org.bluez.Agent1";
constexpr const char* AGENT_PATH = "/com/ns_pc_control/agent";
constexpr const char* HID_UUID = "00001124-0000-1000-8000-00805f9b34fb";

struct SdBusErrorGuard {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    ~SdBusErrorGuard() { sd_bus_error_free(&error); }
};

struct SdBusMessageGuard {
    sd_bus_message* msg = nullptr;
    ~SdBusMessageGuard() { sd_bus_message_unref(msg); }
};

struct SdBusSlotGuard {
    sd_bus_slot* slot = nullptr;
    ~SdBusSlotGuard() { sd_bus_slot_unref(slot); }
};

struct DeviceInfo {
    std::string path;
    std::string address;
    std::string name;
    std::string alias;
    std::string icon;
    std::vector<std::string> uuids;
    uint32_t klass = 0;
    int16_t rssi = 0;
    bool has_rssi = false;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    bool services_resolved = false;
    bool blocked = false;

    std::string display_name() const {
        if (!alias.empty()) return alias;
        if (!name.empty()) return name;
        if (!address.empty()) return address;
        return path;
    }

    bool has_hid_uuid() const {
        for (const std::string& uuid : uuids) {
            if (lower_copy(uuid) == HID_UUID) return true;
        }
        return false;
    }

    bool is_peripheral_class() const {
        return (klass & 0x1f00u) == 0x0500u;
    }

    bool is_gamepad_like() const {
        const std::string s = lower_copy(display_name() + " " + icon);
        return has_hid_uuid() || is_peripheral_class() ||
               s.find("gamepad") != std::string::npos ||
               s.find("joystick") != std::string::npos ||
               s.find("controller") != std::string::npos ||
               s.find("wireless") != std::string::npos ||
               s.find("xbox") != std::string::npos ||
               s.find("dualshock") != std::string::npos ||
               s.find("dualsense") != std::string::npos ||
               s.find("playstation") != std::string::npos ||
               s.find("8bitdo") != std::string::npos ||
               s.find("pro controller") != std::string::npos ||
               s.find("joy-con") != std::string::npos ||
               s.find("nintendo") != std::string::npos;
    }

    bool is_xbox_like() const {
        const std::string s = lower_copy(display_name());
        return s.find("xbox") != std::string::npos || s.find("elite") != std::string::npos || s.find("microsoft") != std::string::npos;
    }
};

class BluezManager {
public:
    explicit BluezManager(bool pair_window) : pair_window_requested(pair_window) {}
    void run();
    static void disconnect_gamepads();

private:
    bool pair_window_requested = false;
    sd_bus* bus = nullptr;
    SdBusSlotGuard agent_slot;
    std::string adapter_path;
    bool discovery_active = false;
    bool pair_window_open = false;
    bool initial_device_snapshot_done = false;
    Clock::time_point pair_window_end{};
    std::map<std::string, Clock::time_point> stale_since;
    std::set<std::string> connect_logged;
    std::set<std::string> connected_paths;
    std::set<std::string> missing_sdl_warned;
    std::map<std::string, Clock::time_point> next_connect_attempt;
    std::map<std::string, int> connect_failures;
    std::set<std::string> xbox_driver_warned;

    bool connect_bus();
    void close_bus();
    bool register_agent();
    bool ensure_adapter();
    bool adapter_set_bool(const char* property, bool value);
    bool start_discovery();
    bool stop_discovery();
    std::vector<DeviceInfo> list_devices();
    bool set_trusted(const DeviceInfo& dev, bool trusted);
    bool pair_device(const DeviceInfo& dev);
    bool connect_device(const DeviceInfo& dev);
    bool disconnect_device(const DeviceInfo& dev);
    bool connect_allowed(const DeviceInfo& dev, Clock::time_point now) const;
    void schedule_connect_retry(const DeviceInfo& dev, Clock::time_point now, bool failed);
    void clear_connect_retry(const DeviceInfo& dev);
    void maybe_warn_xbox_driver_path(const DeviceInfo& dev);
    void tick();
    void open_pair_window(const char* reason);
    void note_connected(const DeviceInfo& dev, const char* action, bool trigger_followup_pair_window = false);
};

static int agent_release(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_pin_code(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    (void)sd_bus_message_read(m, "o", &device);
    return sd_bus_reply_method_return(m, "s", "0000");
}

static int agent_request_passkey(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    (void)sd_bus_message_read(m, "o", &device);
    return sd_bus_reply_method_return(m, "u", 0u);
}

static int agent_display_pin_code(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    const char* pincode = nullptr;
    (void)sd_bus_message_read(m, "os", &device, &pincode);
    return sd_bus_reply_method_return(m, "");
}

static int agent_display_passkey(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    uint32_t passkey = 0;
    uint16_t entered = 0;
    (void)sd_bus_message_read(m, "ouq", &device, &passkey, &entered);
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_confirmation(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    uint32_t passkey = 0;
    (void)sd_bus_message_read(m, "ou", &device, &passkey);
    // Headless appliance mode: accept controller confirmations automatically.
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_authorization(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    (void)sd_bus_message_read(m, "o", &device);
    return sd_bus_reply_method_return(m, "");
}

static int agent_authorize_service(sd_bus_message* m, void*, sd_bus_error*) {
    const char* device = nullptr;
    const char* uuid = nullptr;
    (void)sd_bus_message_read(m, "os", &device, &uuid);
    return sd_bus_reply_method_return(m, "");
}

static int agent_cancel(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_request_pin_code, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_request_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_display_pin_code, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_display_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_request_confirmation, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_request_authorization, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_authorize_service, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

bool BluezManager::connect_bus() {
    close_bus();
    int r = sd_bus_default_system(&bus);
    if (r < 0) {
        std::println(stderr, "[bt] BlueZ D-Bus unavailable: {}", std::strerror(-r));
        bus = nullptr;
        return false;
    }
    return true;
}

void BluezManager::close_bus() {
    agent_slot.slot = nullptr;
    if (bus) {
        sd_bus_flush(bus);
        sd_bus_unref(bus);
        bus = nullptr;
    }
}

bool BluezManager::register_agent() {
    if (!bus) return false;
    int r = sd_bus_add_object_vtable(bus, &agent_slot.slot, AGENT_PATH, AGENT_IFACE, agent_vtable, this);
    if (r < 0 && r != -EEXIST) {
        std::println(stderr, "[bt] failed to export pairing agent: {}", std::strerror(-r));
        return false;
    }

    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    r = sd_bus_call_method(bus, BLUEZ_SERVICE, "/org/bluez", AGENT_MANAGER_IFACE,
                           "RegisterAgent", &err.error, &reply.msg, "os", AGENT_PATH, "NoInputNoOutput");
    if (r < 0 && !sd_bus_error_has_name(&err.error, "org.bluez.Error.AlreadyExists")) {
        std::println(stderr, "[bt] failed to register pairing agent: {}", err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }

    sd_bus_error_free(&err.error);
    sd_bus_message_unref(reply.msg);
    reply.msg = nullptr;
    r = sd_bus_call_method(bus, BLUEZ_SERVICE, "/org/bluez", AGENT_MANAGER_IFACE,
                           "RequestDefaultAgent", &err.error, &reply.msg, "o", AGENT_PATH);
    if (r < 0) {
        std::println(stderr, "[bt] failed to make pairing agent default: {}", err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

static bool read_variant_into_device(sd_bus_message* m, const char* prop, DeviceInfo& dev) {
    const char* contents = nullptr;
    int r = sd_bus_message_peek_type(m, nullptr, &contents);
    if (r < 0) return false;
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
    if (r < 0) return false;

    if (std::strcmp(prop, "Address") == 0 || std::strcmp(prop, "Name") == 0 ||
        std::strcmp(prop, "Alias") == 0 || std::strcmp(prop, "Icon") == 0) {
        const char* s = nullptr;
        if (sd_bus_message_read(m, "s", &s) >= 0 && s) {
            if (std::strcmp(prop, "Address") == 0) dev.address = s;
            else if (std::strcmp(prop, "Name") == 0) dev.name = s;
            else if (std::strcmp(prop, "Alias") == 0) dev.alias = s;
            else if (std::strcmp(prop, "Icon") == 0) dev.icon = s;
        }
    } else if (std::strcmp(prop, "Paired") == 0 || std::strcmp(prop, "Trusted") == 0 ||
               std::strcmp(prop, "Connected") == 0 || std::strcmp(prop, "ServicesResolved") == 0 ||
               std::strcmp(prop, "Blocked") == 0) {
        int b = 0;
        if (sd_bus_message_read(m, "b", &b) >= 0) {
            if (std::strcmp(prop, "Paired") == 0) dev.paired = b;
            else if (std::strcmp(prop, "Trusted") == 0) dev.trusted = b;
            else if (std::strcmp(prop, "Connected") == 0) dev.connected = b;
            else if (std::strcmp(prop, "ServicesResolved") == 0) dev.services_resolved = b;
            else if (std::strcmp(prop, "Blocked") == 0) dev.blocked = b;
        }
    } else if (std::strcmp(prop, "Class") == 0) {
        uint32_t klass = 0;
        if (sd_bus_message_read(m, "u", &klass) >= 0) dev.klass = klass;
    } else if (std::strcmp(prop, "RSSI") == 0) {
        int16_t rssi = 0;
        if (sd_bus_message_read(m, "n", &rssi) >= 0) { dev.rssi = rssi; dev.has_rssi = true; }
    } else if (std::strcmp(prop, "UUIDs") == 0) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s") >= 0) {
            const char* uuid = nullptr;
            while (sd_bus_message_read(m, "s", &uuid) > 0) {
                if (uuid) dev.uuids.emplace_back(uuid);
            }
            (void)sd_bus_message_exit_container(m);
        }
    } else {
        (void)sd_bus_message_skip(m, contents);
    }

    (void)sd_bus_message_exit_container(m);
    return true;
}

static bool read_bool_prop(sd_bus* bus, const std::string& path, const char* iface, const char* prop, bool& out) {
    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_get_property(bus, BLUEZ_SERVICE, path.c_str(), iface, prop, &err.error, &reply.msg, "b");
    if (r < 0) return false;
    int b = 0;
    if (sd_bus_message_read(reply.msg, "b", &b) < 0) return false;
    out = b;
    return true;
}

std::vector<DeviceInfo> BluezManager::list_devices() {
    std::vector<DeviceInfo> out;
    if (!bus) return out;

    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, "/", OBJECT_MANAGER, "GetManagedObjects", &err.error, &reply.msg, "");
    if (r < 0) return out;

    r = sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (r < 0) return out;

    while (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0) {
        const char* obj_path = nullptr;
        (void)sd_bus_message_read(reply.msg, "o", &obj_path);
        DeviceInfo dev{};
        if (obj_path) dev.path = obj_path;
        bool is_device = false;

        if (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_ARRAY, "{sa{sv}}") >= 0) {
            while (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}") > 0) {
                const char* iface = nullptr;
                (void)sd_bus_message_read(reply.msg, "s", &iface);
                if (iface && std::strcmp(iface, DEVICE_IFACE) == 0) is_device = true;

                if (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_ARRAY, "{sv}") >= 0) {
                    while (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                        const char* prop = nullptr;
                        (void)sd_bus_message_read(reply.msg, "s", &prop);
                        if (iface && prop && std::strcmp(iface, DEVICE_IFACE) == 0) {
                            (void)read_variant_into_device(reply.msg, prop, dev);
                        } else {
                            (void)sd_bus_message_skip(reply.msg, "v");
                        }
                        (void)sd_bus_message_exit_container(reply.msg);
                    }
                    (void)sd_bus_message_exit_container(reply.msg);
                }
                (void)sd_bus_message_exit_container(reply.msg);
            }
            (void)sd_bus_message_exit_container(reply.msg);
        }
        (void)sd_bus_message_exit_container(reply.msg);

        if (is_device) {
            if (dev.address.empty()) dev.address = path_to_mac(dev.path);
            out.push_back(std::move(dev));
        }
    }
    return out;
}

bool BluezManager::ensure_adapter() {
    if (!bus) return false;

    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, "/", OBJECT_MANAGER, "GetManagedObjects", &err.error, &reply.msg, "");
    if (r < 0) {
        std::println(stderr, "[bt] BlueZ object manager failed: {}", err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }

    r = sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (r < 0) return false;

    while (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0) {
        const char* obj_path = nullptr;
        (void)sd_bus_message_read(reply.msg, "o", &obj_path);
        bool is_adapter = false;
        if (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_ARRAY, "{sa{sv}}") >= 0) {
            while (sd_bus_message_enter_container(reply.msg, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}") > 0) {
                const char* iface = nullptr;
                (void)sd_bus_message_read(reply.msg, "s", &iface);
                if (iface && std::strcmp(iface, ADAPTER_IFACE) == 0) is_adapter = true;
                (void)sd_bus_message_skip(reply.msg, "a{sv}");
                (void)sd_bus_message_exit_container(reply.msg);
            }
            (void)sd_bus_message_exit_container(reply.msg);
        }
        (void)sd_bus_message_exit_container(reply.msg);
        if (is_adapter && obj_path) {
            adapter_path = obj_path;
            break;
        }
    }

    if (adapter_path.empty()) {
        std::println(stderr, "[bt] no BlueZ adapter found. Is bluetooth.service running and hci0 present?");
        return false;
    }

    adapter_set_bool("Powered", true);
    return true;
}

bool BluezManager::adapter_set_bool(const char* property, bool value) {
    if (!bus || adapter_path.empty()) return false;
    SdBusErrorGuard err;
    int r = sd_bus_set_property(bus, BLUEZ_SERVICE, adapter_path.c_str(), ADAPTER_IFACE,
                                property, &err.error, "b", value ? 1 : 0);
    if (r < 0) {
        if (g_ctx.verbose) std::println(stderr, "[bt] failed to set adapter {}={}: {}", property, value, err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

bool BluezManager::start_discovery() {
    if (!bus || adapter_path.empty() || discovery_active) return false;
    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, adapter_path.c_str(), ADAPTER_IFACE,
                               "StartDiscovery", &err.error, &reply.msg, "");
    if (r < 0) {
        if (g_ctx.verbose) std::println(stderr, "[bt] StartDiscovery failed: {}", err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    discovery_active = true;
    return true;
}

bool BluezManager::stop_discovery() {
    if (!bus || adapter_path.empty() || !discovery_active) return true;
    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, adapter_path.c_str(), ADAPTER_IFACE,
                               "StopDiscovery", &err.error, &reply.msg, "");
    discovery_active = false;
    if (r < 0 && !sd_bus_error_has_name(&err.error, "org.bluez.Error.Failed")) {
        if (g_ctx.verbose) std::println(stderr, "[bt] StopDiscovery failed: {}", err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

bool BluezManager::set_trusted(const DeviceInfo& dev, bool trusted) {
    if (!bus || dev.path.empty()) return false;
    SdBusErrorGuard err;
    int r = sd_bus_set_property(bus, BLUEZ_SERVICE, dev.path.c_str(), DEVICE_IFACE,
                                "Trusted", &err.error, "b", trusted ? 1 : 0);
    if (r < 0) {
        if (g_ctx.verbose) std::println(stderr, "[bt] failed to trust {}: {}", dev.display_name(), err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

bool BluezManager::pair_device(const DeviceInfo& dev) {
    if (!bus || dev.path.empty()) return false;
    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, dev.path.c_str(), DEVICE_IFACE,
                               "Pair", &err.error, &reply.msg, "");
    if (r < 0) {
        if (g_ctx.verbose) std::println(stderr, "[bt] pair failed for {} ({}): {}", dev.display_name(), dev.address, err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

bool BluezManager::connect_device(const DeviceInfo& dev) {
    if (!bus || dev.path.empty()) return false;
    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, dev.path.c_str(), DEVICE_IFACE,
                               "Connect", &err.error, &reply.msg, "");
    if (r < 0) {
        if (g_ctx.verbose) std::println(stderr, "[bt] connect failed for {} ({}): {}", dev.display_name(), dev.address, err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

bool BluezManager::disconnect_device(const DeviceInfo& dev) {
    if (!bus || dev.path.empty()) return false;
    SdBusErrorGuard err;
    SdBusMessageGuard reply;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, dev.path.c_str(), DEVICE_IFACE,
                               "Disconnect", &err.error, &reply.msg, "");
    if (r < 0) {
        if (g_ctx.verbose) std::println(stderr, "[bt] disconnect failed for {} ({}): {}", dev.display_name(), dev.address, err.error.message ? err.error.message : std::strerror(-r));
        return false;
    }
    return true;
}

bool BluezManager::connect_allowed(const DeviceInfo& dev, Clock::time_point now) const {
    const auto it = next_connect_attempt.find(dev.path);
    return it == next_connect_attempt.end() || now >= it->second;
}

void BluezManager::schedule_connect_retry(const DeviceInfo& dev, Clock::time_point now, bool failed) {
    if (failed) {
        ++connect_failures[dev.path];
        maybe_warn_xbox_driver_path(dev);
    }

    // Xbox Bluetooth controllers, especially Elite/Series pads, often leave BlueZ with
    // org.bluez.Error.InProgress while the BLE/HID path is still settling. Retrying
    // every tick just spams Connect() and can make the adapter/controller more stuck.
    const auto delay = dev.is_xbox_like() ? std::chrono::seconds(15) : std::chrono::seconds(5);
    next_connect_attempt[dev.path] = now + delay;
}

void BluezManager::clear_connect_retry(const DeviceInfo& dev) {
    next_connect_attempt.erase(dev.path);
    connect_failures.erase(dev.path);
    missing_sdl_warned.erase(dev.path);
}

void BluezManager::maybe_warn_xbox_driver_path(const DeviceInfo& dev) {
    if (!dev.is_xbox_like()) return;
    if (connect_failures[dev.path] < 2) return;
    if (!xbox_driver_warned.insert(dev.path).second) return;

    std::println(stderr,
        "[bt] Xbox controller {} ({}) is paired/trusted but Bluetooth connect keeps failing; "
        "on Raspberry Pi/Linux this usually needs a stable BLE path plus uhid/xpadneo, not more pairing attempts",
        dev.display_name(), dev.address);
}

void BluezManager::open_pair_window(const char* reason) {
    const bool was_open = pair_window_open;
    pair_window_open = true;
    pair_window_end = Clock::now() + std::chrono::minutes(2);
    adapter_set_bool("Pairable", true);
    adapter_set_bool("Discoverable", true);
    if (!discovery_active) start_discovery();
    if (!was_open) {
        std::println("[bt] pairing window open for 2 minutes ({})", reason ? reason : "requested");
    } else if (g_ctx.verbose) {
        std::println("[bt] pairing window extended for 2 minutes ({})", reason ? reason : "requested");
    }
}

void BluezManager::note_connected(const DeviceInfo& dev, const char* action, bool trigger_followup_pair_window) {
    connected_paths.insert(dev.path);
    if (connect_logged.insert(dev.path + action).second) {
        std::println("[bt] {} {} ({})", action, dev.display_name(), dev.address);
    }
    if (trigger_followup_pair_window) {
        open_pair_window("trusted controller connected");
    }
}

void BluezManager::tick() {
    const auto now = Clock::now();

    if (!g_proactive_reconnect_enabled.load(std::memory_order_relaxed)) {
        if (pair_window_open || discovery_active) {
            pair_window_open = false;
            stop_discovery();
            adapter_set_bool("Discoverable", false);
            adapter_set_bool("Pairable", false);
            if (g_ctx.verbose) std::println("[bt] Switch suspended; proactive Bluetooth reconnect paused");
        }
        for (const DeviceInfo& dev : list_devices()) {
            if (dev.is_gamepad_like() && dev.connected) connected_paths.insert(dev.path);
            else connected_paths.erase(dev.path);
        }
        initial_device_snapshot_done = true;
        return;
    }

    if (pair_window_open && now >= pair_window_end) {
        pair_window_open = false;
        stop_discovery();
        adapter_set_bool("Discoverable", false);
        adapter_set_bool("Pairable", false);
        std::println("[bt] pairing window closed; trusted reconnect stays active");
    }

    auto devices = list_devices();
    bool restarted_discovery = false;

    for (const DeviceInfo& dev : devices) {
        if (dev.blocked || !dev.is_gamepad_like()) continue;

        if (dev.connected) {
            if (dev.services_resolved) {
                stale_since.erase(dev.path);
                clear_connect_retry(dev);
                if (dev.paired || dev.trusted) {
                    if (initial_device_snapshot_done && !connected_paths.contains(dev.path)) {
                        note_connected(dev, "connected", true);
                    } else {
                        connected_paths.insert(dev.path);
                    }
                }
            } else {
                auto [it, inserted] = stale_since.emplace(dev.path, now);
                const auto stale_limit = dev.is_xbox_like() ? std::chrono::seconds(20) : std::chrono::seconds(8);
                if (!inserted && now - it->second > stale_limit) {
                    std::println("[bt] stale connected state for {} ({}), reconnecting", dev.display_name(), dev.address);
                    disconnect_device(dev);
                    std::this_thread::sleep_for(std::chrono::milliseconds(600));
                    if (connect_allowed(dev, now) && connect_device(dev)) clear_connect_retry(dev);
                    else schedule_connect_retry(dev, now, true);
                    it->second = now;
                }
            }
            continue;
        }

        stale_since.erase(dev.path);
        connected_paths.erase(dev.path);

        if (dev.paired || dev.trusted) {
            set_trusted(dev, true);
            if (!connect_allowed(dev, now)) continue;

            const bool was_discovering = discovery_active;
            if (was_discovering) stop_discovery();
            if (connect_device(dev)) {
                clear_connect_retry(dev);
                note_connected(dev, "reconnected", true);
            } else {
                schedule_connect_retry(dev, now, true);
            }
            if (was_discovering && pair_window_open) { start_discovery(); restarted_discovery = true; }
            continue;
        }

        if (!pair_window_open) continue;
        // Only pair devices created/updated by the discovery session. RSSI is not mandatory on every adapter,
        // but it is a good signal that this is a nearby live device, not ancient BlueZ cache.
        if (!dev.has_rssi && dev.uuids.empty() && dev.name.empty()) continue;

        stop_discovery();
        std::println("[bt] pairing {} ({})", dev.display_name(), dev.address);
        if (pair_device(dev)) {
            DeviceInfo paired_dev = dev;
            paired_dev.paired = true;
            set_trusted(paired_dev, true);

            // Xbox Elite/Series pads commonly need a short BLE/BlueZ settle after Pair()
            // before Connect(); without this they often return InProgress/timeout loops.
            if (paired_dev.is_xbox_like()) std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            if (connect_device(paired_dev)) {
                clear_connect_retry(paired_dev);
                note_connected(paired_dev, "paired");
            } else {
                schedule_connect_retry(paired_dev, now, true);
            }
        }
        if (pair_window_open) { start_discovery(); restarted_discovery = true; }
    }

    if (pair_window_open && !discovery_active && !restarted_discovery) start_discovery();
    initial_device_snapshot_done = true;
}

void BluezManager::run() {
    if (!connect_bus()) return;
    register_agent();
    if (!ensure_adapter()) { close_bus(); return; }

    adapter_set_bool("Pairable", false);
    adapter_set_bool("Discoverable", false);
    std::println("[bt] BlueZ D-Bus manager active; trusted controllers may reconnect anytime");
    if (pair_window_requested) {
        open_pair_window("startup --pair");
    }

    while (g_manager_running.load(std::memory_order_relaxed)) {
        (void)sd_bus_process(bus, nullptr);
        tick();
        for (int i = 0; i < 20 && g_manager_running.load(std::memory_order_relaxed); ++i) {
            (void)sd_bus_process(bus, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    stop_discovery();
    adapter_set_bool("Discoverable", false);
    adapter_set_bool("Pairable", false);
    close_bus();
}

void BluezManager::disconnect_gamepads() {
    BluezManager mgr(false);
    if (!mgr.connect_bus() || !mgr.ensure_adapter()) return;
    for (const auto& d : mgr.list_devices()) {
        if (d.is_gamepad_like() && d.connected) {
            std::println("[bt] disconnecting {} ({}) because Switch suspended", d.display_name(), d.address);
            mgr.disconnect_device(d);
        }
    }
}

} // namespace

#endif // NS_ENABLE_BLUEZ_DBUS

void bluetooth_manager_runtime_setup(bool verbose) {
    static std::atomic<bool> already_done{false};
    bool expected = false;
    if (!already_done.compare_exchange_strong(expected, true)) return;
    runtime_setup_impl(verbose);
}

void bluetooth_manager_start(bool open_pair_window) {
    g_proactive_reconnect_enabled.store(true, std::memory_order_relaxed);
    (void)unlink(BT_RECONNECT_PAUSE_FILE);
    bool expected = false;
    if (!g_manager_running.compare_exchange_strong(expected, true)) return;
    if (g_manager_thread.joinable()) g_manager_thread.join();

#ifdef NS_ENABLE_BLUEZ_DBUS
    g_manager_thread = std::thread([open_pair_window] {
        BluezManager mgr(open_pair_window);
        mgr.run();
        g_manager_running.store(false, std::memory_order_relaxed);
        std::println("[bt] BlueZ manager stopped");
    });
#else
    g_fallback_stop.store(false, std::memory_order_relaxed);
    g_manager_thread = std::thread([open_pair_window] {
        run_shell_manager(open_pair_window);
        g_manager_running.store(false, std::memory_order_relaxed);
        std::println("[bt] fallback Bluetooth manager stopped");
    });
#endif
}

void bluetooth_manager_set_proactive_reconnect_enabled(bool enabled) {
    const bool was_enabled = g_proactive_reconnect_enabled.exchange(enabled, std::memory_order_relaxed);
    if (enabled) {
        (void)unlink(BT_RECONNECT_PAUSE_FILE);
    } else {
        std::ofstream out(BT_RECONNECT_PAUSE_FILE);
        if (out) out << "1\n";
    }
    if (g_ctx.verbose && was_enabled != enabled) {
        std::println("[bt] proactive Bluetooth reconnect {}", enabled ? "enabled" : "paused");
    }
}

void bluetooth_manager_stop() {
    g_manager_running.store(false, std::memory_order_relaxed);
    if (g_manager_thread.joinable()) g_manager_thread.join();
}

void bluetooth_manager_disconnect_connected_gamepads() {
#ifdef NS_ENABLE_BLUEZ_DBUS
    BluezManager::disconnect_gamepads();
#else
    const char* script = R"BT(
set +e
bt_timeout() { secs="$1"; shift; if command -v timeout >/dev/null 2>&1; then timeout "$secs" "$@"; else "$@"; fi; }
is_gamepad_name() { case "$1" in *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*|*DualSense*|*DualShock*|*PLAYSTATION*|*Controller*|*Gamepad*) return 0 ;; *) return 1 ;; esac; }
bluetoothctl devices 2>/dev/null | while read -r tag mac name; do
    [ "$tag" = "Device" ] || continue
    is_gamepad_name "$name" || continue
    info="$(bt_timeout 2s bluetoothctl info "$mac" 2>/dev/null || true)"
    echo "$info" | grep -q 'Connected: yes' || continue
    echo "[bt] disconnecting $name ($mac) because Switch suspended"
    bt_timeout 3s bluetoothctl disconnect "$mac" >/dev/null 2>&1 || true
done
)BT";
    (void)std::system(script);
#endif
}
