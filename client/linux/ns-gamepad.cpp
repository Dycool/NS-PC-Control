/// ns-gamepad.cpp  —  Linux frontend for the Switch wireless gamepad bridge
///
/// Uses SDL2 GameController API for cross-platform controller support.
/// Automatically detects Xbox, PlayStation, and generic controllers
/// via USB or Bluetooth — no raw joystick API needed.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread -lSDL2
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]> [-k [single|override]]

#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>
#include <csignal>
#include <string>
#include <vector>
#include <unordered_map>

#include <SDL2/SDL.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include "../../server/rpi/include/sha256.h"

// Import external protocol structures (Version 4 with MultiReport)
#include "../../server/rpi/include/protocol.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Shared gamepad state (SDL2)
// ─────────────────────────────────────────────────────────────────────────────

static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static int keyboard_mode = 0; // 0=off, 1=single, 2=override
static SDL_Window* g_kb_window = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

/// Convert raw analog stick value to normalized 0-255 range with deadzone applied
uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;

    int scaled;
    if (val >= deadzone) {
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SDL2 Discovery & Input
// ─────────────────────────────────────────────────────────────────────────────

void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();

    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    static bool no_controllers_printed = false;
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (!SDL_IsGameController(i)) continue;

        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool found = false;
        for (int p = 0; p < 4; ++p) {
            if (g_pads[p]) {
                SDL_Joystick* js = SDL_GameControllerGetJoystick(g_pads[p]);
                if (js && SDL_JoystickInstanceID(js) == id) {
                    found = true;
                    break;
                }
            }
        }
        if (found) continue;

        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                SDL_GameController* pad = SDL_GameControllerOpen(i);
                if (!pad) break;
                g_pads[p] = pad;
                const char* name = SDL_GameControllerName(pad);
                
                int slot = p + 1;
                if (keyboard_mode == 1 && p == 0) {
                    if (!g_pads[1]) slot = 2;
                    else if (!g_pads[2]) slot = 3;
                    else slot = 4;
                }
                
                std::cout << "Mapped '" << (name ? name : "Unknown") << "' to local slot P" << slot << "\n";
                break;
            }
        }
    }
    
    if (num == 0 && keyboard_mode == 0) {
        if (!no_controllers_printed) {
            std::cout << "No controllers detected — waiting for connections...\n";
            no_controllers_printed = true;
        }
    } else {
        no_controllers_printed = false;
    }
}

void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        std::cout << "Controller in slot P" << (index + 1) << " disconnected.\n";
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        conn = false;
        return;
    }

    conn = true;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) rep.buttons |= ns::BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) rep.buttons |= ns::BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) rep.buttons |= ns::BTN_Y;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) rep.buttons |= ns::BTN_X;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) rep.buttons |= ns::BTN_L;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) rep.buttons |= ns::BTN_R;

    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16000) rep.buttons |= ns::BTN_ZL;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) rep.buttons |= ns::BTN_ZR;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))    rep.buttons |= ns::BTN_MINUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))   rep.buttons |= ns::BTN_PLUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))  rep.buttons |= ns::BTN_LSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) rep.buttons |= ns::BTN_RSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE))   rep.buttons |= ns::BTN_HOME;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
        rep.buttons |= ns::BTN_HOME; rep.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
        rep.buttons |= ns::BTN_CAPTURE; rep.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
    }

    bool up    = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    rep.hat = ns::HAT_NEUTRAL;
    if (up && right) rep.hat = ns::HAT_NE; else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE; else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N; else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W; else if (right) rep.hat = ns::HAT_E;

    int16_t lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    int16_t rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);

    rep.lx = apply_deadzone(lx, false);
    rep.ly = apply_deadzone(ly, false);
    rep.rx = apply_deadzone(rx, false);
    rep.ry = apply_deadzone(ry, false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Keyboard Binding Support
// ─────────────────────────────────────────────────────────────────────────────
struct KeyBindings {
    std::unordered_map<std::string, std::string> map;
    int mode = 0; 

    static std::unordered_map<std::string, std::string> defaults() {
        return {
            {"Y","Z"}, {"B","X"}, {"A","V"}, {"X","C"},
            {"L","Q"}, {"R","E"}, {"ZL","1"}, {"ZR","2"},
            {"MINUS","3"}, {"PLUS","4"},
            {"LSTICK","Left Shift"}, {"RSTICK","Right Shift"},
            {"HOME","Home"}, {"CAPTURE","PrintScreen"},
            {"LSTICK_UP","W"}, {"LSTICK_DOWN","S"},
            {"LSTICK_LEFT","A"}, {"LSTICK_RIGHT","D"},
            {"RSTICK_UP","I"}, {"RSTICK_DOWN","K"},
            {"RSTICK_LEFT","J"}, {"RSTICK_RIGHT","L"},
            {"DPAD_UP","Up"}, {"DPAD_DOWN","Down"},
            {"DPAD_LEFT","Left"}, {"DPAD_RIGHT","Right"}
        };
    }

    std::string get_bindings_path() const {
        char buf[1024];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            std::string p(buf);
            size_t pos = p.find_last_of("/");
            return (pos != std::string::npos ? p.substr(0, pos) : ".") + "/bindings.json";
        }
        return "bindings.json";
    }

    void load_or_create() {
        std::string path = get_bindings_path();
        FILE* f = fopen(path.c_str(), "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0) {
                std::string content((size_t)len, '\0');
                fread(&content[0], 1, (size_t)len, f);
                size_t pos = 0;
                while ((pos = content.find('"', pos)) != std::string::npos) {
                    size_t ks = pos + 1, ke = content.find('"', ks);
                    if (ke == std::string::npos) break;
                    std::string k = content.substr(ks, ke - ks);
                    pos = content.find('"', ke + 1);
                    if (pos == std::string::npos) break;
                    size_t vs = pos + 1, ve = content.find('"', vs);
                    if (ve == std::string::npos) break;
                    map[k] = content.substr(vs, ve - vs);
                    pos = ve + 1;
                }
            }
            fclose(f);
        }
        if (map.empty()) {
            map = defaults();
            f = fopen(path.c_str(), "w");
            if (f) {
                std::string json = "{\n";
                size_t i = 0;
                for (auto& [k, v] : map) {
                    json += "    \"" + k + "\": \"" + v + "\"";
                    if (++i < map.size()) json += ",";
                    json += "\n";
                }
                json += "}\n";
                fputs(json.c_str(), f);
                fclose(f);
            }
            std::cout << "Created default bindings: " << path << "\n";
        }
    }

    void apply(ns::HIDReport& rep) const {
        int numkeys;
        const Uint8* state = SDL_GetKeyboardState(&numkeys);

        auto is_down = [&](const std::string& name) -> bool {
            if (name.empty()) return false;
            SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) {
                // Fallbacks for standard names
                if (name == "LSHIFT" || name == "Left Shift") sc = SDL_SCANCODE_LSHIFT;
                else if (name == "RSHIFT" || name == "Right Shift") sc = SDL_SCANCODE_RSHIFT;
                else if (name == "UP" || name == "Up") sc = SDL_SCANCODE_UP;
                else if (name == "DOWN" || name == "Down") sc = SDL_SCANCODE_DOWN;
                else if (name == "LEFT" || name == "Left") sc = SDL_SCANCODE_LEFT;
                else if (name == "RIGHT" || name == "Right") sc = SDL_SCANCODE_RIGHT;
                else if (name == "SNAPSHOT" || name == "PrintScreen") sc = SDL_SCANCODE_PRINTSCREEN;
            }
            if (sc != SDL_SCANCODE_UNKNOWN && sc < numkeys) return state[sc];
            return false;
        };

        auto get_key = [&](const std::string& btn) -> std::string {
            auto it = map.find(btn);
            return it != map.end() ? it->second : "";
        };

        std::string k;
        k = get_key("Y");      if (is_down(k)) rep.buttons |= ns::BTN_Y;
        k = get_key("B");      if (is_down(k)) rep.buttons |= ns::BTN_B;
        k = get_key("A");      if (is_down(k)) rep.buttons |= ns::BTN_A;
        k = get_key("X");      if (is_down(k)) rep.buttons |= ns::BTN_X;
        k = get_key("L");      if (is_down(k)) rep.buttons |= ns::BTN_L;
        k = get_key("R");      if (is_down(k)) rep.buttons |= ns::BTN_R;
        k = get_key("ZL");     if (is_down(k)) rep.buttons |= ns::BTN_ZL;
        k = get_key("ZR");     if (is_down(k)) rep.buttons |= ns::BTN_ZR;
        k = get_key("MINUS");  if (is_down(k)) rep.buttons |= ns::BTN_MINUS;
        k = get_key("PLUS");   if (is_down(k)) rep.buttons |= ns::BTN_PLUS;
        k = get_key("LSTICK"); if (is_down(k)) rep.buttons |= ns::BTN_LSTICK;
        k = get_key("RSTICK"); if (is_down(k)) rep.buttons |= ns::BTN_RSTICK;
        k = get_key("HOME");   if (is_down(k)) rep.buttons |= ns::BTN_HOME;
        k = get_key("CAPTURE"); if (is_down(k)) rep.buttons |= ns::BTN_CAPTURE;

        bool up = false, down = false, left = false, right = false;
        k = get_key("DPAD_UP");    if (!k.empty()) up    = is_down(k);
        k = get_key("DPAD_DOWN");  if (!k.empty()) down  = is_down(k);
        k = get_key("DPAD_LEFT");  if (!k.empty()) left  = is_down(k);
        k = get_key("DPAD_RIGHT"); if (!k.empty()) right = is_down(k);

        if (up && right) rep.hat = ns::HAT_NE;
        else if (up && left) rep.hat = ns::HAT_NW;
        else if (down && right) rep.hat = ns::HAT_SE;
        else if (down && left) rep.hat = ns::HAT_SW;
        else if (up) rep.hat = ns::HAT_N;
        else if (down) rep.hat = ns::HAT_S;
        else if (left) rep.hat = ns::HAT_W;
        else if (right) rep.hat = ns::HAT_E;

        auto lsu = get_key("LSTICK_UP"), lsd = get_key("LSTICK_DOWN");
        auto lsl = get_key("LSTICK_LEFT"), lsr = get_key("LSTICK_RIGHT");
        bool lsu_dn = is_down(lsu), lsd_dn = is_down(lsd);
        bool lsl_dn = is_down(lsl), lsr_dn = is_down(lsr);
        if (lsl_dn && !lsr_dn) rep.lx = 0; else if (lsr_dn && !lsl_dn) rep.lx = 255; else if (mode != 2) rep.lx = 128;
        if (lsu_dn && !lsd_dn) rep.ly = 0; else if (lsd_dn && !lsu_dn) rep.ly = 255; else if (mode != 2) rep.ly = 128;

        auto rsu = get_key("RSTICK_UP"), rsd = get_key("RSTICK_DOWN");
        auto rsl = get_key("RSTICK_LEFT"), rsr = get_key("RSTICK_RIGHT");
        bool rsu_dn = is_down(rsu), rsd_dn = is_down(rsd);
        bool rsl_dn = is_down(rsl), rsr_dn = is_down(rsr);
        if (rsl_dn && !rsr_dn) rep.rx = 0; else if (rsr_dn && !rsl_dn) rep.rx = 255; else if (mode != 2) rep.rx = 128;
        if (rsu_dn && !rsd_dn) rep.ry = 0; else if (rsd_dn && !rsu_dn) rep.ry = 255; else if (mode != 2) rep.ry = 128;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]]\n";
        return 1;
    }

    std::string host;
    int port = ns::DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            keyboard_mode = 1;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                if (strcmp(argv[i+1], "override") == 0) keyboard_mode = 2;
                i++;
            }
        } else if (host.empty()) {
            host = argv[i];
            size_t colon = host.find(':');
            if (colon != std::string::npos) {
                port = std::atoi(host.c_str() + colon + 1);
                if (port < 1 || port > 65535) {
                    std::cerr << "Invalid port: " << port << " (must be 1–65535)\n";
                    return 1;
                }
                host.resize(colon);
            }
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k]\n";
        return 1;
    }

    KeyBindings kb;
    if (keyboard_mode) {
        kb.load_or_create();
        kb.mode = keyboard_mode;
        std::cout << "Keyboard mode enabled (" << (keyboard_mode == 1 ? "single" : "override") << ") - ";
        std::cout << (keyboard_mode == 1 ? "replaces" : "augments") << " Player 1\n";
    }

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        return 1;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock); return 1;
    }
    
    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    // Initialise SDL2 (Video required on Linux to capture keyboard safely)
    Uint32 sdl_flags = SDL_INIT_GAMECONTROLLER;
    if (keyboard_mode > 0) sdl_flags |= SDL_INIT_VIDEO;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(sdl_flags) < 0) {
        std::cerr << "Failed to initialise SDL2: " << SDL_GetError() << "\n";
        close(sock); return 1;
    }

    if (keyboard_mode > 0) {
        g_kb_window = SDL_CreateWindow("NS PC Control - KB Focus", 
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                       320, 100, SDL_WINDOW_SHOWN);
        std::cout << "\n*** WARNING: To use the keyboard, keep the new SDL Window focused! ***\n\n";
    }

    std::cout << "Started... Press Ctrl+C to stop\n";

    setpriority(PRIO_PROCESS, 0, -20);

    uint32_t seq = 0;
    auto next_tick = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        
        while (std::chrono::steady_clock::now() < next_tick) {
            std::atomic_thread_fence(std::memory_order_relaxed);
        }

        // Pump SDL events
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_running.store(false);
        }
        SDL_GameControllerUpdate();

        scan_for_gamepads();

        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic         = ns::PROTO_MAGIC;
        pkt.version       = ns::PROTO_VERSION;
        pkt.flags         = ns::FLAG_NONE;
        pkt.seq           = seq++;
        pkt.ts_us         = ns::now_us();
        
        pkt.report.reset(); 

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0;

        bool c1 = false, c2 = false, c3 = false, c4 = false;
        for (int i = 0; i < 4; ++i) {
            bool is_conn = false;
            read_pad(i, *out_reports[i], is_conn);
            if (is_conn) {
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
            }
        }

        if (kb.mode == 1) {
            if (c1) {
                if (!c2) { *out_reports[1] = *out_reports[0]; c2 = true; active_count++; }
                else if (!c3) { *out_reports[2] = *out_reports[0]; c3 = true; active_count++; }
                else if (!c4) { *out_reports[3] = *out_reports[0]; c4 = true; active_count++; }
            }
            out_reports[0]->reset();
            kb.apply(*out_reports[0]);
            active_count = std::max(active_count, 1);
        } else if (kb.mode == 2) {
            kb.apply(*out_reports[0]);
            active_count = std::max(active_count, 1);
        }

        {
            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        
        if (active_count > 0) next_tick += std::chrono::milliseconds(4);
        else next_tick += std::chrono::milliseconds(500);
    }

    std::cout << "\nShutting down...\n";
    for (int i = 0; i < 4; ++i) {
        if (g_pads[i]) { SDL_GameControllerClose(g_pads[i]); g_pads[i] = nullptr; }
    }
    if (g_kb_window) SDL_DestroyWindow(g_kb_window);
    SDL_Quit();
    close(sock);
    return 0;
}