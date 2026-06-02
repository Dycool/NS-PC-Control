#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>

#include <iostream>
#include <chrono>
#include <cstdint>
#include <thread>
#include <algorithm>

#pragma comment(lib, "XInput.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace ns {

static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 2;
static constexpr uint16_t DEFAULT_PORT  = 7331;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,      
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,    
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,  
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,                   
};

enum Hat : uint8_t {
    HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3,
    HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8,
};

#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0;   
    uint8_t  hat     = HAT_NEUTRAL; 
    uint8_t  lx      = 128; 
    uint8_t  ly      = 128; 
    uint8_t  rx      = 128; 
    uint8_t  ry      = 128; 
    uint8_t  vendor  = 0;   
    
    void reset() noexcept { buttons = 0; hat = HAT_NEUTRAL; lx = ly = rx = ry = 128; vendor = 0; }
};

enum Flags : uint8_t { 
    FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02  
};

struct Packet {
    uint32_t  magic;         
    uint8_t   version;       
    uint8_t   flags;         
    uint8_t   controller_id;
    uint8_t   pad;
    uint16_t  autofire_mask; 
    uint32_t  seq;           
    uint64_t  ts_us;         
    HIDReport report;        
};
#pragma pack(pop)

static constexpr size_t PACKET_SIZE = sizeof(Packet);

inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ns

uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) {
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

ns::HIDReport map_xinput_to_switch(const XINPUT_GAMEPAD& pad) {
    ns::HIDReport r;
    r.reset();

    // Map physical layouts (Xbox uses different layout vs Nintendo)
    if (pad.wButtons & XINPUT_GAMEPAD_A) r.buttons |= ns::BTN_B; 
    if (pad.wButtons & XINPUT_GAMEPAD_B) r.buttons |= ns::BTN_A;
    if (pad.wButtons & XINPUT_GAMEPAD_X) r.buttons |= ns::BTN_Y;
    if (pad.wButtons & XINPUT_GAMEPAD_Y) r.buttons |= ns::BTN_X;

    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  r.buttons |= ns::BTN_L;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) r.buttons |= ns::BTN_R;
    
    if (pad.bLeftTrigger > 128)  r.buttons |= ns::BTN_ZL;
    if (pad.bRightTrigger > 128) r.buttons |= ns::BTN_ZR;

    if (pad.wButtons & XINPUT_GAMEPAD_BACK)  r.buttons |= ns::BTN_MINUS;
    if (pad.wButtons & XINPUT_GAMEPAD_START) r.buttons |= ns::BTN_PLUS;
    
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)  r.buttons |= ns::BTN_LSTICK;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) r.buttons |= ns::BTN_RSTICK;

    // Emulate system buttons through combos
    if ((pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }

    if ((pad.wButtons & XINPUT_GAMEPAD_BACK) && (pad.wButtons & XINPUT_GAMEPAD_START)) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS); 
    }

    bool up    = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP);
    bool down  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    bool left  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    bool right = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);

    if (up && right)       r.hat = ns::HAT_NE;
    else if (up && left)   r.hat = ns::HAT_NW;
    else if (down && right)r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up)           r.hat = ns::HAT_N;
    else if (down)         r.hat = ns::HAT_S;
    else if (left)         r.hat = ns::HAT_W;
    else if (right)        r.hat = ns::HAT_E;

    r.lx = apply_deadzone(pad.sThumbLX, false);     
    r.ly = apply_deadzone(pad.sThumbLY, true);      
    r.rx = apply_deadzone(pad.sThumbRX, false);     
    r.ry = apply_deadzone(pad.sThumbRY, true);      

    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP>\n";
        return 1;
    }

    std::string host = argv[1];
    uint16_t port    = ns::DEFAULT_PORT;

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa); 

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;      
    hints.ai_socktype = SOCK_DGRAM; 

    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%u", port);
    
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0) {
        std::cerr << "Failed to resolve target IP address.\n";
        return 1;
    }
    
    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res); 

    std::cout << "Client initialized. Supporting up to 4 XInput controllers.\n";
    std::cout << "Broadcasting to " << host << ":" << port << " ...\n";
    std::cout << "Press CTRL+C to exit.\n";

    uint32_t seq[XUSER_MAX_COUNT] = {0};
    auto last_disconnect_ping = std::chrono::steady_clock::now();

    while (true) {
        auto now = std::chrono::steady_clock::now();
        // Send neutral states for disconnected controllers periodically (500ms)
        // to inform the backend without saturating the network.
        bool send_disconnected = (now - last_disconnect_ping >= std::chrono::milliseconds(500));

        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE state;
            ZeroMemory(&state, sizeof(XINPUT_STATE));
            
            ns::Packet pkt{};
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.controller_id = static_cast<uint8_t>(i);
            pkt.pad           = 0;
            pkt.seq           = seq[i]++;
            pkt.ts_us         = ns::now_us();

            if (XInputGetState(i, &state) == ERROR_SUCCESS) {
                // Controller connected: broadcast actual input state
                pkt.report = map_xinput_to_switch(state.Gamepad);
                sendto(sock, (const char*)&pkt, static_cast<int>(ns::PACKET_SIZE), 0, (const sockaddr*)&dest, sizeof(dest));
            } else if (send_disconnected) {
                // Controller offline: broadcast neutral layout
                pkt.report.reset(); 
                sendto(sock, (const char*)&pkt, static_cast<int>(ns::PACKET_SIZE), 0, (const sockaddr*)&dest, sizeof(dest));
            }
        }

        if (send_disconnected) {
            last_disconnect_ping = now;
        }

        // Maintain ~500Hz polling rate per cycle
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); 
    }

    closesocket(sock);
    WSACleanup(); 
    return 0;
}