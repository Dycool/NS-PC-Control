#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"
#include "shared/sdl_input.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

extern std::atomic<bool> g_connected;
extern std::jthread g_senderThread;
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
    ns::HIDReport reports[4];
    ns::MotionReport motion[4][3];
    bool present[4] = {false, false, false, false};
    bool has_motion[4] = {false, false, false, false};
    int controller_for_slot[4] = {-1, -1, -1, -1};
    int active_count = 0;

    void reset();
};

struct ClientStreamConfig {
    std::string host;
    int port = ns::DEFAULT_PORT;
    bool force_legacy_udp = false;
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
                       bool legacy_packet,
                       const ClientFrame& frame);
int run_client_stream(const ClientStreamConfig& cfg,
                      std::stop_token stoken,
                      std::string* err_out = nullptr);
void sender_thread_main(std::stop_token stoken, std::string host, uint16_t port, bool legacy_udp);
bool start_connection(const std::string& target, std::string* err_out = nullptr);
void stop_connection();

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    bool good() const;

private:
    bool ok = false;
};
