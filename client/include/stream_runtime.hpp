#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"
#include "shared/sdl_input.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <array>
#include <thread>
#include <expected>
#include <QByteArray>
#include <QString>

extern std::atomic<bool> g_connected;
extern std::atomic<bool> g_connecting;
extern std::thread g_senderThread;
extern std::atomic<bool> g_senderRunning;
extern uint8_t g_hmacKey[32];
extern std::atomic<uint32_t> g_packetCount;
extern std::mutex g_statusMutex;
extern std::string g_statusMessage;
extern std::string g_lastError;

struct ServerAssignmentView {
    bool accepted = false;
    bool server_full = false;
    uint8_t server_slot = ns::CONTROLLER_PLAYER_INDEX_UNKNOWN;
    uint8_t active_clients = 0;
    uint8_t max_clients = 4;
    uint8_t free_virtual_slots = 0;
    uint8_t console_port_mask[4]{};
    uint8_t primary_console_port[4]{ns::CONTROLLER_CONSOLE_PORT_NONE, ns::CONTROLLER_CONSOLE_PORT_NONE, ns::CONTROLLER_CONSOLE_PORT_NONE, ns::CONTROLLER_CONSOLE_PORT_NONE};
    uint8_t requested_type[4]{};
    uint8_t virtual_type[4]{};
    uint64_t last_update_us = 0;
};

extern std::mutex g_assignmentMutex;
extern ServerAssignmentView g_serverAssignment;
void reset_server_assignment_state();
void handle_client_assignment_packet(const ns::ClientAssignmentPacket& packet);
ServerAssignmentView server_assignment_snapshot();

struct RosterView {
    bool valid = false;
    ns::RosterEntry ports[4]{};
    uint64_t last_update_us = 0;
};
extern std::mutex g_rosterMutex;
extern RosterView g_roster;
void reset_roster_state();
void handle_roster_packet(const ns::RosterPacket& packet);
RosterView roster_snapshot();

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
    bool motion_sample_fresh[4] = {false, false, false, false};
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
extern std::atomic<bool> g_amiiboScanPending[4];
extern std::atomic<uint16_t> g_amiiboRequestSequence[4];
extern std::atomic<uint64_t> g_amiiboScanDeadlineUs[4];
extern QString g_amiiboPaths[4];

void sendAmiiboData(uint8_t subpad, const QByteArray& data);
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
