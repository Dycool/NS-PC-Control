#include "gadget_wakeup.hpp"
#include "app_state.hpp"
#include "virtual_controller.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace ns;

constexpr const char* GADGET_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl";
constexpr const char* CONFIG_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1";

bool hidg_nodes_ready() {
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
        if (access(path, R_OK | W_OK) != 0)
            return false;
    }
    return true;
}

bool dir_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0;
}

bool mkdir_if_needed(const char* path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    std::fprintf(stderr, "[gadget] mkdir %s failed: %s\n", path, std::strerror(errno));
    return false;
}

bool write_all_fd(int fd, const void* data, size_t len, const char* path) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[gadget] write %s failed: %s\n", path, std::strerror(errno));
            return false;
        }
        if (w == 0) {
            std::fprintf(stderr, "[gadget] write %s wrote 0 bytes\n", path);
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

bool write_bytes_file(const char* path, const void* data, size_t len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        std::fprintf(stderr, "[gadget] open %s failed: %s\n", path, std::strerror(errno));
        return false;
    }
    bool ok = write_all_fd(fd, data, len, path);
    close(fd);
    return ok;
}

bool write_text_file(const char* path, const char* text) {
    return write_bytes_file(path, text, std::strlen(text));
}

void remove_link_if_exists(const char* path) {
    if (unlink(path) != 0) {
        if (errno != ENOENT && errno != EISDIR && errno != EPERM) {
            if (g_verbose) {
                std::fprintf(stderr, "[gadget] unlink %s failed: %s\n",
                             path, std::strerror(errno));
            }
        }
    }
}

void rmdir_if_exists(const char* path) {
    if (rmdir(path) != 0 && errno != ENOENT) {
        if (g_verbose)
            std::fprintf(stderr, "[gadget] rmdir %s failed: %s\n", path, std::strerror(errno));
    }
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string first_udc_name() {
    DIR* d = opendir("/sys/class/udc");
    if (!d) return "";
    std::string out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        out = e->d_name;
        break;
    }
    closedir(d);
    return out;
}

bool create_hid_function(int id) {
    char func[256];
    std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, id);
    if (!mkdir_if_needed(func)) return false;

    char path[320];
    std::snprintf(path, sizeof(path), "%s/protocol", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/subclass", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/report_length", func);
    if (!write_text_file(path, g_legacy_mode ? "8" : "64")) return false;

    std::snprintf(path, sizeof(path), "%s/report_desc", func);
    if (g_legacy_mode) {
        if (!write_bytes_file(path, LEGACY_REPORT_DESC, sizeof(LEGACY_REPORT_DESC))) return false;
    } else {
        if (!write_bytes_file(path, VIRTUAL_CONTROLLER_REPORT_DESC, sizeof(VIRTUAL_CONTROLLER_REPORT_DESC))) return false;
    }

    char link_path[320];
    std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, id);
    unlink(link_path);
    if (symlink(func, link_path) != 0) {
        std::fprintf(stderr, "[gadget] symlink %s -> %s failed: %s\n",
                     link_path, func, std::strerror(errno));
        return false;
    }

    return true;
}

void teardown_gadget() {
    g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
            g_switch2_last_usb_activity_us.store(0, std::memory_order_relaxed);
    if (!path_exists(GADGET_DIR)) return;

    std::puts("[gadget] Closing USB gadget...");

    // Unbind first.  This disconnects the virtual controllers from the console.
    std::string udc_path = join_path(GADGET_DIR, "UDC");
    write_text_file(udc_path.c_str(), "");

    // Remove config links before removing functions, mirroring setup_gadget.sh.
    for (int i = 0; i < 4; ++i) {
        char link_path[320];
        std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, i);
        remove_link_if_exists(link_path);
    }

    // Configfs object directories are removed with rmdir; their pseudo-attribute
    // files must not be unlinked manually.
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409");
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1");

    for (int i = 0; i < 4; ++i) {
        char func[256];
        std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, i);
        rmdir_if_exists(func);
    }

    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409");
    rmdir_if_exists(GADGET_DIR);

    std::puts("[gadget] USB gadget closed");
}

int run_shell_best_effort(const char* cmd) {
    int rc = std::system(cmd);
    return rc;
}


std::string trim_copy(const std::string& in) {
    size_t a = 0;
    while (a < in.size() && std::isspace((unsigned char)in[a])) ++a;
    size_t b = in.size();
    while (b > a && std::isspace((unsigned char)in[b - 1])) --b;
    return in.substr(a, b - a);
}

std::string uppercase_hex_copy(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (!std::isspace((unsigned char)ch))
            out.push_back((char)std::toupper((unsigned char)ch));
    }
    return out;
}

std::string lowercase_copy(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

bool valid_mac_string(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (size_t i = 0; i < mac.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') return false;
        } else if (!std::isxdigit((unsigned char)mac[i])) {
            return false;
        }
    }
    return true;
}

bool valid_adv_hex(const std::string& adv) {
    if (adv.empty() || (adv.size() % 2) != 0 || adv.size() > 62) return false;
    for (char ch : adv) {
        if (!std::isxdigit((unsigned char)ch)) return false;
    }
    return true;
}

bool valid_hci_dev_string(const std::string& hci) {
    if (hci.size() < 4) return false;
    if (hci.rfind("hci", 0) != 0) return false;
    for (size_t i = 3; i < hci.size(); ++i) {
        if (!std::isdigit((unsigned char)hci[i])) return false;
    }
    return true;
}

bool command_exists(const char* cmd) {
    std::string test = "command -v ";
    test += cmd;
    test += " >/dev/null 2>&1";
    return std::system(test.c_str()) == 0;
}

bool ensure_parent_dir_for_file(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        if (slash == 0) return true;
        return true;
    }
    std::string dir = path.substr(0, slash);
    std::string cmd = "mkdir -p '" + dir + "'";
    return std::system(cmd.c_str()) == 0;
}

bool read_switch2_wakeup_config_file(const std::string& path, std::string& mac, std::string& adv, std::string& hci) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(line.substr(0, eq));
        std::string val = trim_copy(line.substr(eq + 1));
        if (key == "mac") mac = lowercase_copy(val);
        else if (key == "adv") adv = uppercase_hex_copy(val);
        else if (key == "hci") hci = trim_copy(val);
    }

    if (hci.empty()) hci = "hci0";
    return valid_mac_string(mac) && valid_adv_hex(adv) && valid_hci_dev_string(hci);
}

bool load_switch2_wakeup_config(bool quiet_if_missing) {
    std::string mac, adv, hci;
    if (!read_switch2_wakeup_config_file(g_switch2_wakeup_config_path, mac, adv, hci)) {
        g_switch2_wake_config_loaded = false;
        if (!quiet_if_missing && g_verbose)
            std::printf("[wake] No valid Switch 2 wake config at %s; wake disabled\n", g_switch2_wakeup_config_path.c_str());
        return false;
    }

    g_switch2_wake_mac = mac;
    g_switch2_wake_adv_hex = adv;
    g_switch2_wake_hci_dev = valid_hci_dev_string(hci) ? hci : "hci0";
    g_switch2_wake_config_loaded = true;
    if (g_verbose) {
        std::printf("[wake] Loaded Switch 2 wake config from %s (mac=%s adv_bytes=%zu hci=%s)\n",
                    g_switch2_wakeup_config_path.c_str(), g_switch2_wake_mac.c_str(), g_switch2_wake_adv_hex.size() / 2,
                    g_switch2_wake_hci_dev.c_str());
    }
    return true;
}

bool save_switch2_wakeup_config(const std::string& mac, const std::string& adv, const std::string& hci_dev = "hci0") {
    if (!ensure_parent_dir_for_file(g_switch2_wakeup_config_path)) {
        std::fprintf(stderr, "[wake] Failed to create config directory for %s\n", g_switch2_wakeup_config_path.c_str());
        return false;
    }

    std::ofstream f(g_switch2_wakeup_config_path, std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[wake] Failed to open %s for writing\n", g_switch2_wakeup_config_path.c_str());
        return false;
    }

    f << "# NS-PC-Control Switch 2 Joy-Con 2 wake configuration\n";
    f << "# Generated by: sudo ./ns-backend -wake\n";
    f << "# Keep this private-ish: it contains your paired controller wake identity.\n";
    f << "mac=" << lowercase_copy(mac) << "\n";
    f << "adv=" << uppercase_hex_copy(adv) << "\n";
    f << "hci=" << (valid_hci_dev_string(hci_dev) ? hci_dev : "hci0") << "\n";
    f.close();

    chmod(g_switch2_wakeup_config_path.c_str(), 0600);
    return true;
}

std::string adv_hex_to_hcitool_args(const std::string& adv_hex) {
    std::string adv = uppercase_hex_copy(adv_hex);
    size_t bytes = adv.size() / 2;
    std::ostringstream oss;
    char len[8];
    std::snprintf(len, sizeof(len), "%02X", (unsigned)bytes);
    oss << len;
    for (size_t i = 0; i < bytes; ++i)
        oss << ' ' << adv.substr(i * 2, 2);
    for (size_t i = bytes; i < 31; ++i)
        oss << " 00";
    return oss.str();
}

struct WakeCmdResult {
    int exit_code = -1;
    std::string output;
};

WakeCmdResult run_wake_command(const std::vector<std::string>& args,
                                      bool verbose_output,
                                      bool capture_output = false,
                                      int timeout_ms = 8000) {
    WakeCmdResult res;
    if (args.empty())
        return res;
    if (timeout_ms <= 0)
        timeout_ms = 8000;

    int pipefd[2] = {-1, -1};
    if (capture_output) {
        if (pipe(pipefd) != 0) {
            res.output = std::string("pipe failed: ") + std::strerror(errno);
            return res;
        }
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags >= 0)
            fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        res.output = std::string("fork failed: ") + std::strerror(errno);
        return res;
    }

    if (pid == 0) {
        if (capture_output) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
        } else if (!verbose_output) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (capture_output)
        close(pipefd[1]);

    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    bool exited = false;

    while (true) {
        if (capture_output) {
            char buf[4096];
            while (true) {
                ssize_t n = read(pipefd[0], buf, sizeof(buf));
                if (n > 0) {
                    res.output.append(buf, (size_t)n);
                    continue;
                }
                if (n == 0)
                    break;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                break;
            }
        }

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            exited = true;
            break;
        }
        if (w < 0) {
            if (errno == EINTR)
                continue;
            res.output += std::string("waitpid failed: ") + std::strerror(errno);
            break;
        }

        if (Clock::now() >= deadline) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 10; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    exited = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!exited) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
            res.exit_code = 124;
            res.output += "command timed out";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (capture_output) {
        char buf[4096];
        while (true) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                res.output.append(buf, (size_t)n);
                continue;
            }
            break;
        }
        close(pipefd[0]);
    }

    if (exited) {
        if (WIFEXITED(status))
            res.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            res.exit_code = 128 + WTERMSIG(status);
        else
            res.exit_code = -1;
    }

    return res;
}

bool wake_cmd_ok(const std::vector<std::string>& args, bool verbose_output) {
    WakeCmdResult r = run_wake_command(args, verbose_output, false);
    return r.exit_code == 0;
}

std::string parse_first_hci_device(const std::string& info) {
    std::istringstream iss(info);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.rfind("hci", 0) == 0) {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
                return line.substr(0, colon);
        }
    }
    return "";
}

std::string parse_btmgmt_addr(const std::string& info) {
    std::istringstream iss(info);
    std::string tok;
    while (iss >> tok) {
        if (tok == "addr") {
            std::string addr;
            if (iss >> addr)
                return uppercase_hex_copy(addr);
        }
    }
    return "";
}

std::string first_hci_from_sysfs() {
    DIR* d = opendir("/sys/class/bluetooth");
    if (!d) return "";

    std::vector<std::string> devices;
    while (dirent* ent = readdir(d)) {
        std::string name = ent->d_name ? ent->d_name : "";
        if (valid_hci_dev_string(name))
            devices.push_back(name);
    }
    closedir(d);

    if (devices.empty()) return "";
    std::sort(devices.begin(), devices.end());
    return devices.front();
}

std::string detect_wake_hci_for_setup() {
    std::string hci = first_hci_from_sysfs();
    if (valid_hci_dev_string(hci))
        return hci;

    WakeCmdResult r = run_wake_command({"btmgmt", "info"}, false, true, 3000);
    if (r.exit_code == 0) {
        hci = parse_first_hci_device(r.output);
        if (valid_hci_dev_string(hci))
            return hci;
    }

    return "hci0";
}

bool wait_for_wake_hci(std::string& hci_dev, bool verbose_output) {
    // Runtime wake path must not depend on `btmgmt info`.
    // Under systemd service mode, `btmgmt info` can hang in the backend cgroup even
    // though the exact same raw wake sequence works from a shell/systemd-run.
    // The Raspberry Pi onboard Bluetooth adapter is hci0 for the supported setup, so
    // use it directly and let the actual btmgmt/hcitool commands be the real test.
    if (hci_dev.empty())
        hci_dev = "hci0";
    if (verbose_output)
        std::printf("[wake] Using Bluetooth controller %s\n", hci_dev.c_str());
    return true;
}

void wake_disable_advertising_quiet(const std::string& hci_dev) {
    run_wake_command({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000A", "00"}, false, false, 3000);
}

bool reset_wake_bt_stack(std::string& hci_dev, bool verbose_output) {
    if (hci_dev.empty())
        hci_dev = "hci0";
    if (verbose_output)
        std::fprintf(stderr, "[wake] Resetting Bluetooth stack / hciuart\n");

    // Keep this close to the working standalone wake script, but do not ask btmgmt
    // for adapter discovery here; service mode has been observed hanging there.
    run_wake_command({"systemctl", "stop", "bluetooth"}, false, false, 5000);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false, false, 3000);
    wake_disable_advertising_quiet(hci_dev);
    run_wake_command({"systemctl", "restart", "hciuart"}, false, false, 15000);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return true;
}

bool prepare_wake_controller(std::string& hci_dev, const std::string& mac_lc, bool verbose_output) {
    if (hci_dev.empty())
        hci_dev = "hci0";

    run_wake_command({"systemctl", "stop", "bluetooth"}, false, false, 5000);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false, false, 3000);

    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (verbose_output)
            std::printf("[wake] Preparing Bluetooth controller, attempt %d, device %s\n", attempt, hci_dev.c_str());

        wake_cmd_ok({"btmgmt", "-i", hci_dev, "power", "off"}, verbose_output);

        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "privacy", "off"}, verbose_output)) {
            reset_wake_bt_stack(hci_dev, verbose_output);
            continue;
        }
        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "bredr", "off"}, verbose_output)) {
            reset_wake_bt_stack(hci_dev, verbose_output);
            continue;
        }
        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "le", "on"}, verbose_output)) {
            reset_wake_bt_stack(hci_dev, verbose_output);
            continue;
        }
        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "public-addr", mac_lc}, verbose_output)) {
            reset_wake_bt_stack(hci_dev, verbose_output);
            continue;
        }
        if (!wake_cmd_ok({"btmgmt", "-i", hci_dev, "power", "on"}, verbose_output)) {
            reset_wake_bt_stack(hci_dev, verbose_output);
            continue;
        }

        // Do not verify with `btmgmt info` here. Verification is nice in an
        // interactive shell, but on the systemd service path it can hang before the
        // advert is sent. The wake packet itself is the important operation.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (verbose_output)
            std::printf("[wake] Bluetooth controller prepared as %s\n", mac_lc.c_str());
        return true;
    }

    return false;
}

bool start_wake_raw_advertising(const std::string& hci_dev,
                                       const std::string& mac_lc,
                                       const std::string& adv_uc,
                                       int seconds,
                                       bool verbose_output) {
    std::vector<std::string> adv_args;
    adv_args.reserve(32);
    size_t adv_bytes = adv_uc.size() / 2;
    char len_arg[8];
    std::snprintf(len_arg, sizeof(len_arg), "%02X", (unsigned)adv_bytes);
    adv_args.push_back(len_arg);
    for (size_t i = 0; i < adv_bytes; ++i)
        adv_args.push_back(adv_uc.substr(i * 2, 2));
    while (adv_args.size() < 32)
        adv_args.push_back("00");

    if (verbose_output)
        std::printf("[wake] Disable advertising first\n");
    wake_disable_advertising_quiet(hci_dev);

    if (verbose_output)
        std::printf("[wake] Set advertising parameters\n");
    if (!wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0006",
                      "20", "00", "40", "00", "03", "00", "00",
                      "00", "00", "00", "00", "00", "00", "07", "00"}, verbose_output))
        return false;

    if (verbose_output)
        std::printf("[wake] Set advertising data (%zu bytes)\n", adv_bytes);
    std::vector<std::string> set_adv = {"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0008"};
    set_adv.insert(set_adv.end(), adv_args.begin(), adv_args.end());
    if (!wake_cmd_ok(set_adv, verbose_output))
        return false;

    if (verbose_output)
        std::printf("[wake] Clear scan response data\n");
    std::vector<std::string> clear_scan = {"hcitool", "-i", hci_dev, "cmd", "0x08", "0x0009", "00"};
    for (int i = 0; i < 31; ++i)
        clear_scan.push_back("00");
    if (!wake_cmd_ok(clear_scan, verbose_output))
        return false;

    if (verbose_output)
        std::printf("[wake] Enable advertising as %s for %ds\n", mac_lc.c_str(), seconds);
    if (!wake_cmd_ok({"hcitool", "-i", hci_dev, "cmd", "0x08", "0x000A", "01"}, verbose_output))
        return false;

    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    if (verbose_output)
        std::printf("[wake] Disable advertising\n");
    wake_disable_advertising_quiet(hci_dev);
    return true;
}

bool send_switch2_wake_advert_once(const std::string& mac,
                                           const std::string& adv_hex,
                                           int seconds,
                                           bool verbose_output = false,
                                           bool force_prepare = false) {
    if (!valid_mac_string(mac) || !valid_adv_hex(adv_hex)) {
        std::fprintf(stderr, "[wake] Invalid wake MAC/ADV; not sending\n");
        return false;
    }
    if (seconds <= 0) seconds = 1;
    if (seconds > 10) seconds = 10;

    const std::string mac_lc = lowercase_copy(mac);
    const std::string adv_uc = uppercase_hex_copy(adv_hex);
    std::string hci_dev = valid_hci_dev_string(g_switch2_wake_hci_dev) ? g_switch2_wake_hci_dev : "hci0";

    if (verbose_output) {
        std::printf("[wake] Wake MAC: %s\n", mac_lc.c_str());
        std::printf("[wake] ADV bytes: %zu\n", adv_uc.size() / 2);
        std::printf("[wake] Duration: %ds\n", seconds);
        if (!force_prepare)
            std::printf("[wake] Fast runtime path: sending ADV directly on %s without Bluetooth prep\n", hci_dev.c_str());
    }

    bool ok = false;

    if (!force_prepare) {
        // Runtime/service wake path: the -wake setup already registered the HCI
        // adapter and prepared/spoofed the controller identity. Do not spend
        // several seconds on btmgmt cleanup every time a client connects; just
        // emit the saved Joy-Con 2 advertising payload immediately.
        ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose_output);

        // If raw HCI itself fails, fall back to the full prepare path once. This
        // preserves recovery from cold/odd Bluetooth states without slowing down
        // the normal good path.
        if (!ok && verbose_output)
            std::fprintf(stderr, "[wake] Fast ADV failed; falling back to full Bluetooth prepare once.\n");
    }

    if (!ok) {
        if (prepare_wake_controller(hci_dev, mac_lc, verbose_output))
            ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose_output);
    }

    if (!ok && force_prepare) {
        if (verbose_output)
            std::fprintf(stderr, "[wake] Raw advertising failed. Trying one Bluetooth stack reset, then retrying once.\n");
        if (reset_wake_bt_stack(hci_dev, verbose_output) && prepare_wake_controller(hci_dev, mac_lc, verbose_output))
            ok = start_wake_raw_advertising(hci_dev, mac_lc, adv_uc, seconds, verbose_output);
    }

    wake_disable_advertising_quiet(hci_dev);

    if (!ok) {
        std::fprintf(stderr, "[wake] Raw HCI wake advert failed\n");
        return false;
    }
    if (verbose_output)
        std::printf("[wake] Done\n");
    return true;
}

void switch2_wake_adv_worker(int burst_ms) {
    int seconds = std::max(1, (burst_ms + 999) / 1000);
    send_switch2_wake_advert_once(g_switch2_wake_mac, g_switch2_wake_adv_hex, seconds, g_verbose, false);
    g_switch2_wake_adv_running.store(false, std::memory_order_relaxed);
}

void maybe_send_switch2_wake_advert(const char* reason) {
    if (!g_switch2_wake_adv_enabled)
        return;
    if (!g_switch2_wake_config_loaded)
        return;

    const uint64_t now = now_us();

    if (switch2_usb_host_recently_active(now)) {
        if (g_verbose) {
            std::printf("[wake] %s; recent Switch USB activity seen, skipping wake advert\n",
                        reason ? reason : "client connected");
        }
        return;
    }

    // Only send wake advertisements when a PC/web/mobile client is actually alive.
    if (!any_recent_client_active(now))
        return;

    const uint64_t last = g_switch2_last_wake_adv_us.load(std::memory_order_relaxed);
    if (last != 0 && elapsed_us_saturated(now, last) < SWITCH2_WAKE_ADV_COOLDOWN_US)
        return;

    bool expected = false;
    if (!g_switch2_wake_adv_running.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;

    g_switch2_last_wake_adv_us.store(now, std::memory_order_relaxed);

    if (g_verbose) {
        std::printf("[wake] %s; sending Joy-Con 2 BLE wake advert for %dms as %s\n",
                    reason ? reason : "client connected", SWITCH2_WAKE_ADV_BURST_MS,
                    g_switch2_wake_mac.c_str());
    } else {
        std::printf("[wake] waking up Switch 2\n");
    }

    std::thread(switch2_wake_adv_worker, SWITCH2_WAKE_ADV_BURST_MS).detach();
}

bool parse_nintendo_adv_from_btmon_log(const std::string& path,
                                             const std::string& preferred_mac,
                                             std::string& out_mac,
                                             std::string& out_adv) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    std::string current_mac;
    std::string preferred = lowercase_copy(preferred_mac);
    while (std::getline(f, line)) {
        std::string lower = lowercase_copy(line);
        size_t ap = lower.find("address:");
        if (ap != std::string::npos) {
            size_t p = ap + 8;
            while (p < line.size() && std::isspace((unsigned char)line[p])) ++p;
            if (p + 17 <= line.size()) {
                std::string candidate = lowercase_copy(line.substr(p, 17));
                if (valid_mac_string(candidate)) current_mac = candidate;
            }
        }

        size_t dp = lower.find("data[24]:");
        if (dp == std::string::npos)
            continue;
        size_t p = line.find(':', dp);
        if (p == std::string::npos)
            continue;
        std::string data = uppercase_hex_copy(line.substr(p + 1));
        if (data.size() != 48)
            continue;
        if (current_mac.empty())
            continue;
        if (!preferred.empty() && current_mac != preferred)
            continue;

        // Full raw advertising payload: Flags + manufacturer data header + Nintendo data.
        out_mac = current_mac;
        out_adv = "0201061BFF5305" + data;
        if (valid_adv_hex(out_adv))
            return true;
    }
    return false;
}

bool capture_switch2_wake_advert(int seconds,
                                        const std::string& preferred_mac,
                                        std::string& out_mac,
                                        std::string& out_adv) {
    char log_path[128];
    std::snprintf(log_path, sizeof(log_path), "/tmp/ns_switch2_wake_%ld.log", (long)getpid());

    std::ostringstream cmd;
    const std::string hci_dev = valid_hci_dev_string(g_switch2_wake_hci_dev) ? g_switch2_wake_hci_dev : "hci0";

    cmd << "sh -c 'rm -f " << log_path << "; "
        << "timeout " << seconds << " btmon -T > " << log_path << " 2>&1 & mon=$!; "
        << "sleep 1; "
        << "timeout " << std::max(1, seconds - 2) << " hcitool -i " << hci_dev << " lescan --duplicates >/dev/null 2>&1 || true; "
        << "kill $mon >/dev/null 2>&1 || true; wait $mon >/dev/null 2>&1 || true'";

    run_wake_command({"systemctl", "stop", "bluetooth"}, false, false, 5000);
    run_wake_command({"rfkill", "unblock", "bluetooth"}, false, false, 3000);
    run_wake_command({"btmgmt", "-i", hci_dev, "power", "off"}, false, false, 5000);
    run_wake_command({"btmgmt", "-i", hci_dev, "privacy", "off"}, false, false, 5000);
    run_wake_command({"btmgmt", "-i", hci_dev, "bredr", "off"}, false, false, 5000);
    run_wake_command({"btmgmt", "-i", hci_dev, "le", "on"}, false, false, 5000);
    run_wake_command({"btmgmt", "-i", hci_dev, "power", "on"}, false, false, 5000);

    int rc = std::system(cmd.str().c_str());
    (void)rc;

    bool ok = parse_nintendo_adv_from_btmon_log(log_path, preferred_mac, out_mac, out_adv);
    unlink(log_path);
    return ok;
}

void wait_for_enter(const char* prompt) {
    if (prompt && *prompt) std::printf("%s", prompt);
    std::fflush(stdout);
    std::string dummy;
    std::getline(std::cin, dummy);
}

bool auto_find_joycon_for_setup(std::string& joycon_mac) {
    std::puts("\n[wake] Step 1/4: Finding your Joy-Con 2 automatically.");
    std::puts("[wake] Put the Joy-Con 2 very close to the Pi and hold the small SYNC button.");

    std::string adv;
    for (int attempt = 1; attempt <= 12; ++attempt) {
        std::printf("[wake] Scanning for Nintendo/Joy-Con 2 advert... attempt %d/12\n", attempt);
        if (capture_switch2_wake_advert(10, "", joycon_mac, adv)) {
            std::printf("[wake] Found Nintendo controller: %s\n", joycon_mac.c_str());
            return true;
        }
        std::puts("[wake] No Nintendo advert yet. Keep holding SYNC or press it again; retrying...");
    }

    std::fprintf(stderr, "[wake] Could not find a Nintendo/Joy-Con 2 advertisement. Keep it very close to the Pi and try again.\n");
    return false;
}

int run_switch2_wakeup_setup() {
    if (geteuid() != 0) {
        std::fprintf(stderr, "[wake] -wake needs root because it controls Bluetooth. Run with sudo.\n");
        return 2;
    }

    const char* required[] = {"btmon", "btmgmt", "hcitool", "rfkill", "systemctl"};
    for (const char* cmd : required) {
        if (!command_exists(cmd)) {
            std::fprintf(stderr, "[wake] Missing command: %s. Install BlueZ tools with: sudo apt install bluez\n", cmd);
            return 2;
        }
    }

    g_switch2_wake_hci_dev = detect_wake_hci_for_setup();

    std::puts("NS-PC-Control Switch 2 Joy-Con 2 wake setup");
    std::puts("------------------------------------------------");
    std::printf("[wake] Config will be saved to: %s\n", g_switch2_wakeup_config_path.c_str());
    std::printf("[wake] Bluetooth adapter registered for runtime wake: %s\n", g_switch2_wake_hci_dev.c_str());
    std::puts("[wake] This setup scans your Joy-Con 2 on the Pi, captures its HOME advert, then asks you to attach it back to the Switch 2.");
    std::puts("[wake] Use your own Joy-Con 2 right joycon that is paired to this Switch 2.\n");

    std::string mac;
    if (!auto_find_joycon_for_setup(mac))
        return 1;

    std::puts("\n[wake] Step 2/4: Capture the Joy-Con 2 HOME wake advertisement.");
    std::puts("[wake] Keep the Joy-Con 2 very close to the Pi.");
    std::puts("[wake] If pressing HOME wakes the Switch 2, put it back to sleep and retry.");
    std::puts("[wake] It should take about 3-5 tries, this is normal.");

    std::string cap_mac, cap_adv;
    bool captured_home = false;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        std::printf("[wake] HOME capture attempt %d/5.\n", attempt);
        wait_for_enter("[wake] Press Enter, then press HOME on the Joy-Con 2 immediately... ");
        if (capture_switch2_wake_advert(20, mac, cap_mac, cap_adv)) {
            captured_home = true;
            break;
        }
        std::fprintf(stderr, "[wake] Did not catch the HOME advert from %s.\n", mac.c_str());
        std::fprintf(stderr, "[wake] Put the Switch 2 back to sleep if it woke, keep the Joy-Con close to the Pi, and retry.\n");
    }
    if (!captured_home) {
        std::fprintf(stderr, "[wake] Could not capture the HOME advertisement from %s.\n", mac.c_str());
        return 1;
    }

    mac = cap_mac;
    std::printf("[wake] Captured wake MAC: %s\n", mac.c_str());
    std::printf("[wake] Captured wake ADV: %s\n", cap_adv.c_str());

    std::puts("\n[wake] Step 3/4: Attach the Joy-Con 2 to the Switch 2, then suspend the Switch 2.");
    std::puts("[wake] Attach the Joy-Con 2 to the Switch 2 and wait until the console accepts/pairs it again.");
    std::puts("[wake] Then put the Switch 2 to sleep before continuing.");
    wait_for_enter("[wake] Press Enter when the Joy-Con 2 is paired back AND the Switch 2 is asleep; setup will test wake... ");

    if (!save_switch2_wakeup_config(mac, cap_adv, g_switch2_wake_hci_dev))
        return 1;

    g_switch2_wake_mac = lowercase_copy(mac);
    g_switch2_wake_adv_hex = uppercase_hex_copy(cap_adv);
    g_switch2_wake_hci_dev = valid_hci_dev_string(g_switch2_wake_hci_dev) ? g_switch2_wake_hci_dev : "hci0";
    g_switch2_wake_config_loaded = true;
    std::printf("[wake] Saved wake config to %s\n", g_switch2_wakeup_config_path.c_str());

    std::puts("[wake] Step 4/4: Sending test wake advert with MAC spoofing...");
    if (!send_switch2_wake_advert_once(g_switch2_wake_mac, g_switch2_wake_adv_hex, 1, false, true)) {
        std::fprintf(stderr, "[wake] Test wake send failed. Config was saved, but Bluetooth raw HCI send did not complete.\n");
        return 1;
    }

    std::puts("[wake] Test wake advert sent. If the Switch 2 woke up, setup is complete.");
    return 0;
}


bool setup_gadget_builtin(bool force, const char* reason) {
    // Non-forced calls are still cheap/retry-safe for internal recovery paths,
    // but startup passes force=true so the gadget is always torn down and
    // recreated from a known-good state.
    if (!force && hidg_nodes_ready())
        return true;

    if (!force && g_gadget_setup_attempted.exchange(true))
        return hidg_nodes_ready();
    if (force)
        g_gadget_setup_attempted.store(true);

    if (geteuid() != 0) {
        std::fprintf(stderr,
            "[gadget] requested /dev/hidg* nodes are not ready and built-in setup needs root.\n"
            "[gadget] Run: sudo ./ns-backend ...\n");
        return false;
    }

    if (g_verbose)
        std::printf("[gadget] %s; creating built-in %d-interface %s gadget\n",
                    reason ? reason : "HID gadget not ready",
                    HID_PORT_COUNT,
                    g_legacy_mode ? "legacy 8-byte" : "64-byte motion");

    // Try to load and mount configfs.  Ignore failures here because both may
    // already be active on systems that previously used setup_gadget.sh.
    (void)run_shell_best_effort("modprobe libcomposite >/dev/null 2>&1 || true");
    (void)run_shell_best_effort("mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config >/dev/null 2>&1 || true");

    if (!dir_exists("/sys/kernel/config/usb_gadget")) {
        std::fprintf(stderr,
            "[gadget] /sys/kernel/config/usb_gadget is unavailable.\n"
            "[gadget] Check libcomposite/configfs and dtoverlay=dwc2.\n");
        return false;
    }

    // Always remove our previous gadget object before creating a new one when
    // force=true.  This protects normal startup from stale configfs state left
    // by crashes, kill -9, power loss, or older versions of setup_gadget.sh.
    if (path_exists(GADGET_DIR)) {
        if (force || !hidg_nodes_ready()) {
            teardown_gadget();
            std::this_thread::sleep_for(ms(300));
        } else {
            return true;
        }
    }

    if (!mkdir_if_needed(GADGET_DIR)) return false;

    std::string strings_dir = join_path(GADGET_DIR, "strings/0x409");
    std::string configs_dir = join_path(GADGET_DIR, "configs/c.1");
    std::string config_strings_dir = join_path(configs_dir, "strings/0x409");
    std::string functions_dir = join_path(GADGET_DIR, "functions");

    if (!mkdir_if_needed(join_path(GADGET_DIR, "strings").c_str())) return false;
    if (!mkdir_if_needed(strings_dir.c_str())) return false;
    if (!mkdir_if_needed(join_path(GADGET_DIR, "configs").c_str())) return false;
    if (!mkdir_if_needed(configs_dir.c_str())) return false;
    if (g_legacy_mode) {
        if (!mkdir_if_needed(join_path(configs_dir, "strings").c_str())) return false;
        if (!mkdir_if_needed(config_strings_dir.c_str())) return false;
    }
    if (!mkdir_if_needed(functions_dir.c_str())) return false;

    if (!write_text_file(join_path(GADGET_DIR, "bcdDevice").c_str(), g_legacy_mode ? "0x0200" : "0x0210")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bcdUSB").c_str(), "0x0200")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idVendor").c_str(), g_legacy_mode ? "0x0F0D" : "0x057e")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idProduct").c_str(), g_legacy_mode ? "0x0092" : "0x2009")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceClass").c_str(), g_legacy_mode ? "0xFF" : "0x00")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceSubClass").c_str(), g_legacy_mode ? "0xFF" : "0x00")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceProtocol").c_str(), g_legacy_mode ? "0xFF" : "0x00")) return false;

    // USB descriptor serial belongs here, not in the controller SPI area.
    if (!write_text_file(join_path(strings_dir, "serialnumber").c_str(), g_legacy_mode ? "000000000001" : g_usb_serial.c_str())) return false;
    if (!write_text_file(join_path(strings_dir, "manufacturer").c_str(), "NS Bridge")) return false;
    if (!write_text_file(join_path(strings_dir, "product").c_str(), g_legacy_mode ? "Legacy USB Gamepad" : "Motion USB Gamepad")) return false;

    if (!write_text_file(join_path(configs_dir, "MaxPower").c_str(), "500")) return false;
    if (!write_text_file(join_path(configs_dir, "bmAttributes").c_str(), g_legacy_mode ? "0x80" : "0xA0")) return false;
    if (g_legacy_mode) {
        if (!write_text_file(join_path(config_strings_dir, "configuration").c_str(), "USB 4-Player Hub Config")) return false;
    }

    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (!create_hid_function(i)) return false;
    }

    std::string udc = first_udc_name();
    if (udc.empty()) {
        std::fprintf(stderr,
            "[gadget] No UDC found. Check dtoverlay=dwc2 in /boot/config.txt.\n");
        return false;
    }

    if (!write_text_file(join_path(GADGET_DIR, "UDC").c_str(), udc.c_str())) return false;
    if (g_verbose)
        std::printf("[gadget] Bound to UDC: %s\n", udc.c_str());

    // /dev/hidg* can appear shortly after binding.
    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
            if (access(path, F_OK) != 0) all_seen = false;
            chmod(path, 0666);
        }
        if (all_seen && hidg_nodes_ready()) {
            std::printf("[gadget] Done. Exposed %d USB gamepad HID interface(s) (/dev/hidg0..%d)\n", HID_PORT_COUNT, HID_PORT_COUNT - 1);
            return true;
        }
        std::this_thread::sleep_for(ms(100));
    }

    std::fprintf(stderr, "[gadget] setup finished, but requested /dev/hidg* nodes are still not ready.\n");
    return false;
}

bool run_gadget_setup_if_needed(bool force, const char* reason) {
    return setup_gadget_builtin(force, reason);
}

void drain_hid_output_queue(int fd) {

    if (fd < 0) return;

    uint8_t discard[PRO_REPORT_SIZE];
    for (int i = 0; i < 32; ++i) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
        ssize_t r = read(fd, discard, sizeof(discard));
        if (r <= 0) break;
    }
}
