#include "../../include/protocol.hpp"
#include "../../include/sha256.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <algorithm>
#include <vector>
#include <map>
#include <set>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <errno.h>

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// HMAC authentication (key derived from DEFAULT_SECRET at startup)
static uint8_t  g_hmac_key[32];

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
static constexpr int MAX_CLIENTS    = 5; // 4 UDP + 1 web client
static constexpr int WEB_CLIENT_IDX = MAX_CLIENTS - 1; // Slot 4 reserved for web

struct ClientSession {
    bool        active = false;
    sockaddr_in addr{};
    uint64_t    last_rx_us = 0;
    uint32_t    expected_seq = 0;
    bool        first_pkt = true;
    MultiReport report{}; // The inputs coming from this specific PC
};

static std::mutex    g_mtx;
static ClientSession g_clients[MAX_CLIENTS];

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }


// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    // Tracks which physical Switch port is claimed by which (Client, SubController)
    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];

    auto is_neutral = [](const HIDReport& r) {
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
            std::puts("4x /dev/hidg* opened");
        was_connected = true;

        auto next = Clock::now() + tick;
        MultiReport prev{}; prev.p1.buttons = 0xFFFF; // Force first write
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now(); next = std::max(next + tick, now + tick);

            MultiReport r;
            r.reset(); // Base neutral state

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                uint64_t now_stamp = now_us();

                // 1. Clear timed-out clients (Watchdog)
                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (g_clients[c].active && (now_stamp - g_clients[c].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                        g_clients[c].active = false;
                        if (g_verbose) std::printf("PC %d timed out and was disconnected.\n", c+1);
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
                    
                    HIDReport* subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
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
                                        std::printf("Map -> PC %d (Pad %d) took Switch Port %d\n", c+1, s+1, h+1);
                                    break;
                                }
                            }
                        }
                    }
                }

                // 4. Construct the final mixed 4-player report
                HIDReport* out_subs[4] = { &r.p1, &r.p2, &r.p3, &r.p4 };
                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        int c = hw_slots[h].client_idx;
                        int s = hw_slots[h].sub_idx;
                        HIDReport* src_subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                                   &g_clients[c].report.p3, &g_clients[c].report.p4 };
                        *out_subs[h] = *src_subs[s];
                    }
                }
            }

            // 5. Send to physical USB gadget drivers efficiently
            bool ok = true;
            if (r.p1 != prev.p1) { if(write(fds[0], &r.p1, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p2 != prev.p2) { if(write(fds[1], &r.p2, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p3 != prev.p3) { if(write(fds[2], &r.p3, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p4 != prev.p4) { if(write(fds[3], &r.p4, 8) < 0 && errno != EAGAIN) ok = false; }

            if (!ok) {
                if (!error_shown) { std::puts("Switch disconnected — waiting for reconnect..."); error_shown = true; }
                for(int i=0; i<4; ++i) { close(fds[i]); fds[i] = -1; }
                std::this_thread::sleep_for(ms(1000)); break;
            }
            prev = r;
            ++g_hid_writes;
        }
    }
    
    // Shutdown securely by neutralizing all ports
    MultiReport neutral{}; neutral.reset();
    for(int i=0; i<4; ++i) { 
        if (fds[i] >= 0) { (void)write(fds[i], &neutral.p1, 8); close(fds[i]); }
    }
}


// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));
        if (!g_verbose) continue;
        std::printf("pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static bool rate_allow(uint32_t ip) {
    uint64_t now = now_us();
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


#ifdef USE_UPNP
// ── UPnP port forwarding ──
static bool g_upnp_active = false;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) return false;
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    g_upnp_active = true;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("UPnP: UDP port %u successfully forwarded!\n", port);
        std::printf("UPnP: Tell your clients to connect to -> %s:%u\n", external_ip, port);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t port) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    std::puts("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif


// ── Web Server (HTTP + WebSocket) ─────────────────────────────────────────────

static const char* WEB_PAGE = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NS PC Control</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f0f0;color:#333;display:flex;justify-content:center;padding:30px 20px}
.container{max-width:420px;width:100%;background:#fff;border-radius:8px;padding:24px 20px;box-shadow:0 2px 12px rgba(0,0,0,.15)}
h1{font-size:22px;font-weight:700;color:#c00;margin:0 0 16px 0;text-align:center}
.row{margin-bottom:10px;display:flex;align-items:center;gap:8px}
.row label{display:flex;align-items:center;gap:6px;cursor:pointer;font-size:14px}
.row input[type=text]{flex:1;padding:6px 10px;border:1px solid #ccc;border-radius:4px;font-size:14px;font-family:Consolas,monospace;background:#f9f9f9}
.row button{padding:6px 18px;border:none;border-radius:4px;cursor:pointer;font-size:14px;font-weight:600}
.btn-primary{background:#c00;color:#fff;width:100%;padding:10px}
.btn-primary.active{background:#2e7d32}
.btn-secondary{background:#e0e0e0;color:#333;padding:6px 14px;font-size:12px}
.btn-secondary:disabled{opacity:.4;cursor:default}
#status{font-size:13px;color:#666;text-align:center;padding:8px;background:#f5f5f5;border-radius:4px;display:block;margin-top:4px}
.player-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:6px}
.player{font-size:13px;padding:6px 10px;background:#f5f5f5;border-radius:4px;border-left:3px solid #ccc}
.player.active{border-left-color:#2e7d32;background:#e8f5e9}
.pkt-info{font-size:12px;color:#999;text-align:center;margin-top:8px}
/* Bindings Editor Modal */
.modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.4);justify-content:center;align-items:center;z-index:100}
.modal-box{background:#fff;border-radius:8px;padding:12px;box-shadow:0 4px 20px rgba(0,0,0,.3);max-height:90vh;overflow-y:auto;width:560px}
.modal-box h2{font-size:16px;margin:0 0 8px 0;color:#c00}
#edit-table{width:100%;border-collapse:collapse}
#edit-table td{padding:2px 3px;font-size:12px}
#edit-table td.el{text-align:center;font-weight:600;width:80px}
#edit-table td.ek{font-family:Consolas,monospace;background:#f5f5f5;padding:2px 6px;border-radius:3px;width:120px;text-align:center}
#edit-table td button{padding:1px 8px;font-size:11px;border:1px solid #bbb;border-radius:3px;background:#fff;cursor:pointer}
#edit-table td button:hover{background:#eee}
.modal-buttons{display:flex;justify-content:center;gap:6px;margin-top:8px}
.modal-buttons button{padding:6px 16px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600}
</style>
</head>
<body>
<div class="container">
<h1>NS PC Control</h1>
<div class="row"><label style="min-width:110px;text-align:right">Raspberry Pi IP:</label><input type="text" id="ip" readonly></div>
<div class="row"><label><input type="checkbox" id="kb-mode"> Keyboard Mode (overrides P1)</label><button id="bindings-btn" class="btn-secondary" disabled>Bindings...</button></div>
<div class="row"><button id="connect-btn" class="btn-primary">Connect</button></div>
<span id="status">Web Server Active</span>
<div class="player-grid"><div class="player" id="p1">P1: Waiting...</div><div class="player" id="p2">P2: Waiting...</div><div class="player" id="p3">P3: Waiting...</div><div class="player" id="p4">P4: Waiting...</div></div>
<div class="pkt-info" id="pkt-info">Packets sent: 0</div>
</div>
<div class="modal-overlay" id="edit-modal" tabindex="0">
<div class="modal-box">
<h2>Edit Key Bindings</h2>
<table id="edit-table"><tbody></tbody></table>
<div class="modal-buttons" style="display:flex;justify-content:space-between;margin:8px 4px 0">
<div style="display:flex;flex-direction:column;gap:6px"><button onclick="saveEditor()" style="background:#c00;color:#fff;padding:6px 20px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600">Save</button><button onclick="closeEditor()" style="padding:6px 20px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600">Cancel</button></div>
<div style="display:flex;flex-direction:column;gap:6px"><button onclick="startSetup()" style="padding:6px 20px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600">Setup</button><button onclick="resetEditor()" style="padding:6px 20px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600">Reset</button></div>
</div>
</div>
</div>
<script>
const WS_URL = `ws://${location.hostname}:${location.port}/`;
let ws=null, connected=false, enabled=false, kbMode=false, pollId=null;
let keys={}, kbdBindings={};
try{const s=JSON.parse(localStorage.getItem('nsBindings'));if(s)kbdBindings=s}catch(e){}
const DEF={Y:'KeyZ',B:'KeyX',A:'KeyC',X:'KeyV',L:'KeyQ',R:'KeyE',ZL:'Digit1',ZR:'Digit2',MINUS:'Digit3',PLUS:'Digit4',LSTICK:'ShiftLeft',RSTICK:'ShiftRight',DUP:'ArrowUp',DDOWN:'ArrowDown',DLEFT:'ArrowLeft',DRIGHT:'ArrowRight'};
const BTN_BIT={Y:0,B:1,A:2,X:3,L:4,R:5,ZL:6,ZR:7,MINUS:8,PLUS:9,LSTICK:10,RSTICK:11};
const DPAD={'ArrowUp':0,'ArrowUpRight':1,'ArrowRight':2,'ArrowDownRight':3,'ArrowDown':4,'ArrowDownLeft':5,'ArrowLeft':6,'ArrowUpLeft':7};
Object.assign(kbdBindings,DEF);
function buildButtonMap(){const m={};for(const[btn,code]of Object.entries(kbdBindings)){if(code&&code!=='---'&&BTN_BIT[btn]!==undefined)m[code]=1<<BTN_BIT[btn]}return m}
// ── Bindings Editor ──
let editBindings={};
let listeningIdx=-1,setupMode=false;
const BKEYS=['Y','B','A','X','L','R','ZL','ZR','MINUS','PLUS','LSTICK','RSTICK','DUP','DDOWN','DLEFT','DRIGHT'];
function openEditor(){
editBindings=JSON.parse(JSON.stringify(kbdBindings));
listeningIdx=-1;setupMode=false;
const tb=document.getElementById('edit-table').tBodies[0];tb.innerHTML='';
for(let i=0;i<12;i++){const li=i,ri=i+12;const tr=document.createElement('tr');
tr.innerHTML='<td class="el">'+BKEYS[li]+'</td><td class="ek" id="ek-'+li+'">'+(editBindings[BKEYS[li]]||'')+'</td><td><button class="eb" data-idx="'+li+'">Ch</button></td>'+
'<td class="el">'+BKEYS[ri]+'</td><td class="ek" id="ek-'+ri+'">'+(editBindings[BKEYS[ri]]||'')+'</td><td><button class="eb" data-idx="'+ri+'">Ch</button></td>';
tr.querySelectorAll('.eb')[0].onclick=function(){setupMode=false;listeningIdx=parseInt(this.dataset.idx);document.getElementById('ek-'+listeningIdx).textContent='...';document.getElementById('edit-modal').focus()};
tr.querySelectorAll('.eb')[1].onclick=function(){setupMode=false;listeningIdx=parseInt(this.dataset.idx);document.getElementById('ek-'+listeningIdx).textContent='...';document.getElementById('edit-modal').focus()};
tb.appendChild(tr)}
document.getElementById('edit-modal').style.display='flex';document.getElementById('edit-modal').focus()}
function closeEditor(){document.getElementById('edit-modal').style.display='none';listeningIdx=-1;setupMode=false}
function saveEditor(){kbdBindings=JSON.parse(JSON.stringify(editBindings));try{localStorage.setItem('nsBindings',JSON.stringify(kbdBindings))}catch(e){}closeEditor()}
function resetEditor(){for(let i=0;i<BKEYS.length;i++){editBindings[BKEYS[i]]=DEF[BKEYS[i]];document.getElementById('ek-'+i).textContent=DEF[BKEYS[i]]}}
function startSetup(){
setupMode=true;
for(let i=0;i<BKEYS.length;i++){editBindings[BKEYS[i]]='';document.getElementById('ek-'+i).textContent=i===0?'...':''}
listeningIdx=0;document.getElementById('edit-modal').focus()}
document.addEventListener('keydown',function(e){
if(listeningIdx<0||document.getElementById('edit-modal').style.display!=='flex')return;
e.preventDefault();const k=BKEYS[listeningIdx];
if(e.code==='Escape'){
editBindings[k]='';document.getElementById('ek-'+listeningIdx).textContent='';
if(setupMode){listeningIdx++;if(listeningIdx<BKEYS.length){document.getElementById('ek-'+listeningIdx).textContent='...';return}}
listeningIdx=-1;setupMode=false;return}
// In setup mode, skip already-bound keys
if(setupMode){let ab=false;for(let i=0;i<BKEYS.length;i++){if(i!==listeningIdx&&editBindings[BKEYS[i]]===e.code){ab=true;break}}if(ab)return}
// Remove this key from any other binding
for(let i=0;i<BKEYS.length;i++){if(i!==listeningIdx&&editBindings[BKEYS[i]]===e.code){editBindings[BKEYS[i]]='';document.getElementById('ek-'+i).textContent=''}}
editBindings[k]=e.code;document.getElementById('ek-'+listeningIdx).textContent=e.code;
if(setupMode){listeningIdx++;if(listeningIdx<BKEYS.length){document.getElementById('ek-'+listeningIdx).textContent='...';return}}
listeningIdx=-1;setupMode=false});
function sendR(){if(!ws||ws.readyState!==1||!enabled)return;let b=0,h=8,lx=128,ly=128,rx=128,ry=128;const gp=navigator.getGamepads();let used=false;for(const g of gp){if(g){b|=(g.buttons[0]?.pressed?1<<1:0);b|=(g.buttons[1]?.pressed?1<<2:0);b|=(g.buttons[2]?.pressed?1<<3:0);b|=(g.buttons[3]?.pressed?1<<0:0);b|=(g.buttons[4]?.pressed?1<<4:0);b|=(g.buttons[5]?.pressed?1<<5:0);b|=(g.buttons[6]?.pressed?1<<6:0);b|=(g.buttons[7]?.pressed?1<<7:0);b|=(g.buttons[8]?.pressed?1<<8:0);b|=(g.buttons[9]?.pressed?1<<9:0);b|=(g.buttons[10]?.pressed?1<<10:0);b|=(g.buttons[11]?.pressed?1<<11:0);const u=g.buttons[12]?.pressed,d=g.buttons[13]?.pressed,l=g.buttons[14]?.pressed,r=g.buttons[15]?.pressed;if(u&&r)h=1;else if(d&&r)h=3;else if(d&&l)h=5;else if(u&&l)h=7;else if(u)h=0;else if(r)h=2;else if(d)h=4;else if(l)h=6;lx=Math.round(((g.axes[0]??0)+1)*127.5);ly=Math.round(((g.axes[1]??0)+1)*127.5);rx=Math.round(((g.axes[2]??0)+1)*127.5);ry=Math.round(((g.axes[3]??0)+1)*127.5);used=true;break}}
if(!used&&kbMode){const bm=buildButtonMap();for(const[code,bit]of Object.entries(bm)){if(keys[code])b|=bit}const ku=keys['ArrowUp'],kd=keys['ArrowDown'],kl=keys['ArrowLeft'],kr=keys['ArrowRight'];if(ku&&kr)h=1;else if(kd&&kr)h=3;else if(kd&&kl)h=5;else if(ku&&kl)h=7;else if(ku)h=0;else if(kr)h=2;else if(kd)h=4;else if(kl)h=6}
const dv=new DataView(new ArrayBuffer(8));dv.setUint16(0,b,1);dv.setUint8(2,h);dv.setUint8(3,lx);dv.setUint8(4,ly);dv.setUint8(5,rx);dv.setUint8(6,ry);dv.setUint8(7,0);ws.send(dv.buffer)}
function conn(){ws=new WebSocket(WS_URL);ws.onopen=()=>{connected=true;ui();pollId=setInterval(sendR,4)};ws.onmessage=e=>{try{const d=JSON.parse(e.data);if(d.type==='status'&&d.clients){for(let i=0;i<4;i++){const el=document.getElementById(`p${i+1}`),c=d.clients[i];if(c&&c.active){el.textContent=`P${i+1}: Connected`;el.className='player active'}else{el.textContent=`P${i+1}: Not connected`;el.className='player'}}if(d.pkts_rx!==undefined)document.getElementById('pkt-info').textContent=`Packets sent: ${d.pkts_rx}`}}catch(e){}};ws.onclose=()=>{connected=false;enabled=false;if(pollId){clearInterval(pollId);pollId=null}ui();setTimeout(conn,2000)};ws.onerror=()=>{}}
function ui(){const b=document.getElementById('connect-btn');if(enabled){b.textContent='Disconnect';b.className='btn-primary active'}else{b.textContent='Connect';b.className='btn-primary'}document.getElementById('status').textContent=connected?(enabled?'Connected':'Web Server Active'):'Connecting...'}
document.getElementById('connect-btn').onclick=()=>{if(!connected)return;enabled=!enabled;if(!enabled){const dv=new DataView(new ArrayBuffer(8));for(let i=0;i<8;i++)dv.setUint8(i,i<2?0:128);ws.send(dv.buffer)}ui()};
document.getElementById('kb-mode').onchange=e=>{kbMode=e.target.checked;const bb=document.getElementById('bindings-btn');bb.disabled=!kbMode;if(kbMode)bb.onclick=openEditor;else bb.onclick=null};
document.addEventListener('keydown',e=>{if(!kbMode||!enabled||listeningIdx>=0)return;keys[e.code]=true;e.preventDefault()});
document.addEventListener('keyup',e=>{if(!kbMode)return;keys[e.code]=false;e.preventDefault()});
window.addEventListener('beforeunload',()=>{if(ws&&ws.readyState===1){ws.close()}});
document.getElementById('ip').value=location.hostname;conn();
</script>
</body>
</html>)html";

// --- SHA1 for WebSocket handshake ---
struct SHA1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void init() {
        state[0] = 0x67452301; state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE; state[3] = 0x10325476; state[4] = 0xC3D2E1F0;
        count = 0;
    }

    void transform(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)block[i*4] << 24 | (uint32_t)block[i*4+1] << 16 |
                   (uint32_t)block[i*4+2] << 8 | block[i*4+3];
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)       { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40)  { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
            else if (i < 60)  { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
            uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        size_t idx = (size_t)(count & 63);
        count += len;
        size_t free = 64 - idx;
        if (len >= free) {
            memcpy(buffer + idx, data, free);
            transform(buffer);
            data += free; len -= free;
            while (len >= 64) { transform(data); data += 64; len -= 64; }
            idx = 0;
        }
        memcpy(buffer + idx, data, len);
    }

    void final(uint8_t* out) {
        size_t idx = (size_t)(count & 63);
        size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
        uint64_t bits = count * 8;
        uint8_t padding[128] = {0x80};
        update(padding, pad);
        uint8_t len_buf[8];
        for (int i = 0; i < 8; ++i) len_buf[i] = (uint8_t)(bits >> (56 - i*8));
        update(len_buf, 8);
        for (int i = 0; i < 5; ++i) {
            out[i*4]   = (uint8_t)(state[i] >> 24);
            out[i*4+1] = (uint8_t)(state[i] >> 16);
            out[i*4+2] = (uint8_t)(state[i] >> 8);
            out[i*4+3] = (uint8_t)(state[i]);
        }
    }
};

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64enc(const uint8_t* data, size_t len) {
    std::string r; r.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i+1 < len) v |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) v |= data[i+2];
        r += B64[(v >> 18) & 0x3F];
        r += B64[(v >> 12) & 0x3F];
        r += (i+1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        r += (i+2 < len) ? B64[v & 0x3F] : '=';
    }
    return r;
}

static std::string ws_accept_key(const std::string& key) {
    std::string s = key + "258EAFA5-E914-47DA-95CA-5AB9DC11B85B";
    uint8_t hash[20];
    SHA1Ctx sha1;
    sha1.init();
    sha1.update((const uint8_t*)s.data(), s.size());
    sha1.final(hash);
    return b64enc(hash, 20);
}

static bool read_fully(int fd, void* buf, size_t len) {
    uint8_t* p = (uint8_t*)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

static bool ws_send_frame(int fd, const void* data, size_t len, uint8_t opcode) {
    uint8_t hdr[14]; size_t hl;
    hdr[0] = 0x80 | opcode;
    if (len < 126) { hdr[1] = (uint8_t)len; hl = 2; }
    else if (len < 65536) {
        hdr[1] = 126; hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len; hl = 4;
    } else {
        hdr[1] = 127;
        uint64_t n = (uint64_t)len;
        for (int i = 7; i >= 0; --i) { hdr[2+i] = (uint8_t)(n & 0xFF); n >>= 8; }
        hl = 10;
    }
    struct iovec iov[2] = {{hdr, hl}, {(void*)data, len}};
    struct msghdr msg = {}; msg.msg_iov = iov; msg.msg_iovlen = 2;
    return sendmsg(fd, &msg, MSG_NOSIGNAL) == (ssize_t)(hl + len);
}

// Returns: -1 = error, -2 = close, 0 = control frame (no data), >0 = payload length
static int ws_recv_frame(int fd, uint8_t* buf, size_t cap) {
    uint8_t hdr[2];
    if (!read_fully(fd, hdr, 2)) return -1;
    bool mask = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2]; if (!read_fully(fd, ext, 2)) return -1;
        len = (uint64_t)ext[0] << 8 | ext[1];
    } else if (len == 127) {
        uint8_t ext[8]; if (!read_fully(fd, ext, 8)) return -1;
        len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    uint8_t mk[4] = {0};
    if (mask && !read_fully(fd, mk, 4)) return -1;
    if (len > cap) return -1;
    if (len > 0 && !read_fully(fd, buf, (size_t)len)) return -1;
    if (mask) for (uint64_t i = 0; i < len; ++i) buf[i] ^= mk[i & 3];
    if ((hdr[0] & 0x0F) == 0x8) return -2; // Close
    if ((hdr[0] & 0x0F) == 0x9) { // Ping -> Pong
        uint8_t resp[2] = {0x8A, (uint8_t)len};
        write(fd, resp, 2); if (len) write(fd, buf, (size_t)len);
        return 0;
    }
    if ((hdr[0] & 0x0F) == 0xA) return 0; // Pong
    return (int)len;
}

static void webserver_thread(int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("web socket"); return; }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("web bind"); close(sock); return; }
    if (listen(sock, 8) < 0) { perror("web listen"); close(sock); return; }
    std::printf("Web server listening on TCP %d\n", port);

    struct Client { int fd; bool ws; };
    std::vector<Client> clients;
    uint64_t last_status = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        // Build poll set
        std::vector<struct pollfd> pfds;
        pfds.push_back({sock, POLLIN, 0});
        for (auto& c : clients) pfds.push_back({c.fd, POLLIN, 0});

        int n = poll(pfds.data(), (nfds_t)pfds.size(), 200);
        if (n < 0) { if (errno == EINTR) continue; break; }

        // Accept new connections
        if (n > 0 && (pfds[0].revents & POLLIN)) {
            sockaddr_in ca{}; socklen_t sl = sizeof(ca);
            int cfd = accept(sock, (sockaddr*)&ca, &sl);
            if (cfd >= 0) {
                int fl = fcntl(cfd, F_GETFL, 0);
                fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
                clients.push_back({cfd, false});
            }
        }

        // Process existing clients
        std::vector<int> dead;
        for (size_t i = 0; i < clients.size(); ++i) {
            int pi = (int)i + 1; // index into pfds
            if (pi >= (int)pfds.size() || !(pfds[pi].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            int fd = clients[i].fd;
            if (clients[i].ws) {
                uint8_t buf[8];
                int r = ws_recv_frame(fd, buf, sizeof(buf));
                if (r == -2 || r == -1) { dead.push_back((int)i); continue; }
                if (r == 0) continue; // control frame
                if (r >= 8) {
                    HIDReport report;
                    memcpy(&report, buf, sizeof(HIDReport));
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_clients[WEB_CLIENT_IDX].active = true;
                    g_clients[WEB_CLIENT_IDX].report.p1 = report;
                    g_clients[WEB_CLIENT_IDX].report.p2.reset();
                    g_clients[WEB_CLIENT_IDX].report.p3.reset();
                    g_clients[WEB_CLIENT_IDX].report.p4.reset();
                    g_clients[WEB_CLIENT_IDX].last_rx_us = now_us();
                }
            } else {
                uint8_t rbuf[4096];
                ssize_t nr = read(fd, rbuf, sizeof(rbuf)-1);
                if (nr <= 0) { dead.push_back((int)i); continue; }
                rbuf[nr] = 0;
                std::string req((const char*)rbuf, nr);

                if (req.find("Upgrade: websocket") != std::string::npos ||
                    req.find("upgrade: websocket") != std::string::npos) {
                    auto kp = req.find("Sec-WebSocket-Key:");
                    if (kp == std::string::npos) kp = req.find("sec-websocket-key:");
                    if (kp != std::string::npos) {
                        kp = req.find(':', kp) + 1;
                        while (kp < req.size() && req[kp] == ' ') ++kp;
                        auto ke = req.find("\r\n", kp);
                        if (ke != std::string::npos) {
                            std::string key = req.substr(kp, ke - kp);
                            std::string accept = ws_accept_key(key);
                            std::string resp =
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                            write(fd, resp.data(), resp.size());
                            clients[i].ws = true;
                        } else { dead.push_back((int)i); }
                    } else { dead.push_back((int)i); }
                } else if (req.find("GET ") == 0) {
                    size_t slen = strlen(WEB_PAGE);
                    std::string resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Connection: close\r\n"
                        "Content-Length: " + std::to_string(slen) + "\r\n\r\n" + WEB_PAGE;
                    write(fd, resp.data(), resp.size());
                    dead.push_back((int)i);
                } else {
                    std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                    write(fd, resp.data(), resp.size());
                    dead.push_back((int)i);
                }
            }
        }

        // Cleanup dead clients (reverse order to maintain indices)
        std::sort(dead.begin(), dead.end(), std::greater<int>());
        for (int idx : dead) {
            if (idx >= 0 && idx < (int)clients.size()) {
                if (clients[idx].ws) {
                    // Reset web client state
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_clients[WEB_CLIENT_IDX].active = false;
                    g_clients[WEB_CLIENT_IDX].report.reset();
                }
                close(clients[idx].fd);
                clients.erase(clients.begin() + idx);
            }
        }
        dead.clear();

        // Broadcast status periodically
        uint64_t now = now_us();
        if (now - last_status > 100000) { // Every 100ms
            last_status = now;
            std::string json = "{\"type\":\"status\",\"clients\":[";
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (i > 0) json += ",";
                json += "{\"active\":" + std::string(g_clients[i].active ? "true" : "false") + "}";
            }
            json += "],\"pkts_rx\":" + std::to_string(g_pkts_rx.load()) + "}";
            for (auto& c : clients) {
                if (c.ws) ws_send_frame(c.fd, json.data(), json.size(), 1);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (auto& c : clients) close(c.fd);
    close(sock);
}

// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    bool        do_web    = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "-w")               do_web     = true;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-b ADDR] [-w] [--upnp] [-v]");
            return 0;
        }
    }

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    
    std::printf("UDP %s:%u writer=%d Hz\n",
                bind_addr.c_str(), port, WRITER_HZ);

    std::thread wt(writer_thread, WRITER_HZ);
    std::thread st(stats_thread);
    std::thread web;
    if (do_web) {
        web = std::thread(webserver_thread, port);
        std::printf("Web interface enabled on http://<ip>:%u\n", port);
    }

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    Packet pkt{};
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200 /*ms timeout*/);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen);

        if (bytes != (ssize_t)PACKET_SIZE) continue;

        // ── 1. Per-IP rate limiter ────────────────────────────────────────────────
        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip)) {
            if (g_verbose) puts("rate limit exceeded, dropped");
            continue;
        }

        // ── 2. Magic + version check ──────────────────────────────────────────────
        if (!packet_ok(pkt)) {
            if (g_verbose) puts("bad magic/version, dropped");
            continue;
        }

        // ── 3. Find Client Session or Pin new IP ──────────────────────────────────
        int client_idx = -1;
        uint64_t now = now_us();

        // Web client slot (index 4) is reserved; only UDP clients search slots 0-3
        static constexpr int UDP_SLOTS = MAX_CLIENTS - 1;
        
        for (int i = 0; i < UDP_SLOTS; ++i) {
            if (g_clients[i].active &&
                g_clients[i].addr.sin_addr.s_addr == src_ip &&
                g_clients[i].addr.sin_port == sender.sin_port) {
                client_idx = i;
                break;
            }
        }

        // If not found, assign to a free/timed-out slot
        if (client_idx == -1) {
            for (int i = 0; i < UDP_SLOTS; ++i) {
                if (!g_clients[i].active || (now - g_clients[i].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                    client_idx = i;
                    g_clients[i].active = true;
                    g_clients[i].addr = sender;
                    g_clients[i].first_pkt = true;
                    g_clients[i].report.reset();
                    if (g_verbose) std::printf("New PC accepted into Server Slot %d/4\n", i+1);
                    break;
                }
            }
        }

        // If all 4 slots are taken by active PCs, drop the packet
        if (client_idx == -1) {
            if (g_verbose) puts("server is full (4 PCs already active), dropped");
            continue;
        }

        // ── 4. HMAC authentication ────────────────────────────────────────────────
        if (hmac_verify(g_hmac_key, 32, (const uint8_t *)&pkt, PACKET_AUTH_SIZE, pkt.hmac, HMAC_TAG_SIZE) != 0) {
            if (g_verbose) puts("bad HMAC, dropped");
            continue;
        }

        // ── 5. Sequence counter (Anti-Replay) ─────────────────────────────────────
        bool is_reset = (pkt.flags & FLAG_RESET);
        bool sequence_jump = (g_clients[client_idx].expected_seq > pkt.seq) && ((g_clients[client_idx].expected_seq - pkt.seq) > 100);

        if (!g_clients[client_idx].first_pkt && pkt.seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
            if (g_verbose)
                std::printf("PC %d out-of-order seq=%u, dropped\n", client_idx+1, pkt.seq);
            continue;
        }
        g_clients[client_idx].first_pkt = false;
        g_clients[client_idx].expected_seq = pkt.seq + 1;

        // ── 6. Apply to shared state ──────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (is_reset) {
                g_clients[client_idx].report.reset();
            } else {
                g_clients[client_idx].report = pkt.report;
            }
            g_clients[client_idx].last_rx_us = now_us();
        }
        ++g_pkts_rx;
    }

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web.joinable()) web.join();
    return 0;
}