#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <condition_variable>
#include <netinet/tcp.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

// ── Shared Protocol and Configurations ───────────────────────────────────────
namespace ns {
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 4;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr size_t HMAC_TAG_SIZE = 16;
static constexpr int WATCHDOG_MS = 2000;
static constexpr int WRITER_HZ = 60;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};
enum Hat : uint8_t { HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8 };
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };

#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0; 
    uint8_t hat = HAT_NEUTRAL;
    uint8_t lx=128, ly=128, rx=128, ry=128, vendor=0;
    
    void reset() noexcept { *this = HIDReport{}; }
    bool operator!=(const HIDReport& o) const noexcept {
        return buttons != o.buttons || hat != o.hat || lx != o.lx || ly != o.ly || rx != o.rx || ry != o.ry;
    }
};
struct MultiReport {
    HIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
};
struct Packet {
    uint32_t magic; 
    uint8_t version; 
    uint8_t flags; 
    uint16_t autofire_mask;
    uint32_t seq; 
    uint64_t ts_us; 
    MultiReport report; 
    uint8_t hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)

static constexpr size_t PACKET_SIZE = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace ns

using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Simple HMAC SHA-256 for validation (Pure C++ Implementation) ─────────────
// (Avoids external dependency bindings on raw Raspberry Pi)
static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (data[i * 4] << 24) | (data[i * 4 + 1] << 16) | (data[i * 4 + 2] << 8) | data[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
        uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_hash(const uint8_t* msg, size_t len, uint8_t digest[32]) {
    uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::vector<uint8_t> block(64, 0);
    size_t i = 0, rem = len;
    while (rem >= 64) {
        sha256_transform(state, msg + i); i += 64; rem -= 64;
    }
    memcpy(block.data(), msg + i, rem);
    block[rem] = 0x80;
    if (rem >= 56) {
        sha256_transform(state, block.data());
        memset(block.data(), 0, 64);
    }
    uint64_t total_bits = len * 8;
    for (int b = 0; b < 8; ++b) {
        block[63 - b] = (total_bits >> (b * 8)) & 0xFF;
    }
    sha256_transform(state, block.data());
    for (int b = 0; b < 8; ++b) {
        digest[b * 4]     = (state[b] >> 24) & 0xFF;
        digest[b * 4 + 1] = (state[b] >> 16) & 0xFF;
        digest[b * 4 + 2] = (state[b] >> 8) & 0xFF;
        digest[b * 4 + 3] = state[b] & 0xFF;
    }
}

static void hmac_sha256_verify(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]) {
    uint8_t k_ipad[64], k_opad[64];
    memset(k_ipad, 0, 64); memset(k_opad, 0, 64);
    if (key_len > 64) {
        uint8_t temp_key[32];
        sha256_hash(key, key_len, temp_key);
        memcpy(k_ipad, temp_key, 32); memcpy(k_opad, temp_key, 32);
    } else {
        memcpy(k_ipad, key, key_len); memcpy(k_opad, key, key_len);
    }
    for (int i = 0; i < 64; ++i) {
        k_ipad[i] ^= 0x36; k_opad[i] ^= 0x5c;
    }
    std::vector<uint8_t> inner(64 + data_len);
    memcpy(inner.data(), k_ipad, 64);
    if (data_len > 0) {
        memcpy(inner.data() + 64, data, data_len);
    }
    uint8_t inner_hash[32];
    sha256_hash(inner.data(), inner.size(), inner_hash);

    uint8_t outer[64 + 32];
    memcpy(outer, k_opad, 64);
    memcpy(outer + 64, inner_hash, 32);
    sha256_hash(outer, 96, out);
}

// ── WebSocket Helper Utilities ───────────────────────────────────────────────
static std::string base64_encode(const unsigned char* input, size_t length) {
    static const char char_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((length + 2) / 3) * 4);
    union { uint8_t bytes[4]; uint32_t value; } block;
    for (size_t i = 0; i < length; i += 3) {
        size_t count = std::min(length - i, (size_t)3);
        block.value = 0;
        for (size_t j = 0; j < count; ++j) {
            block.bytes[2 - j] = input[i + j];
        }
        output += char_set[(block.value >> 18) & 0x3F];
        output += char_set[(block.value >> 12) & 0x3F];
        output += (count > 1) ? char_set[(block.value >> 6) & 0x3F] : '=';
        output += (count > 2) ? char_set[block.value & 0x3F] : '=';
    }
    return output;
}

static void sha1_hash(const std::string& input, unsigned char hash[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::vector<uint8_t> buf(input.begin(), input.end());
    uint64_t bits = buf.size() * 8;
    buf.push_back(0x80);
    while ((buf.size() * 8) % 512 != 448) {
        buf.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        buf.push_back((bits >> (i * 8)) & 0xFF);
    }
    for (size_t chunk = 0; chunk < buf.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (buf[chunk + i * 4] << 24) | (buf[chunk + i * 4 + 1] << 16) | (buf[chunk + i * 4 + 2] << 8) | buf[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t val = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (val << 1) | (val >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    hash[0] = (h0 >> 24) & 0xFF; hash[1] = (h0 >> 16) & 0xFF; hash[2] = (h0 >> 8) & 0xFF; hash[3] = h0 & 0xFF;
    hash[4] = (h1 >> 24) & 0xFF; hash[5] = (h1 >> 16) & 0xFF; hash[6] = (h1 >> 8) & 0xFF; hash[7] = h1 & 0xFF;
    hash[8] = (h2 >> 24) & 0xFF; hash[9] = (h2 >> 16) & 0xFF; hash[10] = (h2 >> 8) & 0xFF; hash[11] = h2 & 0xFF;
    hash[12] = (h3 >> 24) & 0xFF; hash[13] = (h3 >> 16) & 0xFF; hash[14] = (h3 >> 8) & 0xFF; hash[15] = h3 & 0xFF;
    hash[16] = (h4 >> 24) & 0xFF; hash[17] = (h4 >> 16) & 0xFF; hash[18] = (h4 >> 8) & 0xFF; hash[19] = h4 & 0xFF;
}

static std::string generate_ws_accept_key(const std::string& client_key) {
    unsigned char hash[20];
    sha1_hash(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", hash);
    return base64_encode(hash, 20);
}

static std::string get_embedded_html() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Nintendo Switch Multi-Controller Web-GUI</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <script src="https://unpkg.com/lucide@latest"></script>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap');
        body { font-family: 'Inter', sans-serif; }
        .mono { font-family: 'JetBrains Mono', monospace; }
        .controller-glow { box-shadow: 0 0 15px rgba(139, 92, 246, 0.15); }
    </style>
</head>
<body class="bg-slate-950 text-slate-100 min-h-screen flex flex-col justify-between selection:bg-violet-500 selection:text-white">

    <!-- Header -->
    <header class="border-b border-slate-800 bg-slate-900/60 backdrop-blur-md px-6 py-4 flex flex-wrap justify-between items-center gap-4 sticky top-0 z-50">
        <div class="flex items-center gap-3">
            <div class="w-10 h-10 rounded-xl bg-gradient-to-tr from-violet-600 to-indigo-600 flex items-center justify-center font-bold text-white tracking-widest shadow-md shadow-violet-900/20">
                NS
            </div>
            <div>
                <h1 class="font-bold tracking-tight text-lg text-white">Nintendo Switch Controller Web-GUI</h1>
                <p class="text-xs text-slate-400 font-medium">Standalone High-Performance C++ Web Receiver</p>
            </div>
        </div>
        
        <div class="flex items-center gap-3 text-sm">
            <div class="flex items-center gap-2 bg-slate-800/80 px-3 py-1.5 rounded-lg border border-slate-700">
                <span class="w-2 h-2 rounded-full bg-emerald-500 animate-pulse"></span>
                <span id="ppsVal" class="text-xs text-slate-300 font-mono font-medium">0 PPS</span>
            </div>
            <div id="statusBadge" class="flex items-center gap-2 bg-slate-800/80 px-3 py-1.5 rounded-lg border border-slate-700 transition-all duration-300">
                <span id="statusDot" class="w-2 h-2 rounded-full bg-red-500"></span>
                <span id="statusText" class="text-xs font-semibold text-slate-400">Disconnected</span>
            </div>
        </div>
    </header>

    <!-- Main Content Grid -->
    <main class="max-w-7xl w-full mx-auto px-6 py-8 flex-1 grid grid-cols-1 lg:grid-cols-12 gap-8">
        
        <!-- Left Side: Controls & Bindings -->
        <section class="lg:col-span-5 flex flex-col gap-6">
            
            <!-- Connection & Settings Card -->
            <div class="bg-slate-900 border border-slate-800 rounded-xl p-5 shadow-sm flex flex-col gap-4">
                <h2 class="font-semibold text-white flex items-center gap-2 text-md">
                    <i data-lucide="settings" class="w-4 h-4 text-violet-400"></i> Settings & Handshake
                </h2>
                
                <div class="grid grid-cols-1 sm:grid-cols-2 gap-4">
                    <div>
                        <label class="block text-xs text-slate-400 font-medium mb-1">Secret Key / Private Password</label>
                        <input id="secretKey" type="password" value="nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY" 
                               class="w-full bg-slate-950 border border-slate-800 rounded-lg px-3 py-2 text-sm text-slate-300 focus:outline-none focus:border-violet-500 font-mono transition">
                    </div>
                    <div>
                        <label class="block text-xs text-slate-400 font-medium mb-1">Keyboard Mode</label>
                        <select id="kbMode" class="w-full bg-slate-950 border border-slate-800 rounded-lg px-3 py-2 text-sm text-slate-300 focus:outline-none focus:border-violet-500 font-medium transition">
                            <option value="0">Keyboard: Disabled</option>
                            <option value="1">Keyboard: Single Board P1</option>
                            <option value="2">Keyboard: Override P1</option>
                        </select>
                    </div>
                </div>

                <button id="connectBtn" class="w-full mt-2 bg-gradient-to-r from-violet-600 to-indigo-600 hover:from-violet-500 hover:to-indigo-500 text-white font-medium py-2.5 rounded-lg transition active:scale-[0.98] shadow-md shadow-violet-900/10">
                    Establish Host Websocket
                </button>
            </div>

            <!-- Key Bindings Manager Card -->
            <div class="bg-slate-900 border border-slate-800 rounded-xl p-5 shadow-sm flex flex-col gap-4">
                <div class="flex justify-between items-center bg-slate-900">
                    <h2 class="font-semibold text-white flex items-center gap-2 text-md">
                        <i data-lucide="key" class="w-4 h-4 text-violet-400"></i> Custom Mappings
                    </h2>
                    <button id="openWizardBtn" class="text-xs bg-slate-800 hover:bg-slate-700 text-violet-400 hover:text-white border border-slate-700 hover:border-violet-500 px-3 py-1 rounded-md transition font-medium flex items-center gap-1">
                        <i data-lucide="sparkles" class="w-3.5 h-3.5"></i> Setup Wizard
                    </button>
                </div>

                <!-- Custom Scroller list of key bindings -->
                <div class="max-h-80 overflow-y-auto border border-slate-850 rounded-lg bg-slate-950 pr-1 divide-y divide-slate-850">
                    <div id="bindingsList" class="p-3 grid grid-cols-2 sm:grid-cols-3 gap-2">
                        <!-- Key bind elements loaded from JavaScript -->
                    </div>
                </div>

                <div class="flex gap-2">
                    <button id="resetBindingsBtn" class="flex-1 bg-slate-800 hover:bg-slate-700 text-slate-300 border border-slate-700 py-1.5 rounded-lg transition text-xs font-semibold">
                        Reset to Defaults
                    </button>
                    <button id="saveBindingsBtn" class="flex-1 bg-violet-600 hover:bg-violet-500 text-white py-1.5 rounded-lg transition text-xs font-semibold">
                        Save Configuration
                    </button>
                </div>
            </div>

        </section>

        <!-- Right Side: Beautiful Controller Dashboard Visualizers -->
        <section class="lg:col-span-7 flex flex-col gap-6">
            
            <div class="bg-slate-900 border border-slate-800 rounded-xl p-6 shadow-sm flex flex-col gap-4">
                <div class="flex justify-between items-center bg-slate-900">
                    <h2 class="font-semibold text-white flex items-center gap-2 text-md">
                        <i data-lucide="gamepad" class="w-4 h-4 text-violet-400"></i> Active Virtual Dashboard
                    </h2>
                    <div class="text-xs text-slate-400 font-mono">Ports multiplexing state</div>
                </div>

                <!-- 4 Player HUD Grid -->
                <div class="grid grid-cols-1 sm:grid-cols-2 gap-4">
                    
                    <!-- Player 1 -->
                    <div class="bg-slate-950 border border-slate-850 rounded-lg p-4 flex flex-col gap-3 relative overflow-hidden transition hover:border-violet-500/30">
                        <div class="flex items-center justify-between">
                            <div class="flex items-center gap-2">
                                <span class="w-2.5 h-2.5 rounded-full bg-cyan-400 shadow shadow-cyan-400"></span>
                                <span class="font-bold text-sm text-slate-200">PORT 1 (P1)</span>
                            </div>
                            <span id="p1Status" class="text-[10px] bg-slate-800 px-2 py-0.5 rounded text-slate-400 font-mono font-semibold">NEUTRAL</span>
                        </div>
                        <div class="flex justify-between items-center gap-2">
                            <!-- Sticks display -->
                            <div class="flex gap-4">
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p1LStick" class="absolute w-3 h-3 rounded-full bg-cyan-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">L</span>
                                </div>
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p1RStick" class="absolute w-3 h-3 rounded-full bg-violet-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">R</span>
                                </div>
                            </div>
                            <!-- Buttons list -->
                            <div id="p1Btns" class="flex flex-wrap gap-1 justify-end max-w-28 text-[9px] font-bold">
                                <!-- Dynamic badges of pressed buttons -->
                            </div>
                        </div>
                    </div>

                    <!-- Player 2 -->
                    <div class="bg-slate-950 border border-slate-850 rounded-lg p-4 flex flex-col gap-3 relative overflow-hidden transition hover:border-red-500/30">
                        <div class="flex items-center justify-between">
                            <div class="flex items-center gap-2">
                                <span class="w-2.5 h-2.5 rounded-full bg-red-500 shadow shadow-red-500"></span>
                                <span class="font-bold text-sm text-slate-200">PORT 2 (P2)</span>
                            </div>
                            <span id="p2Status" class="text-[10px] bg-slate-800 px-2 py-0.5 rounded text-slate-400 font-mono font-semibold">NEUTRAL</span>
                        </div>
                        <div class="flex justify-between items-center gap-2">
                            <!-- Sticks display -->
                            <div class="flex gap-4">
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p2LStick" class="absolute w-3 h-3 rounded-full bg-red-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">L</span>
                                </div>
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p2RStick" class="absolute w-3 h-3 rounded-full bg-red-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">R</span>
                                </div>
                            </div>
                            <!-- Buttons list -->
                            <div id="p2Btns" class="flex flex-wrap gap-1 justify-end max-w-28 text-[9px] font-bold">
                                <!-- Dynamic badges of pressed buttons -->
                            </div>
                        </div>
                    </div>

                    <!-- Player 3 -->
                    <div class="bg-slate-950 border border-slate-850 rounded-lg p-4 flex flex-col gap-3 relative overflow-hidden transition hover:border-yellow-500/30">
                        <div class="flex items-center justify-between">
                            <div class="flex items-center gap-2">
                                <span class="w-2.5 h-2.5 rounded-full bg-yellow-400 shadow shadow-yellow-500"></span>
                                <span class="font-bold text-sm text-slate-200">PORT 3 (P3)</span>
                            </div>
                            <span id="p3Status" class="text-[10px] bg-slate-800 px-2 py-0.5 rounded text-slate-400 font-mono font-semibold">NEUTRAL</span>
                        </div>
                        <div class="flex justify-between items-center gap-2">
                            <!-- Sticks display -->
                            <div class="flex gap-4">
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p3LStick" class="absolute w-3 h-3 rounded-full bg-yellow-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">L</span>
                                </div>
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p3RStick" class="absolute w-3 h-3 rounded-full bg-yellow-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">R</span>
                                </div>
                            </div>
                            <!-- Buttons list -->
                            <div id="p3Btns" class="flex flex-wrap gap-1 justify-end max-w-28 text-[9px] font-bold">
                                <!-- Dynamic badges of pressed buttons -->
                            </div>
                        </div>
                    </div>

                    <!-- Player 4 -->
                    <div class="bg-slate-950 border border-slate-850 rounded-lg p-4 flex flex-col gap-3 relative overflow-hidden transition hover:border-emerald-500/30">
                        <div class="flex items-center justify-between">
                            <div class="flex items-center gap-2">
                                <span class="w-2.5 h-2.5 rounded-full bg-emerald-500 shadow shadow-emerald-500"></span>
                                <span class="font-bold text-sm text-slate-200">PORT 4 (P4)</span>
                            </div>
                            <span id="p4Status" class="text-[10px] bg-slate-800 px-2 py-0.5 rounded text-slate-400 font-mono font-semibold">NEUTRAL</span>
                        </div>
                        <div class="flex justify-between items-center gap-2">
                            <!-- Sticks display -->
                            <div class="flex gap-4">
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p4LStick" class="absolute w-3 h-3 rounded-full bg-emerald-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">L</span>
                                </div>
                                <div class="relative w-14 h-14 rounded-full bg-slate-900 border border-slate-800 flex items-center justify-center">
                                    <div id="p4RStick" class="absolute w-3 h-3 rounded-full bg-emerald-400 left-1/2 top-1/2 transform -translate-x-1/2 -translate-y-1/2 shadow transition"></div>
                                    <span class="text-[8px] text-slate-500 font-bold tracking-widest pointer-events-none">R</span>
                                </div>
                            </div>
                            <!-- Buttons list -->
                            <div id="p4Btns" class="flex flex-wrap gap-1 justify-end max-w-28 text-[9px] font-bold">
                                <!-- Dynamic badges of pressed buttons -->
                            </div>
                        </div>
                    </div>

                </div>

                <!-- Web Gamepad Detection -->
                <div class="mt-4 border-t border-slate-800 pt-4 grid grid-cols-2 shadow-sm rounded gap-3 bg-slate-900/40 p-3 text-xs">
                    <div class="flex items-center gap-2 text-slate-300">
                        <i data-lucide="info" class="w-4 h-4 text-sky-400"></i>
                        <span>Press any controller button in the browser to log gamepad connections dynamically.</span>
                    </div>
                    <div id="gamepadConnectState" class="text-right text-slate-400 font-semibold flex items-center justify-end gap-1.5 font-mono">
                        No native gamepad detected
                    </div>
                </div>
            </div>

        </section>

    </main>

    <!-- Modal dialogue: Setup Sequence Wizard dialog -->
    <div id="wizardModal" class="fixed inset-0 bg-black/80 backdrop-blur-sm flex items-center justify-center z-50 pointer-events-none opacity-0 transform scale-95 transition-all duration-300">
        <div class="bg-slate-900 border border-slate-800 max-w-md w-full mx-4 rounded-xl p-6 shadow-xl flex flex-col gap-5">
            <div class="flex justify-between items-start">
                <div>
                    <h3 class="text-lg font-bold text-white tracking-tight flex items-center gap-2">
                        <i data-lucide="sparkles" class="w-5 h-5 text-violet-400"></i> Sequential Bindings Wizard
                    </h3>
                    <p class="text-xs text-slate-400">Press the requested key to program the controller layout cleanly.</p>
                </div>
                <button id="closeWizardBtn" class="p-1 rounded bg-slate-800 hover:bg-slate-700 transition">
                    <i data-lucide="x" class="w-4 h-4 text-slate-400 hover:text-white"></i>
                </button>
            </div>
            
            <!-- Calibration Status Pane -->
            <div class="bg-slate-950 rounded-lg p-6 border border-slate-850 flex flex-col items-center justify-center text-center gap-3">
                <div class="text-xs text-slate-500 font-semibold tracking-wider uppercase">Currently calibrating</div>
                <div id="wizardActionBtnName" class="text-2xl font-bold text-violet-400 animate-pulse tracking-wide">BUTTON_A</div>
                <div class="text-[11px] text-slate-400">
                    Press any keyboard key, or <kbd class="bg-slate-800 px-1.5 py-0.5 rounded text-[10px] text-white">Esc</kbd> to leave slot empty.
                </div>
            </div>

            <!-- Steps Progress list -->
            <div class="flex items-center justify-between text-xs font-semibold text-slate-400">
                <span id="wizardStepCount">Step 1 of 25</span>
                <span class="text-slate-500">Anti-collision fully active</span>
            </div>
        </div>
    </div>

    <!-- Minimalistic Page footer bar -->
    <footer class="border-t border-slate-800 py-4 px-6 text-center text-xs text-slate-500 bg-slate-950">
        Nintendo Switch Controller Server WebApp &copy; 2026. Standalone Single-executable C++ Edition.
    </footer>

    <!-- JS Client Logics and binary sockets generators -->
    <script>
        // Icons binding
        lucide.createIcons();

        // Constants
        const DEFAULT_BINDINGS = {
            A: "V", B: "X", X: "C", Y: "Z",
            L: "Q", R: "E", ZL: "1", ZR: "2",
            MINUS: "3", PLUS: "4", LSTICK: "LSHIFT", RSTICK: "RSHIFT",
            HOME: "HOME", CAPTURE: "SNAPSHOT",
            LSTICK_UP: "W", LSTICK_DOWN: "S", LSTICK_LEFT: "A", LSTICK_RIGHT: "D",
            RSTICK_UP: "I", RSTICK_DOWN: "K", RSTICK_LEFT: "J", RSTICK_RIGHT: "L",
            DPAD_UP: "UP", DPAD_DOWN: "DOWN", DPAD_LEFT: "LEFT", DPAD_RIGHT: "RIGHT"
        };
        
        const BINDING_KEYS_ORDER = [
            "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "MINUS", "PLUS", "LSTICK", "RSTICK", "HOME",
            "LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT",
            "RSTICK_UP", "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT",
            "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "CAPTURE"
        ];

        const BUTTON_BIT_MAP = {
            Y: 1<<0, B: 1<<1, A: 1<<2, X: 1<<3,
            L: 1<<4, R: 1<<5, ZL: 1<<6, ZR: 1<<7,
            MINUS: 1<<8, PLUS: 1<<9, LSTICK: 1<<10, RSTICK: 1<<11,
            HOME: 1<<12, CAPTURE: 1<<13
        };

        const LOCAL_BINDINGS_KEY = "ns_webgui_custom_bindings";
        const LOCAL_SECRET_KEY = "ns_webgui_secret_key";
        const LOCAL_KBMODE_KEY = "ns_webgui_kb_mode";

        // Global States
        let customBindings = { ...DEFAULT_BINDINGS };
        let ws = null;
        let wsConnected = false;
        let pps = 0;
        let ppsCounter = 0;
        let seq = 0;
        let activeKeys = new Set();
        let hmacKeyDigest = null;
        let pollTimer = null;
        let wizardIndex = null;
        let isModalOpen = false;

        // Load Persistent configurations
        try {
            const savedBinds = localStorage.getItem(LOCAL_BINDINGS_KEY);
            if (savedBinds) customBindings = JSON.parse(savedBinds);
            
            const savedSec = localStorage.getItem(LOCAL_SECRET_KEY);
            if (savedSec) document.getElementById("secretKey").value = savedSec;
            
            const savedMode = localStorage.getItem(LOCAL_KBMODE_KEY);
            if (savedMode !== null) document.getElementById("kbMode").value = savedMode;
        } catch (e) {
            console.error("Local load failed:", e);
        }

        // Initialize/Render Bindings layout list in GUI
        function renderBindingsTable() {
            const listContainer = document.getElementById("bindingsList");
            listContainer.innerHTML = "";
            for (const key of BINDING_KEYS_ORDER) {
                const mapVal = customBindings[key] || "NONE";
                const div = document.createElement("div");
                div.className = "flex flex-col gap-1 bg-slate-900/60 p-2 rounded border border-slate-800/80";
                div.innerHTML = `
                    <span class="text-[9px] font-bold text-slate-500 uppercase tracking-wide">${key}</span>
                    <button id="btnMap-${key}" onclick="initManualKeyBind('${key}')" class="text-left font-mono text-xs px-2 py-1 rounded bg-slate-950 border border-slate-850 hover:border-violet-500 text-violet-400 truncate hover:text-white transition">
                        ${mapVal == " " ? "SPACE" : mapVal}
                    </button>
                `;
                listContainer.appendChild(div);
            }
        }
        renderBindingsTable();

        // Synchronous Keys listeners to feed fast loop inputs safely
        function getEventKeyStr(e) {
            let keyStr = e.key.toUpperCase();
            if (e.key === " ") keyStr = " ";
            if (e.key === "ArrowUp") keyStr = "UP";
            if (e.key === "ArrowDown") keyStr = "DOWN";
            if (e.key === "ArrowLeft") keyStr = "LEFT";
            if (e.key === "ArrowRight") keyStr = "RIGHT";
            if (e.code === "PrintScreen") keyStr = "SNAPSHOT";
            return keyStr;
        }

        window.addEventListener("keydown", (e) => {
            if (isModalOpen) return;
            const key = getEventKeyStr(e);
            activeKeys.add(key);
            
            // Block standard viewport keys mapped by user
            const mappedValues = Object.values(customBindings).map(k => k.toUpperCase());
            if (mappedValues.includes(key) || ["ARROWUP", "ARROWDOWN", "ARROWLEFT", "ARROWRIGHT", " "].includes(e.key.toUpperCase())) {
                e.preventDefault();
            }
        });

        window.addEventListener("keyup", (e) => {
            if (isModalOpen) return;
            const key = getEventKeyStr(e);
            activeKeys.delete(key);
        });

        function isKeyPressed(btnToken) {
            const binding = customBindings[btnToken];
            if (!binding) return false;
            return activeKeys.has(binding.toUpperCase());
        }

        // Manual button selection binding logic
        let listeningBindKey = null;
        function initManualKeyBind(key) {
            listeningBindKey = key;
            const btn = document.getElementById(`btnMap-${key}`);
            btn.textContent = "Listening...";
            btn.className = "text-left font-mono text-xs px-2 py-1 rounded bg-slate-950 border border-amber-500 text-amber-500 truncate transition";
            
            const captureOne = (e) => {
                e.preventDefault();
                e.stopPropagation();
                let pressed = getEventKeyStr(e);
                if (e.key === "Escape") pressed = "";
                
                customBindings[listeningBindKey] = pressed;
                renderBindingsTable();
                window.removeEventListener("keydown", captureOne, true);
                listeningBindKey = null;
            };
            window.addEventListener("keydown", captureOne, true);
        }

        // Sequential Wizard calibration logic
        const openWizardBtn = document.getElementById("openWizardBtn");
        const closeWizardBtn = document.getElementById("closeWizardBtn");
        const wizardModal = document.getElementById("wizardModal");
        const wizardActionBtnName = document.getElementById("wizardActionBtnName");
        const wizardStepCount = document.getElementById("wizardStepCount");

        openWizardBtn.onclick = () => {
            wizardIndex = 0;
            isModalOpen = true;
            wizardModal.classList.remove("pointer-events-none", "opacity-0", "scale-95");
            wizardModal.classList.add("opacity-100", "scale-100");
            updateWizardStep();
            
            window.addEventListener("keydown", captureWizardKey, true);
        };

        function closeWizard() {
            wizardIndex = null;
            isModalOpen = false;
            wizardModal.classList.add("pointer-events-none", "opacity-0", "scale-95");
            wizardModal.classList.remove("opacity-100", "scale-100");
            window.removeEventListener("keydown", captureWizardKey, true);
            renderBindingsTable();
        }
        closeWizardBtn.onclick = closeWizard;

        function updateWizardStep() {
            if (wizardIndex === null || wizardIndex >= BINDING_KEYS_ORDER.length) {
                closeWizard(); return;
            }
            const keyName = BINDING_KEYS_ORDER[wizardIndex];
            wizardActionBtnName.textContent = `BUTTON_` + keyName;
            wizardStepCount.textContent = `Step ${wizardIndex + 1} of ${BINDING_KEYS_ORDER.length}`;
        }

        function captureWizardKey(e) {
            e.preventDefault();
            e.stopPropagation();
            
            let pressed = getEventKeyStr(e);
            if (e.key === "Escape") pressed = ""; // clear binding
            
            const keyName = BINDING_KEYS_ORDER[wizardIndex];
            customBindings[keyName] = pressed;
            
            wizardIndex++;
            updateWizardStep();
        }

        // Web Crypto API Key compilation
        async function recompileHmacKey() {
            const secretText = document.getElementById("secretKey").value;
            localStorage.setItem(LOCAL_SECRET_KEY, secretText);
            
            const encoder = new TextEncoder();
            const secretBytes = encoder.encode(secretText);
            
            // Subkey standard derivation (SHA-256 of the text key)
            const digest = await window.crypto.subtle.digest("SHA-256", secretBytes);
            
            // Import SHA-256 digested subkey for raw binary signing
            hmacKeyDigest = await window.crypto.subtle.importKey(
                "raw",
                digest,
                { name: "HMAC", hash: "SHA-256" },
                false,
                ["sign"]
            );
        }

        document.getElementById("secretKey").onchange = recompileHmacKey;
        recompileHmacKey();

        // WebSocket core setups
        const connectBtn = document.getElementById("connectBtn");
        const statusBadge = document.getElementById("statusBadge");
        const statusDot = document.getElementById("statusDot");
        const statusText = document.getElementById("statusText");

        function setStatus(connState, msg) {
            wsConnected = connState;
            statusText.textContent = msg;
            if (connState) {
                statusDot.className = "w-2 h-2 rounded-full bg-emerald-500 animate-pulse";
                statusBadge.className = "flex items-center gap-2 bg-emerald-900/20 border border-emerald-500 px-3 py-1.5 rounded-lg text-emerald-400";
                connectBtn.textContent = "Close Active Sync Socket";
                connectBtn.className = "w-full mt-2 bg-gradient-to-r from-red-600 to-rose-600 hover:from-red-500 hover:to-rose-500 text-white font-medium py-2.5 rounded-lg shadow-md hover:shadow-red-900/10";
            } else {
                statusDot.className = "w-2 h-2 rounded-full bg-red-500";
                statusBadge.className = "flex items-center gap-2 bg-slate-800/80 px-3 py-1.5 rounded-lg border border-slate-700 text-slate-400";
                connectBtn.textContent = "Establish Host Websocket";
                connectBtn.className = "w-full mt-2 bg-gradient-to-r from-violet-600 to-indigo-600 hover:from-violet-500 hover:to-indigo-500 text-white font-medium py-2.5 rounded-lg shadow-md";
                document.getElementById("ppsVal").textContent = "0 PPS";
            }
        }

        connectBtn.onclick = () => {
            if (ws) {
                ws.close();
                ws = null;
                return;
            }
            
            const isHttps = window.location.protocol === "https:";
            const proto = isHttps ? "wss:" : "ws:";
            // Standard relative hostname address mapping
            const wsUrl = `${proto}//${window.location.host}/ws`;
            
            statusText.textContent = "Upgrading Connection...";
            statusDot.className = "w-2 h-2 rounded-full bg-amber-500 animate-spin";
            
            ws = new WebSocket(wsUrl);
            ws.binaryType = "arraybuffer";
            
            ws.onopen = () => {
                setStatus(true, "Synchronized");
                seq = 0;
            };
            ws.onclose = () => {
                setStatus(false, "Disconnected");
                ws = null;
            };
            ws.onerror = (e) => {
                setStatus(false, "Socket Interrupted");
                console.error("WS error:", e);
                ws = null;
            };
        };

        // Gamepad dynamic API poll updates
        window.addEventListener("gamepadconnected", () => {
            document.getElementById("gamepadConnectState").innerHTML = `<i data-lucide="check" class="w-4 h-4 text-emerald-400"></i> Local Gamepad Bound`;
            lucide.createIcons();
        });
        window.addEventListener("gamepaddisconnected", () => {
            const gps = navigator.getGamepads();
            if (!gps[0] && !gps[1]) {
                document.getElementById("gamepadConnectState").textContent = "No native gamepad detected";
            }
        });

        // 60Hz high speed ticking generator loop
        setInterval(() => {
            const nowTime = Date.now();
            ppsCounter++;
            
            // 4 reports
            const defaultRep = () => ({ buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 });
            let reports = { p1: defaultRep(), p2: defaultRep(), p3: defaultRep(), p4: defaultRep() };
            
            const gps = navigator.getGamepads();
            const padConnected = [false, false, false, false];
            
            // Poll standard gamepads
            for (let i = 0; i < 4; i++) {
                if (gps[i]) {
                    padConnected[i] = true;
                    const polled = pollGamepad(i);
                    if (polled) {
                        if (i === 0) reports.p1 = polled;
                        else if (i === 1) reports.p2 = polled;
                        else if (i === 2) reports.p3 = polled;
                        else if (i === 3) reports.p4 = polled;
                    }
                }
            }

            // Keyboard Mode override layer logic from settings
            const mode = Number(document.getElementById("kbMode").value);
            localStorage.setItem(LOCAL_KBMODE_KEY, mode);

            if (mode === 1) { // Single Board P1
                // Shift Gamepad 1 over to Player 2, Gamepad 2 to Player 3 if slot free
                if (padConnected[0]) {
                    if (!padConnected[1]) { reports.p2 = { ...reports.p1 }; padConnected[1] = true; }
                    else if (!padConnected[2]) { reports.p3 = { ...reports.p1 }; padConnected[2] = true; }
                    else if (!padConnected[3]) { reports.p4 = { ...reports.p1 }; padConnected[3] = true; }
                }
                
                reports.p1 = defaultRep();
                applyKeyboardLayer(reports.p1);
                padConnected[0] = true;
            } else if (mode === 2) { // Override P1 keys
                applyKeyboardLayer(reports.p1);
                padConnected[0] = true;
            }

            // Update user HUD panels in HTML real-time
            renderHudPort(1, reports.p1, padConnected[0]);
            renderHudPort(2, reports.p2, padConnected[1]);
            renderHudPort(3, reports.p3, padConnected[2]);
            renderHudPort(4, reports.p4, padConnected[3]);

            if (ws && wsConnected) {
                transmitCompressedReportBin(reports);
            }
        }, 16.6); // ~60Hz loop ticker

        // PPS display refresh clock
        setInterval(() => {
            document.getElementById("ppsVal").textContent = `${ppsCounter} PPS`;
            ppsCounter = 0;
        }, 1000);

        function applyKeyboardLayer(targetReport) {
            // Mapping buttons
            for (const key of Object.keys(BUTTON_BIT_MAP)) {
                if (isKeyPressed(key)) {
                    targetReport.buttons |= BUTTON_BIT_MAP[key];
                }
            }
            
            // Dpatch mapping hat
            const up = isKeyPressed("DPAD_UP");
            const down = isKeyPressed("DPAD_DOWN");
            const left = isKeyPressed("DPAD_LEFT");
            const right = isKeyPressed("DPAD_RIGHT");
            
            if (up && right) targetReport.hat = 1;
            else if (up && left) targetReport.hat = 7;
            else if (down && right) targetReport.hat = 3;
            else if (down && left) targetReport.hat = 5;
            else if (up) targetReport.hat = 0;
            else if (down) targetReport.hat = 4;
            else if (left) targetReport.hat = 6;
            else if (right) targetReport.hat = 2;
            else targetReport.hat = 8;

            // Sticks
            if (isKeyPressed("LSTICK_LEFT")) targetReport.lx = 0;
            else if (isKeyPressed("LSTICK_RIGHT")) targetReport.lx = 255;
            if (isKeyPressed("LSTICK_UP")) targetReport.ly = 0;
            else if (isKeyPressed("LSTICK_DOWN")) targetReport.ly = 255;

            if (isKeyPressed("RSTICK_LEFT")) targetReport.rx = 0;
            else if (isKeyPressed("RSTICK_RIGHT")) targetReport.rx = 255;
            if (isKeyPressed("RSTICK_UP")) targetReport.ry = 0;
            else if (isKeyPressed("RSTICK_DOWN")) targetReport.ry = 255;
        }

        function pollGamepad(idx) {
            const gps = navigator.getGamepads();
            const gp = gps[idx];
            if (!gp) return null;
            
            let rep = { buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };
            
            // Buttons standard Switch/Xbox binding allocations
            if (gp.buttons[0]?.pressed) rep.buttons |= 1<<1;  // B
            if (gp.buttons[1]?.pressed) rep.buttons |= 1<<2;  // A
            if (gp.buttons[2]?.pressed) rep.buttons |= 1<<0;  // Y
            if (gp.buttons[3]?.pressed) rep.buttons |= 1<<3;  // X
            if (gp.buttons[4]?.pressed) rep.buttons |= 1<<4;  // L
            if (gp.buttons[5]?.pressed) rep.buttons |= 1<<5;  // R
            if (gp.buttons[6]?.pressed || gp.buttons[6]?.value > 0.4) rep.buttons |= 1<<6;  // ZL
            if (gp.buttons[7]?.pressed || gp.buttons[7]?.value > 0.4) rep.buttons |= 1<<7;  // ZR
            if (gp.buttons[8]?.pressed) rep.buttons |= 1<<8;  // MINUS
            if (gp.buttons[9]?.pressed) rep.buttons |= 1<<9;  // PLUS
            if (gp.buttons[10]?.pressed) rep.buttons |= 1<<10; // LSTICK
            if (gp.buttons[11]?.pressed) rep.buttons |= 1<<11; // RSTICK
            if (gp.buttons[16]?.pressed) rep.buttons |= 1<<12; // HOME
            
            // Home combo
            if (gp.buttons[10]?.pressed && gp.buttons[11]?.pressed) {
                rep.buttons |= 1<<12;
                rep.buttons &= ~((1<<10)|(1<<11));
            }
            // Capture logic
            if (gp.buttons[8]?.pressed && gp.buttons[9]?.pressed) {
                rep.buttons |= 1<<13;
                rep.buttons &= ~((1<<8)|(1<<9));
            }

            const up = gp.buttons[12]?.pressed;
            const down = gp.buttons[13]?.pressed;
            const left = gp.buttons[14]?.pressed;
            const right = gp.buttons[15]?.pressed;
            if (up && right) rep.hat = 1;
            else if (up && left) rep.hat = 7;
            else if (down && right) rep.hat = 3;
            else if (down && left) rep.hat = 5;
            else if (up) rep.hat = 0;
            else if (down) rep.hat = 4;
            else if (left) rep.hat = 6;
            else if (right) rep.hat = 2;
            else rep.hat = 8;

            const scale = (val) => {
                const dead = 0.15;
                if (Math.abs(val) < dead) return 128;
                return Math.min(255, Math.max(0, Math.round(128 + val * 127)));
            };
            rep.lx = scale(gp.axes[0]);
            rep.ly = scale(gp.axes[1]);
            rep.rx = scale(gp.axes[2]);
            rep.ry = scale(gp.axes[3]);

            return rep;
        }

        function renderHudPort(num, rep, isActive) {
            const statusEl = document.getElementById(`p${num}Status`);
            const btnsEl = document.getElementById(`p${num}Btns`);
            
            // Left Stick dot layout representation
            const lStickDot = document.getElementById(`p${num}LStick`);
            const lxPct = (rep.lx / 255) * 100;
            const lyPct = (rep.ly / 255) * 100;
            lStickDot.style.left = `${lxPct}%`;
            lStickDot.style.top = `${lyPct}%`;

            // Right Stick dot layout representation
            const rStickDot = document.getElementById(`p${num}RStick`);
            const rxPct = (rep.rx / 255) * 100;
            const ryPct = (rep.ry / 255) * 100;
            rStickDot.style.left = `${rxPct}%`;
            rStickDot.style.top = `${ryPct}%`;

            if (!isActive) {
                statusEl.textContent = "OFFLINE";
                statusEl.className = "text-[10px] bg-slate-900 px-2 py-0.5 rounded text-slate-600 font-mono font-bold";
                btnsEl.innerHTML = "";
                return;
            }

            statusEl.textContent = "ONLINE";
            statusEl.className = "text-[10px] bg-violet-950 px-2 py-0.5 rounded text-violet-400 font-mono font-bold animate-pulse";
            
            // Print pressed badges
            let badges = [];
            for (const key of Object.keys(BUTTON_BIT_MAP)) {
                if ((rep.buttons & BUTTON_BIT_MAP[key]) !== 0) {
                    badges.push(`<span class="bg-violet-600 outline-none text-white px-1.5 py-0.5 rounded text-[8px] animate-fade-in">${key}</span>`);
                }
            }
            if (rep.hat !== 8) {
                badges.push(`<span class="bg-indigo-600 text-white px-1.5 py-0.5 rounded text-[8px] uppercase">HAT ${rep.hat}</span>`);
            }
            btnsEl.innerHTML = badges.join("");
        }

        // Fast raw binary subkey compiler transmission
        async function transmitCompressedReportBin(reportsBlock) {
            if (!ws || ws.readyState !== WebSocket.OPEN || !hmacKeyDigest) return;
            
            const buffer = new ArrayBuffer(68);
            const view = new DataView(buffer);
            const u8 = new Uint8Array(buffer);
            
            // Magic Check: 0x4E535743
            view.setUint32(0, 0x4E535743, true);
            view.setUint8(4, 4); // Version
            view.setUint8(5, 0); // Flags
            view.setUint16(6, 0, true); // Autofire
            view.setUint32(8, seq, true); // Seq
            seq = (seq + 1) & 0xffffffff;
            
            view.setBigUint64(12, BigInt(Date.now()) * 1000n, true); // ts microseconds
            
            const players = [reportsBlock.p1, reportsBlock.p2, reportsBlock.p3, reportsBlock.p4];
            for (let i = 0; i < 4; i++) {
                const p = players[i];
                const offset = 20 + i * 8;
                
                view.setUint16(offset, p.buttons, true);
                view.setUint8(offset + 2, p.hat);
                view.setUint8(offset + 3, p.lx);
                view.setUint8(offset + 4, p.ly);
                view.setUint8(offset + 5, p.rx);
                view.setUint8(offset + 6, p.ry);
                view.setUint8(offset + 7, 0); // vendor
            }

            // Compute subkey HMAC-SHA256 of first 52 bytes
            const bodyBytes = u8.subarray(0, 52);
            const sig = await window.crypto.subtle.sign("HMAC", hmacKeyDigest, bodyBytes);
            const sigU8 = new Uint8Array(sig);
            
            // Set first 16 bytes of HMAC tag on signature offset 52
            u8.set(sigU8.subarray(0, 16), 52);
            
            // Directly dispatch byte payload
            ws.send(buffer);
        }

        // Reset and save bindings elements
        document.getElementById("resetBindingsBtn").onclick = () => {
            customBindings = { ...DEFAULT_BINDINGS };
            renderBindingsTable();
        };

        document.getElementById("saveBindingsBtn").onclick = () => {
            localStorage.setItem(LOCAL_BINDINGS_KEY, JSON.stringify(customBindings));
            alert("Mappings configured saved to localStorage successfully!");
        };

    </script>
</body>
</html>
)HTML";
}

static void serve_http_webpage(int client_fd, const std::string& request) {
    std::string html = get_embedded_html();
    std::string response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(html.size()) + "\r\n"
        "Connection: close\r\n\r\n" + html;
    send(client_fd, response.data(), response.size(), 0);
    close(client_fd);
}

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;
static bool g_enable_webapp = false;

// Derived HMAC keys from DEFAULT_SECRET
static uint8_t g_hmac_key[32];

// Per-IP rate limiting (token bucket, 32-entry hash table)
static constexpr uint64_t RATE_WINDOW_US = 1'000'000;  // 1 second
static constexpr uint32_t RATE_MAX_PKT   = 2000;       // max packets/sec per IP
static constexpr int      RATE_TABLE     = 32;

struct RateSlot {
    uint32_t ip;           // IP in network byte order, 0 = empty
    uint32_t count;
    uint64_t window_start; // us
};
static RateSlot g_rate_table[RATE_TABLE];

// ── Multi-Client Session State ───────────────────────────────────────────────
static constexpr int MAX_CLIENTS = 4; // Hard limit matching the 4 physical ports

struct ClientSession {
    bool        active = false;
    sockaddr_in addr{};
    uint64_t    last_rx_us = 0;
    uint32_t    expected_seq = 0;
    bool        first_pkt = true;
    ns::MultiReport report{}; // The inputs coming from this specific PC
};

static std::mutex    g_mtx;
static std::condition_variable g_cv;
static bool g_has_update = false;
static ClientSession g_clients[MAX_CLIENTS];

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// WebSocket connection buffers per-socket
struct WsConnState {
    bool handshaked = false;
    std::string http_buffer;
    std::vector<uint8_t> ws_payload_buffer;
};
static std::map<int, WsConnState> g_ws_conns;

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void writer_thread(int hz) {
    // Elevate scheduling priority on Raspberry Pi for optimal real-time response
    pthread_t this_thread = pthread_self();
    struct sched_param sched_params;
    sched_params.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(this_thread, SCHED_FIFO, &sched_params) == 0) {
        if (g_verbose) std::puts("[rt] Output writer thread promoted to real-time SCHED_FIFO scheduling priority");
    } else {
        // Fallback: set thread niceness
        int res = nice(-20);
        (void)res;
    }

    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    // Tracks which physical Switch port is claimed by which (Client, SubController)
    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];

    auto is_neutral = [](const ns::HIDReport& r) {
        return r.buttons == 0 && r.hat == 8 && r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
    };

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for(int i=0; i<4; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_WRONLY);
                if (fds[i] < 0) all_open = false;
            }
        }

        if (!all_open) {
            std::this_thread::sleep_for(ms(500));
            continue;
        }
        
        if (g_verbose || !was_connected)
            std::puts("4x /dev/hidg* opened and mapped securely");
        was_connected = true;

        auto next = Clock::now() + tick;
        ns::MultiReport prev{}; prev.p1.buttons = 0xFFFF; // Force first write
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(g_mtx);
            // Wait for update or timeout (ensures instant response to incoming inputs with ~0ms delay)
            g_cv.wait_until(lk, next, []{ return g_has_update || !g_running.load(std::memory_order_relaxed); });
            g_has_update = false;

            if (!g_running.load(std::memory_order_relaxed)) break;

            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            ns::MultiReport r;
            r.reset(); // Base neutral state

            uint64_t now_stamp = ns::now_us();

            // 1. Clear timed-out clients (Watchdog)
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (g_clients[c].active && (now_stamp - g_clients[c].last_rx_us > ns::WATCHDOG_MS * 1000ULL)) {
                    g_clients[c].active = false;
                    if (g_verbose) std::printf("PC %d input timed out and was disconnected.\n", c+1);
                }
            }

            // 2. Free hardware slots mapped to inactive clients
            for (int h = 0; h < 4; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    if (!g_clients[hw_slots[h].client_idx].active) {
                        hw_slots[h].client_idx = -1;
                        hw_slots[h].sub_idx = -1;
                    }
                }
            }

            // 3. Auto-assign unmapped active inputs to free hardware slots
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!g_clients[c].active) continue;
                
                ns::HIDReport* subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                           &g_clients[c].report.p3, &g_clients[c].report.p4 };
                
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < 4; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true; break;
                        }
                    }
                    
                    // If player pressed a button and doesn't have a physical port yet
                    if (!mapped && !is_neutral(*subs[s])) {
                        for (int h = 0; h < 4; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                hw_slots[h].client_idx = c;
                                hw_slots[h].sub_idx = s;
                                if (g_verbose) 
                                    std::printf("Map -> Web PC %d (Pad %d) mapped to Switch Port %d\n", c+1, s+1, h+1);
                                break;
                            }
                        }
                    }
                }
            }

            // 4. Construct the final mixed 4-player report
            ns::HIDReport* out_subs[4] = { &r.p1, &r.p2, &r.p3, &r.p4 };
            for (int h = 0; h < 4; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    int c = hw_slots[h].client_idx;
                    int s = hw_slots[h].sub_idx;
                    ns::HIDReport* src_subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                               &g_clients[c].report.p3, &g_clients[c].report.p4 };
                    *out_subs[h] = *src_subs[s];
                }
            }

            // Unlock early to minimize lock contention during actual I/O writes
            lk.unlock();

            // 5. Send to physical USB gadget drivers efficiently
            bool ok = true;
            if (r.p1 != prev.p1) { if(write(fds[0], &r.p1, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p2 != prev.p2) { if(write(fds[1], &r.p2, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p3 != prev.p3) { if(write(fds[2], &r.p3, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p4 != prev.p4) { if(write(fds[3], &r.p4, 8) < 0 && errno != EAGAIN) ok = false; }

            if (!ok) {
                if (!error_shown) { std::puts("Switch physical gadgets write failed — awaiting hot-reconnect..."); error_shown = true; }
                for(int i=0; i<4; ++i) { close(fds[i]); fds[i] = -1; }
                std::this_thread::sleep_for(ms(1000)); break;
            }
            prev = r;
            ++g_hid_writes;
        }
    }
    
    // Shutdown securely by neutralizing all ports
    ns::MultiReport neutral{}; neutral.reset();
    for(int i=0; i<4; ++i) { 
        if (fds[i] >= 0) { (void)write(fds[i], &neutral.p1, 8); close(fds[i]); }
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static bool rate_allow(uint32_t ip) {
    uint64_t now = ns::now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
    }
    s.count++;
    return s.count <= RATE_MAX_PKT;
}

// ── Packet Processor Handler ───────────────────────────────────────────────
static void process_incoming_packet(const ns::Packet& pkt, uint32_t src_ip, uint16_t src_port) {
    // 1. Magic and version verification check
    if (pkt.magic != ns::PROTO_MAGIC || pkt.version != ns::PROTO_VERSION) {
        if (g_verbose) std::puts("[validator] Dropping packet with bad magic or version");
        return;
    }

    // 2. HMAC authentication check
    uint8_t hmac_calc[32];
    hmac_sha256_verify(g_hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, hmac_calc);
    if (std::memcmp(hmac_calc, pkt.hmac, ns::HMAC_TAG_SIZE) != 0) {
        if (g_verbose) std::puts("[validator] Handshake dropped: HMAC mismatch (Check Secret matches)");
        return;
    }

    // 3. Pin/Find Active Session
    int client_idx = -1;
    uint64_t now = ns::now_us();
    
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (g_clients[i].active &&
            g_clients[i].addr.sin_addr.s_addr == src_ip &&
            g_clients[i].addr.sin_port == src_port) {
            client_idx = i;
            break;
        }
    }

    // If completely new client connection, find slot
    if (client_idx == -1) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!g_clients[i].active || (now - g_clients[i].last_rx_us > ns::WATCHDOG_MS * 1000ULL)) {
                client_idx = i;
                g_clients[i].active = true;
                g_clients[i].addr.sin_addr.s_addr = src_ip;
                g_clients[i].addr.sin_port = src_port;
                g_clients[i].first_pkt = true;
                g_clients[i].report.reset();
                if (g_verbose) std::printf("[core] New client accepted into physical/virtual slot: %d/4\n", i+1);
                break;
            }
        }
    }

    if (client_idx == -1) {
        if (g_verbose) std::puts("[core] System slot is full, dropping inputs packet");
        return;
    }

    // 4. Anti-Replay seq checks
    bool is_reset = (pkt.flags & ns::FLAG_RESET);
    bool sequence_jump = (g_clients[client_idx].expected_seq > pkt.seq) && ((g_clients[client_idx].expected_seq - pkt.seq) > 100);

    if (!g_clients[client_idx].first_pkt && pkt.seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
        if (g_verbose) std::printf("[anti-replay] Client %d dropped duplicate/stale sequence packet (%u < %u)\n", client_idx+1, pkt.seq, g_clients[client_idx].expected_seq);
        return;
    }
    g_clients[client_idx].first_pkt = false;
    g_clients[client_idx].expected_seq = pkt.seq + 1;

    // 5. Commit states safely
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (is_reset) {
            g_clients[client_idx].report.reset();
        } else {
            g_clients[client_idx].report = pkt.report;
        }
        g_clients[client_idx].last_rx_us = now;
        g_has_update = true;
    }
    g_cv.notify_one();
    ++g_pkts_rx;
}

// ── WebSocket Handler on upgrade and data frame parsing ──────────────────────
static void handle_websocket_upgrade(int client_fd, WsConnState& conn) {
    size_t key_pos = conn.http_buffer.find("Sec-WebSocket-Key: ");
    if (key_pos == std::string::npos) return;
    
    size_t key_end = conn.http_buffer.find("\r\n", key_pos);
    if (key_end == std::string::npos) return;
    
    std::string key = conn.http_buffer.substr(key_pos + 19, key_end - (key_pos + 19));
    std::string accept_val = generate_ws_accept_key(key);
    
    std::string response = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_val + "\r\n\r\n";
        
    send(client_fd, response.data(), response.size(), 0);
    conn.handshaked = true;
    conn.http_buffer.clear();
    if (g_verbose) std::puts("[ws-upgrade] Handshake complete. Upgraded TCP connection to custom low-latency WebSocket.");
}

static void parse_websocket_frames(int client_fd, WsConnState& conn, uint32_t src_ip, uint16_t src_port) {
    while (conn.ws_payload_buffer.size() >= 2) {
        uint8_t byte0 = conn.ws_payload_buffer[0];
        uint8_t byte1 = conn.ws_payload_buffer[1];
        
        bool is_masked = (byte1 & 0x80) != 0;
        uint64_t payload_len = byte1 & 0x7F;
        size_t header_len = 2;
        
        if (payload_len == 126) {
            if (conn.ws_payload_buffer.size() < 4) return;
            payload_len = (conn.ws_payload_buffer[2] << 8) | conn.ws_payload_buffer[3];
            header_len = 4;
        } else if (payload_len == 127) {
            if (conn.ws_payload_buffer.size() < 10) return;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | conn.ws_payload_buffer[2 + i];
            }
            header_len = 10;
        }
        
        size_t mask_offset = header_len;
        if (is_masked) {
            header_len += 4;
        }
        
        if (conn.ws_payload_buffer.size() < header_len + payload_len) {
            return; // Needs more bytes
        }
        
        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0) {
            uint8_t masks[4] = {0};
            if (is_masked) {
                masks[0] = conn.ws_payload_buffer[mask_offset];
                masks[1] = conn.ws_payload_buffer[mask_offset + 1];
                masks[2] = conn.ws_payload_buffer[mask_offset + 2];
                masks[3] = conn.ws_payload_buffer[mask_offset + 3];
            }
            
            for (size_t i = 0; i < payload_len; ++i) {
                uint8_t raw_byte = conn.ws_payload_buffer[header_len + i];
                payload[i] = is_masked ? (raw_byte ^ masks[i % 4]) : raw_byte;
            }
        }
        
        // Remove parsed frame out of FIFO buffer
        conn.ws_payload_buffer.erase(conn.ws_payload_buffer.begin(), conn.ws_payload_buffer.begin() + header_len + payload_len);
        
        uint8_t opcode = byte0 & 0x0F;
        if (opcode == 0x08) { // WebSocket Protocol CLOSE
            if (g_verbose) std::puts("[ws] Client sent CLOSE frame handler.");
            close(client_fd);
            g_ws_conns.erase(client_fd);
            return;
        }
        
        // Packet parsing if binary payload size equals exactly our packed Struct
        if (payload_len == ns::PACKET_SIZE) {
            ns::Packet pkt;
            std::memcpy(&pkt, payload.data(), ns::PACKET_SIZE);
            process_incoming_packet(pkt, src_ip, src_port);
        }
    }
}

// ── Main Server Execution loop ───────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = ns::DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "-w")               g_enable_webapp = true;
        else if (a == "-h" || a == "--help") {
            std::puts("ns-backend  [-p PORT] [-b ADDR] [-w (Enable Webapp WS Proxy)] [-v]");
            return 0;
        }
    }

    // Deriving SHA256 key from default client configuration handshake secret
    uint8_t key_material[64];
    std::memset(key_material, 0, 64);
    std::memcpy(key_material, ns::DEFAULT_SECRET, std::strlen(ns::DEFAULT_SECRET));
    sha256_hash(key_material, std::strlen(ns::DEFAULT_SECRET), g_hmac_key);

    std::signal(SIGINT,  on_signal); 
    std::signal(SIGTERM, on_signal); 
    std::signal(SIGPIPE, SIG_IGN);

    // Initializing UDP socket configuration
    int udp_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (udp_sock < 0) { std::perror("UDP Socket creation failed"); return 1; }

    int yes = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 256 * 1024;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    // Tuning UDP low latency routing TOS
    int tos = 0x10; // IPTOS_LOWDELAY
    setsockopt(udp_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    sockaddr_in addr{}; 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(udp_sock, (sockaddr*)&addr, sizeof(addr)) < 0) { std::perror("UDP Bind failed"); close(udp_sock); return 1; }
    
    // Initializing TCP/WebSocket sockets configurations if webapp toggles
    int tcp_listener = -1;
    if (g_enable_webapp) {
        tcp_listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (tcp_listener < 0) { std::perror("TCP Socket creation failed"); close(udp_sock); return 1; }
        
        setsockopt(tcp_listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(tcp_listener, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        if (bind(tcp_listener, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::perror("TCP Bind failed"); 
            close(udp_sock); close(tcp_listener); 
            return 1; 
        }
        if (listen(tcp_listener, 8) < 0) {
            std::perror("TCP Listen failed");
            close(udp_sock); close(tcp_listener);
            return 1;
        }
    }

    std::printf("[RPi Backend Server Executing on Port %u with %dHz physical HID feedback loops]\n", port, ns::WRITER_HZ);
    if (g_enable_webapp) {
        std::printf("[info] Low-latency target WebSocket direct connections listener is strictly enabled (WS/WSS support enabled)\n");
    }

    std::thread wt(writer_thread, ns::WRITER_HZ);

    int ep = epoll_create1(0); 
    epoll_event ev{}; 
    ev.events = EPOLLIN; 
    ev.data.fd = udp_sock; 
    epoll_ctl(ep, EPOLL_CTL_ADD, udp_sock, &ev);

    if (g_enable_webapp && tcp_listener >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = tcp_listener;
        epoll_ctl(ep, EPOLL_CTL_ADD, tcp_listener, &ev);
    }

    ns::Packet pkt{};
    epoll_event evs[16];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 16, 200 /*ms timeout*/);
        if (n <= 0) continue;

        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;
            
            if (fd == udp_sock) {
                // Handle standard incoming UDP reports packets
                sockaddr_in sender{};
                socklen_t slen = sizeof(sender);
                ssize_t bytes = recvfrom(udp_sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen);

                if (bytes == (ssize_t)ns::PACKET_SIZE) {
                    uint32_t src_ip = sender.sin_addr.s_addr;
                    if (rate_allow(src_ip)) {
                        process_incoming_packet(pkt, src_ip, sender.sin_port);
                    }
                }
            } 
            else if (g_enable_webapp && fd == tcp_listener) {
                // Register/Upgrade direct network incoming Websocket Client requests
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(tcp_listener, (sockaddr*)&client_addr, &client_len);
                if (client_fd >= 0) {
                    // Make socket non-blocking
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                    
                    // Force TCP_NODELAY to completely disable Nagle algorithm (drops latency to sub-millisecond)
                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                    // Boost low-latency packet scheduler
                    int c_tos = 0x10; // IPTOS_LOWDELAY
                    setsockopt(client_fd, IPPROTO_IP, IP_TOS, &c_tos, sizeof(c_tos));

                    // Add to epoll monitor group
                    ev.events = EPOLLIN;
                    ev.data.fd = client_fd;
                    epoll_ctl(ep, EPOLL_CTL_ADD, client_fd, &ev);
                    
                    g_ws_conns[client_fd] = WsConnState{};
                    if (g_verbose) std::printf("[ws-proxy] Active browser peer connection accepted on fd %d\n", client_fd);
                }
            } 
            else {
                // Receive and decode HTTP frames or WebSocket events on connected TCP sockets
                sockaddr_in peer_addr{};
                socklen_t peer_len = sizeof(peer_addr);
                getpeername(fd, (sockaddr*)&peer_addr, &peer_len);
                
                uint8_t read_buf[2048];
                ssize_t bytes = recv(fd, read_buf, sizeof(read_buf), 0);
                
                if (bytes <= 0) {
                    // Socket closed or error occurred
                    close(fd);
                    g_ws_conns.erase(fd);
                    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    if (g_verbose) std::printf("[ws-proxy] Peer disconnected on fd %d\n", fd);
                } else {
                    WsConnState& conn = g_ws_conns[fd];
                    if (!conn.handshaked) {
                        conn.http_buffer.append((char*)read_buf, bytes);
                        if (conn.http_buffer.find("\r\n\r\n") != std::string::npos) {
                            if (conn.http_buffer.find("Upgrade: websocket") != std::string::npos ||
                                conn.http_buffer.find("upgrade: websocket") != std::string::npos ||
                                conn.http_buffer.find("Sec-WebSocket-Key:") != std::string::npos) {
                                handle_websocket_upgrade(fd, conn);
                            } else if (conn.http_buffer.find("GET ") != std::string::npos) {
                                serve_http_webpage(fd, conn.http_buffer);
                                epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                                g_ws_conns.erase(fd);
                            }
                        }
                    } else {
                        conn.ws_payload_buffer.insert(conn.ws_payload_buffer.end(), read_buf, read_buf + bytes);
                        parse_websocket_frames(fd, conn, peer_addr.sin_addr.s_addr, peer_addr.sin_port);
                    }
                }
            }
        }
    }

    std::puts("[core-backend] cleanly shutting down ports multiplexers");
    close(ep); 
    close(udp_sock);
    if (tcp_listener >= 0) close(tcp_listener);
    
    for (auto const& [fd, state] : g_ws_conns) {
        close(fd);
    }
    wt.join();
    return 0;
}
