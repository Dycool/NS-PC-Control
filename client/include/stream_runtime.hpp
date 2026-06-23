#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"
#include "shared/sdl_input.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <expected>

extern std::atomic<bool> g_connected;
extern std::thread g_senderThread;
extern std::atomic<bool> g_senderRunning;
extern uint8_t g_hmacKey[32];
extern std::atomic<uint32_t> g_packetCount;
extern std::mutex g_statusMutex;
extern std::string g_statusMessage;
extern std::string g_lastError;

void set_status_message(const std::string& s);
std::string status_message();
bool parse_host_port(std::string in, std::string& host, int& port);
void raise_process_priority();
void raise_sender_priority();

struct ClientFrame {
    ns::HoriHIDReport reports[4];
    ns::MotionReport motion[4][3];
    bool present[4] = {false, false, false, false};
    bool has_motion[4] = {false, false, false, false};
    int controller_for_slot[4] = {-1, -1, -1, -1};
    int battery_percent[4] = {-1, -1, -1, -1};
    bool battery_charging[4] = {false, false, false, false};
    int active_count = 0;

    void reset();
};

struct ClientStreamConfig {
    std::string host;
    int port = ns::DEFAULT_PORT;
    bool gui_features = false;
    bool print_cli_waiting_messages = false;
    int idle_sleep_ms = 50;
    const uint8_t* hmac_key = nullptr;
};

void build_client_frame(ClientFrame& frame,
                        DigitalReleaseFilter filters[4],
                        bool send_motion,
                        int keyboard_mode);
void send_client_frame(SOCKET sock,
                       const sockaddr_in& dest,
                       const uint8_t hmac_key[32],
                       uint32_t& seq,
                       const ClientFrame& frame);
int run_client_stream(const ClientStreamConfig& cfg,
                      std::atomic<bool>& running,
                      std::string* err_out = nullptr);
void sender_thread_main(std::atomic<bool>& running, std::string host, uint16_t port);
std::expected<void, std::string> start_connection(const std::string& target);
void stop_connection();

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    bool good() const;

private:
    bool ok = false;
};
