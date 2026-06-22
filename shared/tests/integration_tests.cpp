#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"
#include "web_server.hpp"

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
static std::vector<pid_t> g_extra_pids;

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
    for (pid_t pid : g_extra_pids) {
        if (pid > 0) {
            kill(pid, SIGINT);
            int status;
            waitpid(pid, &status, 0);
        }
    }
    g_extra_pids.clear();
}

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

static pid_t spawn_backend(const std::string& backend_bin, uint16_t udp_port, int web_port = -1, bool bt_mode = false) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);

        std::string bind_arg = "127.0.0.1:" + std::to_string(udp_port);
        std::string web_arg = std::to_string(web_port);

        if (web_port > 0) {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(backend_bin.c_str()));
            if (bt_mode) argv.push_back(const_cast<char*>("--bt"));
            argv.push_back(const_cast<char*>("-b"));
            argv.push_back(const_cast<char*>(bind_arg.c_str()));
            argv.push_back(const_cast<char*>("-w"));
            argv.push_back(const_cast<char*>(web_arg.c_str()));
            argv.push_back(nullptr);
            execvp(backend_bin.c_str(), argv.data());
        } else {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(backend_bin.c_str()));
            if (bt_mode) argv.push_back(const_cast<char*>("--bt"));
            argv.push_back(const_cast<char*>("-b"));
            argv.push_back(const_cast<char*>(bind_arg.c_str()));
            argv.push_back(nullptr);
            execvp(backend_bin.c_str(), argv.data());
        }
        std::exit(127);
    }
    return pid;
}

static bool send_probe_and_verify(uint16_t port, int source_fd = -1) {
    int fd = source_fd;
    bool own_fd = false;
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        own_fd = true;
    }

    struct timeval tv{.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    ns::ServerInfoProbe probe{};
    ssize_t sent = sendto(fd, &probe, sizeof(probe), 0, (sockaddr*)&server_addr, sizeof(server_addr));
    if (sent != (ssize_t)sizeof(probe)) { if (own_fd) close(fd); return false; }

    ns::ServerInfoReply reply{};
    socklen_t slen = sizeof(server_addr);
    ssize_t recvd = recvfrom(fd, &reply, sizeof(reply), 0, (sockaddr*)&server_addr, &slen);
    if (own_fd) close(fd);
    return recvd == (ssize_t)sizeof(reply) && reply.magic == ns::SERVER_INFO_MAGIC && reply.version == ns::SERVER_INFO_VERSION;
}

static bool send_signed_packet(int fd, uint16_t port, uint32_t seq, uint8_t flags,
                                const ns::MultiReport& report, const uint8_t hmac_key[32]) {
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    ns::Packet pkt{};
    pkt.magic = ns::PROTO_MAGIC;
    pkt.version = ns::PROTO_VERSION;
    pkt.flags = flags;
    pkt.seq = seq;
    pkt.ts_us = ns::now_us();
    pkt.report = report;

    hmac_sha256(
        std::span<const uint8_t>(hmac_key, 32),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_AUTH_SIZE),
        std::span<uint8_t, 32>(pkt.hmac, 32)
    );

    ssize_t sent = sendto(fd, &pkt, sizeof(pkt), 0, (sockaddr*)&server_addr, sizeof(server_addr));
    return sent == (ssize_t)sizeof(pkt);
}

static bool send_signed_extended_packet(int fd, uint16_t port, uint32_t seq, uint8_t flags,
                                         const ns::ExtendedMultiReport& report, const uint8_t hmac_key[32]) {
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    ExtendedUdpPacket pkt{};
    pkt.magic = ns::PROTO_MAGIC;
    pkt.version = ns::WEB_PROTO_VERSION;
    pkt.flags = flags;
    pkt.seq = seq;
    pkt.timestamp_us = ns::now_us();
    pkt.report = report;

    hmac_sha256(
        std::span<const uint8_t>(hmac_key, 32),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), EXT_UDP_PACKET_AUTH_SIZE),
        std::span<uint8_t, 32>(pkt.hmac, 32)
    );

    ssize_t sent = sendto(fd, &pkt, sizeof(pkt), 0, (sockaddr*)&server_addr, sizeof(server_addr));
    return sent == (ssize_t)sizeof(pkt);
}

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

    write_mock_file("bluetoothctl",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"show\" ]; then\n"
        "  echo \"Controller AA:BB:CC:DD:EE:FF\"\n"
        "  echo \"  Name: test\"\n"
        "  echo \"  Powered: yes\"\n"
        "fi\n"
        "exit 0\n"
    );

    for (const char* tool : {"hcitool", "hciconfig", "rfkill", "systemctl"}) {
        write_mock_file(tool, "#!/bin/sh\nexit 0\n");
    }

    return dir_str;
}

void test_wake_command(const std::string& backend_bin) {
    std::cout << "[test] Spawning " << backend_bin << " -wake with mocked bluetooth tools...\n";

    std::string mock_dir = setup_mock_bluetooth_path();
    std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::string new_path = mock_dir + ":" + original_path;
    setenv("PATH", new_path.c_str(), 1);

    int stdin_pipe[2];
    ASSERT_TRUE(pipe(stdin_pipe) == 0);

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);

        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);

        char* argv[] = {const_cast<char*>(backend_bin.c_str()), const_cast<char*>("-wake"), nullptr};
        execvp(backend_bin.c_str(), argv);
        std::exit(127);
    }

    close(stdin_pipe[0]);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    write(stdin_pipe[1], "\n", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    write(stdin_pipe[1], "\n", 1);
    close(stdin_pipe[1]);

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    setenv("PATH", original_path.c_str(), 1);
    fs::remove_all(mock_dir);

    if (exit_code == 2) {
        std::cout << "[test] -wake test skipped/passed with warning (needs root/sudo to execute fully).\n";
        return;
    }

    ASSERT_EQ(exit_code, 0);

    char* home = std::getenv("HOME");
    ASSERT_TRUE(home != nullptr);
    fs::path config_path = fs::path(home) / ".config" / "ns-pc-control" / "switch2_wake.conf";
    ASSERT_TRUE(fs::exists(config_path));

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

    fs::remove(config_path);

    std::cout << "[test] -wake setup execution check passed.\n";
}

void test_runtime_communication(const std::string& backend_bin, const std::string& client_bin) {
    std::cout << "[test] Starting backend on loopback port 17331...\n";

    g_backend_pid = spawn_backend(backend_bin, 17331, 18080);
    ASSERT_TRUE(g_backend_pid > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    std::cout << "[test] Sending UDP probe to 127.0.0.1:17331...\n";
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(udp_fd >= 0);

    ASSERT_TRUE(send_probe_and_verify(17331, udp_fd));

    std::cout << "[test] Connecting to HTTP Web Server at 127.0.0.1:18080...\n";
    struct timeval tv{.tv_sec = 2, .tv_usec = 0};
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

    std::cout << "[test] Verifying WebSocket Upgrade at ws://127.0.0.1:18080/ws...\n";
    int ws_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(ws_fd >= 0);
    setsockopt(ws_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int ws_conn = connect(ws_fd, (sockaddr*)&http_addr, sizeof(http_addr));
    ASSERT_EQ(ws_conn, 0);

    const char* ws_req = 
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1:18080\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    send(ws_fd, ws_req, strlen(ws_req), 0);

    char ws_buf[1024];
    memset(ws_buf, 0, sizeof(ws_buf));
    ssize_t ws_bytes = recv(ws_fd, ws_buf, sizeof(ws_buf) - 1, 0);
    ASSERT_TRUE(ws_bytes > 0);
    std::string ws_res(ws_buf);
    ASSERT_TRUE(ws_res.find("HTTP/1.1 101") != std::string::npos);
    close(ws_fd);

    std::cout << "[test] Spawning CLI Client " << client_bin << "...\n";
    g_client_pid = fork();
    ASSERT_TRUE(g_client_pid >= 0);

    if (g_client_pid == 0) {
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

    std::cout << "[test] Injecting input packet exchanges over UDP loopback...\n";
    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    ns::MultiReport report{};
    report.p1.buttons = ns::BTN_A;
    report.p1.lx = 50;
    report.p1.ly = 200;

    ASSERT_TRUE(send_signed_packet(udp_fd, 17331, 10, ns::FLAG_NONE, report, hmac_key));

    close(udp_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    cleanup_subprocesses();
    std::cout << "[test] Loopback runtime verification checks passed.\n";
}

void test_multi_client_udp(const std::string& backend_bin) {
    std::cout << "[test] Starting multi-client UDP test on port 17332...\n";

    pid_t pid = spawn_backend(backend_bin, 17332);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    int fds[4];
    for (int i = 0; i < 4; ++i) {
        fds[i] = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_TRUE(fds[i] >= 0);
        struct timeval tv{.tv_sec = 2, .tv_usec = 0};
        setsockopt(fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in src{};
        src.sin_family = AF_INET;
        src.sin_port = htons(30001 + i);
        src.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(fds[i], (sockaddr*)&src, sizeof(src));
    }

    for (int i = 0; i < 4; ++i) {
        ns::MultiReport report{};
        report.p1.buttons = (uint16_t)(1 << i);
        ASSERT_TRUE(send_signed_packet(fds[i], 17332, 1, ns::FLAG_NONE, report, hmac_key));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(send_probe_and_verify(17332, fds[i]));
    }

    for (int i = 0; i < 4; ++i) {
        ns::MultiReport report{};
        report.p1.buttons = (uint16_t)(1 << i);
        ASSERT_TRUE(send_signed_packet(fds[i], 17332, 2, ns::FLAG_NONE, report, hmac_key));
    }

    for (int i = 0; i < 4; ++i) close(fds[i]);

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    std::cout << "[test] Multi-client UDP test passed.\n";
}

void test_client_disconnect_reconnect(const std::string& backend_bin) {
    std::cout << "[test] Starting disconnect/reconnect test on port 17333...\n";

    pid_t pid = spawn_backend(backend_bin, 17333);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct timeval tv{.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = htons(30010);
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, (sockaddr*)&src, sizeof(src));

    ns::MultiReport report{};
    report.p1.buttons = ns::BTN_A;
    ASSERT_TRUE(send_signed_packet(fd, 17333, 1, ns::FLAG_NONE, report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(send_probe_and_verify(17333, fd));

    ns::MultiReport empty_report{};
    ASSERT_TRUE(send_signed_packet(fd, 17333, 2, ns::FLAG_DISCONNECT, empty_report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    report.p1.buttons = ns::BTN_B;
    ASSERT_TRUE(send_signed_packet(fd, 17333, 0, ns::FLAG_RESET, report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(send_probe_and_verify(17333, fd));

    report.p1.buttons = ns::BTN_X;
    ASSERT_TRUE(send_signed_packet(fd, 17333, 1, ns::FLAG_NONE, report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(send_probe_and_verify(17333, fd));

    close(fd);

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    std::cout << "[test] Disconnect/reconnect test passed.\n";
}

void test_macro_udp_submission(const std::string& backend_bin) {
    std::cout << "[test] Starting macro UDP submission test on port 17334...\n";

    pid_t pid = spawn_backend(backend_bin, 17334);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct timeval tv_opt{.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_opt, sizeof(tv_opt));

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = htons(30020);
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, (sockaddr*)&src, sizeof(src));

    ns::MultiReport report{};
    report.p1.buttons = ns::BTN_A;
    ASSERT_TRUE(send_signed_packet(fd, 17334, 1, ns::FLAG_NONE, report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string macro_text = "A 100\nB 200\nWAIT 50";
    uint32_t text_len = (uint32_t)macro_text.size();

    std::vector<uint8_t> macro_pkt(ns::macro::UDP_HEADER_SIZE + text_len + ns::HMAC_TAG_SIZE, 0);

    ns::macro::MacroUdpHeaderWire hdr{};
    hdr.magic = ns::macro::UDP_MAGIC;
    hdr.version = ns::PROTO_VERSION;
    hdr.subpad = 0;
    hdr.text_len = text_len;
    hdr.seq = 1;
    memcpy(macro_pkt.data(), &hdr, sizeof(hdr));
    memcpy(macro_pkt.data() + ns::macro::UDP_HEADER_SIZE, macro_text.data(), text_len);

    size_t auth_len = ns::macro::UDP_HEADER_SIZE + text_len;
    uint8_t full_hmac[32];
    hmac_sha256(
        std::span<const uint8_t>(hmac_key, 32),
        std::span<const uint8_t>(macro_pkt.data(), auth_len),
        std::span<uint8_t, 32>(full_hmac, 32)
    );
    memcpy(macro_pkt.data() + auth_len, full_hmac, ns::HMAC_TAG_SIZE);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(17334);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    ssize_t sent = sendto(fd, macro_pkt.data(), macro_pkt.size(), 0,
                           (sockaddr*)&server_addr, sizeof(server_addr));
    ASSERT_EQ(sent, (ssize_t)macro_pkt.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ASSERT_TRUE(send_probe_and_verify(17334, fd));

    report.p1.buttons = ns::BTN_B;
    ASSERT_TRUE(send_signed_packet(fd, 17334, 2, ns::FLAG_NONE, report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_TRUE(send_probe_and_verify(17334, fd));

    close(fd);

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    std::cout << "[test] Macro UDP submission test passed.\n";
}

void test_websocket_binary_input(const std::string& backend_bin) {
    std::cout << "[test] Starting WebSocket binary input test on port 17335 (web 18085)...\n";

    pid_t pid = spawn_backend(backend_bin, 17335, 18085);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    struct timeval tv_opt{.tv_sec = 3, .tv_usec = 0};

    int ws_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(ws_fd >= 0);
    setsockopt(ws_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_opt, sizeof(tv_opt));
    setsockopt(ws_fd, SOL_SOCKET, SO_SNDTIMEO, &tv_opt, sizeof(tv_opt));

    sockaddr_in ws_addr{};
    ws_addr.sin_family = AF_INET;
    ws_addr.sin_port = htons(18085);
    inet_pton(AF_INET, "127.0.0.1", &ws_addr.sin_addr);

    int conn = connect(ws_fd, (sockaddr*)&ws_addr, sizeof(ws_addr));
    ASSERT_EQ(conn, 0);

    const char* ws_req =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1:18085\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: nspc-protocol\r\n\r\n";
    send(ws_fd, ws_req, strlen(ws_req), 0);

    char hdr_buf[1024];
    memset(hdr_buf, 0, sizeof(hdr_buf));
    ssize_t hdr_bytes = recv(ws_fd, hdr_buf, sizeof(hdr_buf) - 1, 0);
    ASSERT_TRUE(hdr_bytes > 0);
    std::string ws_res(hdr_buf);
    ASSERT_TRUE(ws_res.find("101") != std::string::npos);

    ns::ExtendedMultiReport ws_report{};
    ws_report.p1.input.buttons = ns::BTN_CAPTURE;
    ws_report.p1.input.vendor = ns::EXT_PAD_PRESENT;

    size_t payload_len = ns::WEB_PACKET_SIZE;
    std::vector<uint8_t> payload(payload_len, 0);
    uint32_t magic = ns::PROTO_MAGIC;
    uint8_t version = ns::WEB_PROTO_VERSION;
    uint8_t flags = ns::FLAG_SINGLE_PAD;
    uint32_t seq = 1;
    uint64_t ts = ns::now_us();
    memcpy(payload.data(), &magic, 4);
    payload[4] = version;
    payload[5] = flags;
    memcpy(payload.data() + 8, &seq, 4);
    memcpy(payload.data() + 12, &ts, 8);
    memcpy(payload.data() + 20, &ws_report, sizeof(ws_report));

    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    std::vector<uint8_t> frame;
    frame.push_back(0x82);

    if (payload_len < 126) {
        frame.push_back((uint8_t)(0x80 | payload_len));
    } else {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)(payload_len >> 8));
        frame.push_back((uint8_t)(payload_len & 0xFF));
    }
    frame.insert(frame.end(), mask_key, mask_key + 4);
    for (size_t i = 0; i < payload_len; ++i) {
        frame.push_back(payload[i] ^ mask_key[i % 4]);
    }

    ssize_t ws_sent = send(ws_fd, frame.data(), frame.size(), 0);
    ASSERT_TRUE(ws_sent == (ssize_t)frame.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ASSERT_TRUE(send_probe_and_verify(17335));

    close(ws_fd);

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    std::cout << "[test] WebSocket binary input test passed.\n";
}

void test_flag_reset(const std::string& backend_bin) {
    std::cout << "[test] Starting FLAG_RESET test on port 17336...\n";

    pid_t pid = spawn_backend(backend_bin, 17336);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(fd >= 0);
    struct timeval tv_opt{.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_opt, sizeof(tv_opt));

    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_port = htons(30030);
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, (sockaddr*)&src, sizeof(src));

    ns::MultiReport report{};
    report.p1.buttons = ns::BTN_A;
    for (uint32_t seq = 0; seq < 10; ++seq) {
        ASSERT_TRUE(send_signed_packet(fd, 17336, seq, ns::FLAG_NONE, report, hmac_key));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(send_probe_and_verify(17336, fd));

    ns::MultiReport reset_report{};
    reset_report.p1.buttons = ns::BTN_B;
    ASSERT_TRUE(send_signed_packet(fd, 17336, 0, ns::FLAG_RESET, reset_report, hmac_key));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    for (uint32_t seq = 1; seq < 5; ++seq) {
        report.p1.buttons = ns::BTN_X;
        ASSERT_TRUE(send_signed_packet(fd, 17336, seq, ns::FLAG_NONE, report, hmac_key));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(send_probe_and_verify(17336, fd));

    close(fd);

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    std::cout << "[test] FLAG_RESET test passed.\n";
}

void test_bluetooth_runtime_mode(const std::string& backend_bin) {
    std::cout << "[test] Starting Bluetooth runtime mode test on port 17337...\n";

    std::string mock_dir = setup_mock_bluetooth_path();
    std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    std::string new_path = mock_dir + ":" + original_path;
    setenv("PATH", new_path.c_str(), 1);

    pid_t pid = spawn_backend(backend_bin, 17337, -1, true);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    ASSERT_TRUE(send_probe_and_verify(17337));

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    setenv("PATH", original_path.c_str(), 1);
    fs::remove_all(mock_dir);

    std::cout << "[test] Bluetooth runtime mode test passed.\n";
}

void test_service_mode_only_udp(const std::string& backend_bin) {
    std::cout << "[test] Starting service mode (UDP-only, no web) test on port 17338...\n";

    pid_t pid = spawn_backend(backend_bin, 17338);
    ASSERT_TRUE(pid > 0);
    g_extra_pids.push_back(pid);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    ASSERT_TRUE(send_probe_and_verify(17338));

    kill(pid, SIGINT);
    int status;
    waitpid(pid, &status, 0);
    g_extra_pids.clear();

    std::cout << "[test] Service mode (UDP-only) test passed.\n";
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

    test_multi_client_udp(backend_bin);
    test_client_disconnect_reconnect(backend_bin);
    test_macro_udp_submission(backend_bin);
    test_websocket_binary_input(backend_bin);
    test_flag_reset(backend_bin);

    test_bluetooth_runtime_mode(backend_bin);
    test_service_mode_only_udp(backend_bin);

    std::cout << "========================================\n";
    std::cout << "ALL 9 INTEGRATION TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}
