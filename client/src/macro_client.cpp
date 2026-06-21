#include "macro_client.hpp"
#include "input_settings.hpp"
#include "shared/sha256.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

uint32_t g_macro_udp_seq = 0;

uint32_t next_macro_upload_id() {
    uint32_t v = ++g_macro_udp_seq;
    if (v == 0) v = ++g_macro_udp_seq;
    return v;
}

bool send_macro_udp_packet(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32],
                                  const std::string& json_or_commands, uint8_t subpad) {
    if (json_or_commands.size() > ns::macro::UDP_TEXT_MAX) return false;
    if (json_or_commands.size() + ns::macro::UDP_HEADER_SIZE + ns::HMAC_TAG_SIZE <= 1400) {
        std::vector<uint8_t> buf(ns::macro::UDP_HEADER_SIZE + json_or_commands.size() + ns::HMAC_TAG_SIZE);
        ns::macro::MacroUdpHeaderWire hdr{};
        hdr.magic = ns::macro::UDP_MAGIC;
        hdr.version = ns::PROTO_VERSION;
        hdr.subpad = subpad;
        hdr.text_len = (uint32_t)json_or_commands.size();
        hdr.seq = next_macro_upload_id();
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        if (!json_or_commands.empty()) {
            std::memcpy(buf.data() + sizeof(hdr), json_or_commands.data(), json_or_commands.size());
        }
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, buf.data(), sizeof(hdr) + json_or_commands.size(), full_hmac);
        std::memcpy(buf.data() + sizeof(hdr) + json_or_commands.size(), full_hmac, ns::HMAC_TAG_SIZE);
        return send_all_udp(sock, dest, buf.data(), buf.size()) != SOCKET_ERROR;
    }

    const uint32_t upload_id = next_macro_upload_id();
    const uint32_t chunk_count = (uint32_t)((json_or_commands.size() + ns::macro::UDP_CHUNK_MAX - 1) / ns::macro::UDP_CHUNK_MAX);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        size_t off = (size_t)i * ns::macro::UDP_CHUNK_MAX;
        size_t n = std::min(ns::macro::UDP_CHUNK_MAX, json_or_commands.size() - off);
        std::vector<uint8_t> buf(ns::macro::CHUNK_HEADER_SIZE + n + ns::HMAC_TAG_SIZE);
        ns::macro::MacroUdpChunkHeaderWire hdr{};
        hdr.magic = ns::macro::UDP_CHUNK_MAGIC;
        hdr.version = ns::PROTO_VERSION;
        hdr.subpad = subpad;
        hdr.flags = (i + 1 == chunk_count) ? 0x01 : 0x00;
        hdr.reserved = 0;
        hdr.upload_id = upload_id;
        hdr.chunk_index = i;
        hdr.chunk_count = chunk_count;
        hdr.total_len = (uint32_t)json_or_commands.size();
        hdr.chunk_len = (uint16_t)n;
        hdr.seq = next_macro_upload_id();
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        if (n > 0) std::memcpy(buf.data() + sizeof(hdr), json_or_commands.data() + off, n);
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, buf.data(), ns::macro::CHUNK_HEADER_SIZE + n, full_hmac);
        std::memcpy(buf.data() + ns::macro::CHUNK_HEADER_SIZE + n, full_hmac, ns::HMAC_TAG_SIZE);
        if (send_all_udp(sock, dest, buf.data(), buf.size()) == SOCKET_ERROR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::mutex g_macro_mtx;
std::vector<ns::macro::Step> g_macro_steps;
bool g_macro_running = false;
uint64_t g_macro_start_us = 0;
std::string g_macro_upload_pending;
std::vector<ns::macro::Entry> g_macro_entries;
std::unordered_map<std::string, bool> g_macro_hotkey_down;

int find_macro_entry_by_name(const std::string& name) {
    std::string wanted = ns::macro::upper(ns::macro::trim(name));
    if (wanted.empty()) return -1;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (ns::macro::upper(g_macro_entries[i].name) == wanted) return i;
    }
    return -1;
}

std::string unique_macro_name(const std::string& base_raw) {
    std::string base = ns::macro::trim(base_raw);
    if (base.empty()) base = "Recorded Macro";
    std::string name = base;
    int suffix = 2;
    while (find_macro_entry_by_name(name) >= 0) name = base + " " + std::to_string(suffix++);
    return name;
}

bool macro_hotkey_conflicts(const std::string& hotkey, std::string* conflict_name) {
    std::string hk = normalize_key_name(hotkey);
    if (hk.empty()) return false;
    for (const auto& kv : g_keyBindings) {
        if (normalize_key_name(kv.second) == hk) {
            if (conflict_name) *conflict_name = kv.first;
            return true;
        }
    }
    return false;
}

bool macro_entry_hotkey_conflicts(const std::string& hotkey, int skip_index, std::string* conflict_name) {
    std::string hk = normalize_key_name(hotkey);
    if (hk.empty()) return false;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (i == skip_index) continue;
        if (normalize_key_name(g_macro_entries[i].hotkey) == hk) {
            if (conflict_name) *conflict_name = g_macro_entries[i].name.empty() ? "another macro" : g_macro_entries[i].name;
            return true;
        }
    }
    return false;
}

void rebuild_macro_hotkey_state() {
    g_macro_hotkey_down.clear();
    for (const auto& e : g_macro_entries) {
        std::string hk = normalize_key_name(e.hotkey);
        if (!hk.empty()) g_macro_hotkey_down[hk] = false;
    }
}

void load_macro_entries() {
    std::string err;
    std::vector<ns::macro::Entry> loaded;
    std::string raw = ns::macro::read_text_file_limited(macros_path(), &err);
    if (!ns::macro::parse_entries_text(raw, loaded, err, normalize_macro_hotkey_for_io)) loaded.clear();
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_entries = std::move(loaded);
    rebuild_macro_hotkey_state();
}

bool save_macro_entries_to_disk() {
    std::string json;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        json = ns::macro::entries_to_json(g_macro_entries, normalize_macro_hotkey_for_io);
    }
    if (json.size() > ns::macro::JSON_MAX_BYTES) return false;
    std::ofstream f(macros_path(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(json.data(), (std::streamsize)json.size());
    return (bool)f;
}

bool start_macro_text(const std::string& txt, std::string* err_out) {
    std::vector<ns::macro::Step> parsed;
    if (!ns::macro::validate_text(txt, parsed, nullptr)) {
        if (err_out) *err_out = ns::macro::last_error();
        return false;
    }
    std::string pretty, err;
    if (!ns::macro::validate_to_pretty_json(txt, pretty, err, "Macro")) {
        if (err_out) *err_out = err;
        return false;
    }
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_upload_pending = pretty;
    g_macro_steps = std::move(parsed);
    g_macro_running = false;
    g_macro_start_us = ns::now_us();
    return true;
}

bool upsert_macro_entry(ns::macro::Entry e, bool force_unique_name, std::string* err_out) {
    std::string pretty, err;
    if (!ns::macro::validate_to_pretty_json(e.json, pretty, err, e.name.empty() ? "Macro" : e.name)) {
        if (err_out) *err_out = err;
        return false;
    }
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    e.name = ns::macro::trim(e.name);
    if (e.name.empty()) e.name = ns::macro::extract_name_or_default(pretty, "Macro");
    if (force_unique_name) e.name = unique_macro_name(e.name);
    e.hotkey = normalize_key_name(e.hotkey);
    e.json = ns::macro::pretty_json_with_forced_name(pretty, e.name);
    int existing = force_unique_name ? -1 : find_macro_entry_by_name(e.name);
    std::string conflict;
    if (macro_hotkey_conflicts(e.hotkey, &conflict) || macro_entry_hotkey_conflicts(e.hotkey, existing, &conflict)) {
        e.hotkey.clear();
    }
    if (existing >= 0) g_macro_entries[existing] = std::move(e);
    else g_macro_entries.push_back(std::move(e));
    rebuild_macro_hotkey_state();
    return true;
}

void poll_macro_entry_hotkeys() {
    std::vector<std::string> to_run;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        for (const auto& e : g_macro_entries) {
            std::string hk = normalize_key_name(e.hotkey);
            if (hk.empty()) continue;
            if (macro_hotkey_conflicts(hk, nullptr)) continue;
            bool down = key_is_down(hk);
            bool was_down = g_macro_hotkey_down[hk];
            g_macro_hotkey_down[hk] = down;
            if (down && !was_down) to_run.push_back(e.json);
        }
    }
    for (const auto& json : to_run) start_macro_text(json, nullptr);
}

ns::macro::Recorder g_macro_recorder;

void macro_record_start() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recorder.start(ns::now_us());
}

std::string macro_record_stop() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    return g_macro_recorder.stop(ns::now_us());
}

void macro_record_sample(const ns::HIDReport& report) {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recorder.sample(report, ns::now_us(), g_macro_running);
}

bool poll_macro_record_p1(ns::HIDReport& report) {
    report.reset();
    auto sdl = g_sdlInput.snapshot();
    if (sdl[0].connected) {
        report = sdl[0].input;
        return true;
    }
    int km = g_keyboardMode.load();
    if (km != KB_OFF) {
        apply_keyboard_to_report(report, km == KB_OVERRIDE);
        return true;
    }
    return false;
}

void macro_record_sample_p1() {
    ns::HIDReport report;
    poll_macro_record_p1(report);
    macro_record_sample(report);
}

bool apply_macro_override(ns::HIDReport logical_reports[4], bool present[4], bool has_motion[4]) {
    (void)has_motion;
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_running) return false;
    uint64_t elapsed_ms = (ns::now_us() - g_macro_start_us) / 1000ULL;
    ns::HIDReport mr;
    bool active = ns::macro::report_at(g_macro_steps, elapsed_ms, mr);
    for (int i = 0; i < 4; ++i) {
        logical_reports[i].reset();
        present[i] = false;
        has_motion[i] = false;
    }
    logical_reports[0] = mr;
    present[0] = true;
    if (!active && elapsed_ms > ns::macro::total_ms(g_macro_steps) + 120) g_macro_running = false;
    return true;
}
