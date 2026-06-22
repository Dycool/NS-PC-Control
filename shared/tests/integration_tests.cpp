#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <fstream>
#include <filesystem>
#include <system_error>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>

namespace fs = std::filesystem;

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Integration Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
            cleanup_subprocesses(); \
            std::exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(val1, val2) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << "Integration Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " \
                      << #val1 << " == " << #val2 << " (" << (val1) << " != " << (val2) << ")\n"; \
            cleanup_subprocesses(); \
            std::exit(1); \
        } \
    } while (0)

static pid_t g_backend_pid = -1;
static pid_t g_client_pid = -1;

void cleanup_subprocesses() {
    if (g_client_pid > 0) {
        std::cout << "[teardown] Terminating client process (PID " << g_client_pid << ")...\n";
        kill(g_client_pid, SIGINT);
        int status;
        waitpid(g_client_pid, &status, 0);
        g_client_pid = -1;
    }
    if (g_backend_pid > 0) {
        std::cout << "[teardown] Terminating backend process (PID " << g_backend_pid << ")...\n";
        kill(g_backend_pid, SIGINT);
        int status;
        waitpid(g_backend_pid, &status, 0);
        g_backend_pid = -1;
    }
}

// Find binary relative to current dir, or check env
std::string get_backend_path() {
    const char* env = std::getenv("NS_BACKEND_BIN");
    if (env && *env) return env;
    std::vector<std::string> candidates = {
        "./ns-backend",
        "../server/build/ns-backend",
        "../../server/build/ns-backend",
        "./server/build/ns-backend"
    };
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return "";
}

std::string get_client_path() {
    const char* env = std::getenv("NS_CLIENT_BIN");
    if (env && *env) return env;
    std::vector<std::string> candidates = {
        "./ns-client",
        "../client/build/ns-client",
        "../../client/build/ns-client",
        "./client/build/ns-client",
        "../client/build/ns-client.app/Contents/MacOS/ns-client",
        "../../client/build/ns-client.app/Contents/MacOS/ns-client",
        "./client/build/ns-client.app/Contents/MacOS/ns-client"
    };
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return "";
}

// Write mock bluetooth tools into a temp directory and add to PATH
std::string setup_mock_bluetooth_path() {
    char template_path[] = "/tmp/ns_mocks_XXXXXX";
    char* temp_dir = mkdtemp(template_path);
    ASSERT_TRUE(temp_dir != nullptr);
    std::string dir_str(temp_dir);

    auto write_mock_file = [&](const std::string& name, const std::string& script) {
        std::string path = dir_str + "/" + name;
        std::ofstream f(path);
        f << script;
        f.close();
        chmod(path.c_str(), 0755);
    };

    write_mock_file("btmgmt", 
        "#!/bin/sh\n"
        "if [ \"$1\" = \"info\" ]; then\n"
        "  echo \"hci0: Primary Controller\"\n"
        "  echo \"      Address: AA:BB:CC:DD:EE:FF\"\n"
        "fi\n"
        "exit 0\n"
    );

    write_mock_file("btmon",
        "#!/bin/sh\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in\n"
        "    *ns_switch2_wake_*)\n"
        "      echo \"address: 00:09:bf:ab:cd:ef\" > \"$arg\"\n"
        "      echo \"data[24]: 11223344556677889900aabbccddeeff1122334455667788\" >> \"$arg\"\n"
        "      ;;\n"
        "  esac\n"
        "done\n"
        "exit 0\n"
    );

    for (const char* tool : {"hcitool", "hciconfig", "rfkill", "systemctl"}) {
        write_mock_file(tool, "#!/bin/sh\nexit 0\n");
    }

    return dir_str;
}

void test_wake_command(const std::string& backend_bin) {
    std::cout << "[test] Spawning " << backend_bin << " -wake with mocked bluetooth tools...\n";

    // Set up mock path
    std::string mock_dir = setup_mock_bluetooth_path();
    std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::string new_path = mock_dir + ":" + original_path;
    setenv("PATH", new_path.c_str(), 1);

    // Create pipes to feed stdin
    int stdin_pipe[2];
    ASSERT_TRUE(pipe(stdin_pipe) == 0);

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        // Child
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);

        // Redirect stdout/stderr to silence it
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);

        char* argv[] = {const_cast<char*>(backend_bin.c_str()), const_cast<char*>("-wake"), nullptr};
        execvp(backend_bin.c_str(), argv);
        std::exit(127);
    }

    // Parent
    close(stdin_pipe[0]);

    // Feed the two required interactive Enters with short delays
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    write(stdin_pipe[1], "\n", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    write(stdin_pipe[1], "\n", 1);
    close(stdin_pipe[1]);

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    // Restore path
    setenv("PATH", original_path.c_str(), 1);
    fs::remove_all(mock_dir);

    // If we're not root, the wake tool might exit with 2 (Warning bypass)
    if (exit_code == 2) {
        std::cout << "[test] -wake test skipped/passed with warning (needs root/sudo to execute fully).\n";
        return;
    }

    ASSERT_EQ(exit_code, 0);

    // Verify config file was written correctly
    char* home = std::getenv("HOME");
    ASSERT_TRUE(home != nullptr);
    fs::path config_path = fs::path(home) / ".config" / "ns-pc-control" / "switch2_wake.conf";
    ASSERT_TRUE(fs::exists(config_path));

    // Verify content of config
    std::ifstream f(config_path);
    std::string line;
    bool found_mac = false, found_adv = false;
    while (std::getline(f, line)) {
        if (line.find("mac=00:09:bf:ab:cd:ef") != std::string::npos) found_mac = true;
        if (line.find("adv=0201061BFF530511223344556677889900AABBCCDDEEFF1122334455667788") != std::string::npos) found_adv = true;
    }
    f.close();

    ASSERT_TRUE(found_mac);
    ASSERT_TRUE(found_adv);

    // Clean up config file after verification
    fs::remove(config_path);

    std::cout << "[test] -wake setup execution check passed.\n";
}

void test_runtime_communication(const std::string& backend_bin, const std::string& client_bin) {
    std::cout << "[test] Starting backend on loopback port 17331...\n";

    // 1. Start Backend
    g_backend_pid = fork();
    ASSERT_TRUE(g_backend_pid >= 0);

    if (g_backend_pid == 0) {
        // Child
        // Redirect outputs to silence
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);

        char* argv[] = {
            const_cast<char*>(backend_bin.c_str()), 
            const_cast<char*>("-b"), const_cast<char*>("127.0.0.1:17331"),
            const_cast<char*>("-w"), const_cast<char*>("18080"),
            nullptr
        };
        execvp(backend_bin.c_str(), argv);
        std::exit(127);
    }

    // Wait for server socket initialization
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 2. Validate Server Info Probe via UDP Socket
    std::cout << "[test] Sending UDP probe to 127.0.0.1:17331...\n";
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(udp_fd >= 0);

    // Set recv timeout
    struct timeval tv{.tv_sec = 2, .tv_usec = 0};
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(17331);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    ns::ServerInfoProbe probe{};
    ssize_t sent = sendto(udp_fd, &probe, sizeof(probe), 0, (sockaddr*)&server_addr, sizeof(server_addr));
    ASSERT_EQ(sent, (ssize_t)sizeof(probe));

    ns::ServerInfoReply reply{};
    socklen_t slen = sizeof(server_addr);
    ssize_t recvd = recvfrom(udp_fd, &reply, sizeof(reply), 0, (sockaddr*)&server_addr, &slen);
    ASSERT_EQ(recvd, (ssize_t)sizeof(reply));
    ASSERT_EQ(reply.magic, ns::SERVER_INFO_MAGIC);
    ASSERT_EQ(reply.version, ns::SERVER_INFO_VERSION);

    // 3. Verify HTTP Web Server responding with 200 OK
    std::cout << "[test] Connecting to HTTP Web Server at 127.0.0.1:18080...\n";
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(tcp_fd >= 0);
    setsockopt(tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in http_addr{};
    http_addr.sin_family = AF_INET;
    http_addr.sin_port = htons(18080);
    inet_pton(AF_INET, "127.0.0.1", &http_addr.sin_addr);

    int conn = connect(tcp_fd, (sockaddr*)&http_addr, sizeof(http_addr));
    ASSERT_EQ(conn, 0);

    const char* req = "GET / HTTP/1.1\r\nHost: 127.0.0.1:18080\r\nConnection: close\r\n\r\n";
    send(tcp_fd, req, strlen(req), 0);

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t http_bytes = recv(tcp_fd, buf, sizeof(buf) - 1, 0);
    ASSERT_TRUE(http_bytes > 0);
    std::string http_res(buf);
    ASSERT_TRUE(http_res.find("HTTP/1.1 200") != std::string::npos);
    close(tcp_fd);

    // 4. Spawn CLI Client
    std::cout << "[test] Spawning CLI Client " << client_bin << "...\n";
    g_client_pid = fork();
    ASSERT_TRUE(g_client_pid >= 0);

    if (g_client_pid == 0) {
        // Child
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);

        char* argv[] = {
            const_cast<char*>(client_bin.c_str()),
            const_cast<char*>("--cli"), const_cast<char*>("127.0.0.1:17331"),
            const_cast<char*>("-k"), const_cast<char*>("single"),
            nullptr
        };
        execvp(client_bin.c_str(), argv);
        std::exit(127);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 5. Send authenticated input packets
    std::cout << "[test] Injecting input packet exchanges over UDP loopback...\n";
    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    ns::Packet packet{};
    packet.seq = 10;
    packet.report.p1.buttons = ns::BTN_A;
    packet.report.p1.lx = 50;
    packet.report.p1.ly = 200;
    
    // Sign the packet
    hmac_sha256(
        std::span<const uint8_t>(hmac_key, 32), 
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&packet), ns::PACKET_AUTH_SIZE), 
        std::span<uint8_t, 32>(packet.hmac, 32)
    );

    sent = sendto(udp_fd, &packet, sizeof(packet), 0, (sockaddr*)&server_addr, sizeof(server_addr));
    ASSERT_EQ(sent, (ssize_t)sizeof(packet));

    close(udp_fd);

    // Give it a brief moment
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 6. Cleanup subprocesses
    cleanup_subprocesses();
    std::cout << "[test] Loopback runtime verification checks passed.\n";
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    std::cout << "========================================\n";
    std::cout << "Starting NS-PC-Control Integration Tests\n";
    std::cout << "========================================\n";

    std::string backend_bin = get_backend_path();
    std::string client_bin = get_client_path();

    if (backend_bin.empty() || client_bin.empty()) {
        std::cerr << "ERROR: ns-backend (" << (backend_bin.empty() ? "missing" : "found")
                  << ") or ns-client (" << (client_bin.empty() ? "missing" : "found")
                  << ") not found in target directories.\n";
        return 1;
    }

    std::cout << "[info] Found ns-backend: " << backend_bin << "\n";
    std::cout << "[info] Found ns-client: " << client_bin << "\n";

    test_wake_command(backend_bin);
    test_runtime_communication(backend_bin, client_bin);

    std::cout << "========================================\n";
    std::cout << "ALL INTEGRATION TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}
