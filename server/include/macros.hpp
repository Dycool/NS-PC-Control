#pragma once

// Shared macro helpers for NS-PC-Control clients/server.

#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ns {
namespace macro {

// Grammar:
//   WAIT 100                         -> release macro inputs for 100ms
//   A 100                            -> hold A for 100ms
//   R+LSTICK_LEFT 450                -> hold R and steer left for 450ms
//   LOOP 200                         -> repeat the block since previous LOOP/start 200 times
// Accepted JSON:
//   {"name":"...","commands":"WAIT 100; A 100"}
//   {"name":"...","commands":["WAIT 100", "A 100"]}
//   ["WAIT 100", "A 100"]
inline constexpr std::size_t JSON_MAX_BYTES = 50ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t MAX_EXPANDED_STEPS = 1000000ULL;

inline constexpr std::uint32_t UDP_MAGIC       = 0x4E534D43u; // 'NSMC' legacy one-datagram upload
inline constexpr std::uint32_t UDP_CHUNK_MAGIC = 0x4E534D4Bu; // 'NSMK' chunked upload
inline constexpr std::size_t   UDP_TEXT_MAX    = JSON_MAX_BYTES;
inline constexpr std::size_t   UDP_CHUNK_MAX   = 1200;
inline constexpr std::uint8_t  CHUNK_FLAG_LAST = 0x01;

struct Step {
    std::uint16_t buttons = 0;
    std::uint8_t hat = ns::HAT_NEUTRAL;
    std::uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    bool has_lstick = false;
    bool has_rstick = false;
    std::uint32_t duration_ms = 0;
};

struct Entry {
    std::string name;
    std::string hotkey;
    std::string json;
};

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct MacroUdpHeaderWire {
    std::uint32_t magic;
    std::uint8_t version;
    std::uint8_t subpad;
    std::uint32_t text_len;
    std::uint32_t seq;
}
#ifndef _MSC_VER
__attribute__((packed))
#endif
;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

inline constexpr std::size_t UDP_HEADER_SIZE = sizeof(MacroUdpHeaderWire);
inline constexpr std::size_t udp_auth_size(std::size_t text_len) { return UDP_HEADER_SIZE + text_len; }

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct MacroUdpChunkHeaderWire {
    std::uint32_t magic;
    std::uint8_t version;
    std::uint8_t subpad;
    std::uint8_t flags;
    std::uint8_t reserved;
    std::uint32_t upload_id;
    std::uint32_t chunk_index;
    std::uint32_t chunk_count;
    std::uint32_t total_len;
    std::uint16_t chunk_len;
    std::uint32_t seq;
}
#ifndef _MSC_VER
__attribute__((packed))
#endif
;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

inline constexpr std::size_t CHUNK_HEADER_SIZE = sizeof(MacroUdpChunkHeaderWire);
static_assert(CHUNK_HEADER_SIZE == 30, "Macro chunk header must stay 30 bytes");

inline std::string& last_error_storage() {
    static thread_local std::string err;
    return err;
}

inline void set_error(const std::string& e) { last_error_storage() = e; }
inline const std::string& last_error() { return last_error_storage(); }

inline std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline bool is_hex4(const std::string& s, std::size_t pos) {
    if (pos + 4 > s.size()) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(s[pos + i]))) return false;
    }
    return true;
}

inline bool read_json_string_at(const std::string& raw, std::size_t& pos, std::string& out, std::string& err) {
    if (pos >= raw.size() || raw[pos] != '"') { err = "expected JSON string"; return false; }
    out.clear();
    ++pos;
    while (pos < raw.size()) {
        char c = raw[pos++];
        if (static_cast<unsigned char>(c) < 0x20) { err = "unescaped control character in JSON string"; return false; }
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (pos >= raw.size()) { err = "unfinished JSON escape"; return false; }
        char e = raw[pos++];
        switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                if (!is_hex4(raw, pos)) { err = "invalid JSON unicode escape"; return false; }
                // Macro commands are ASCII; preserve unicode names as '?' rather than failing the whole file.
                out += '?';
                pos += 4;
                break;
            default:
                err = "invalid JSON escape";
                return false;
        }
    }
    err = "unterminated JSON string";
    return false;
}

inline void skip_ws(const std::string& raw, std::size_t& pos) {
    while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) ++pos;
}

inline bool skip_json_value(const std::string& raw, std::size_t& pos, std::string& err);

inline bool skip_json_array(const std::string& raw, std::size_t& pos, std::string& err) {
    if (pos >= raw.size() || raw[pos] != '[') { err = "expected JSON array"; return false; }
    ++pos;
    skip_ws(raw, pos);
    if (pos < raw.size() && raw[pos] == ']') { ++pos; return true; }
    while (pos < raw.size()) {
        if (!skip_json_value(raw, pos, err)) return false;
        skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') { ++pos; skip_ws(raw, pos); continue; }
        if (pos < raw.size() && raw[pos] == ']') { ++pos; return true; }
        err = "expected ',' or ']' in JSON array";
        return false;
    }
    err = "unterminated JSON array";
    return false;
}

inline bool skip_json_object(const std::string& raw, std::size_t& pos, std::string& err) {
    if (pos >= raw.size() || raw[pos] != '{') { err = "expected JSON object"; return false; }
    ++pos;
    skip_ws(raw, pos);
    if (pos < raw.size() && raw[pos] == '}') { ++pos; return true; }
    while (pos < raw.size()) {
        std::string key;
        if (!read_json_string_at(raw, pos, key, err)) return false;
        skip_ws(raw, pos);
        if (pos >= raw.size() || raw[pos] != ':') { err = "expected ':' after JSON key"; return false; }
        ++pos;
        skip_ws(raw, pos);
        if (!skip_json_value(raw, pos, err)) return false;
        skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') { ++pos; skip_ws(raw, pos); continue; }
        if (pos < raw.size() && raw[pos] == '}') { ++pos; return true; }
        err = "expected ',' or '}' in JSON object";
        return false;
    }
    err = "unterminated JSON object";
    return false;
}

inline bool skip_json_value(const std::string& raw, std::size_t& pos, std::string& err) {
    skip_ws(raw, pos);
    if (pos >= raw.size()) { err = "missing JSON value"; return false; }
    if (raw[pos] == '"') { std::string tmp; return read_json_string_at(raw, pos, tmp, err); }
    if (raw[pos] == '{') return skip_json_object(raw, pos, err);
    if (raw[pos] == '[') return skip_json_array(raw, pos, err);
    if (raw.compare(pos, 4, "true") == 0) { pos += 4; return true; }
    if (raw.compare(pos, 5, "false") == 0) { pos += 5; return true; }
    if (raw.compare(pos, 4, "null") == 0) { pos += 4; return true; }
    if (raw[pos] == '-' || std::isdigit(static_cast<unsigned char>(raw[pos]))) {
        ++pos;
        while (pos < raw.size() &&
               (std::isdigit(static_cast<unsigned char>(raw[pos])) || raw[pos] == '.' || raw[pos] == 'e' ||
                raw[pos] == 'E' || raw[pos] == '+' || raw[pos] == '-')) ++pos;
        return true;
    }
    err = "invalid JSON value";
    return false;
}

inline bool extract_commands_text(const std::string& raw_in, std::string& out, std::string& err) {
    if (raw_in.size() > JSON_MAX_BYTES) { err = "macro JSON exceeds 50MB limit"; return false; }
    std::string raw = trim(raw_in);
    out.clear();
    if (raw.empty()) { err = "empty macro"; return false; }

    if (raw[0] != '{' && raw[0] != '[') { out = raw; return true; }

    std::size_t pos = 0;
    skip_ws(raw, pos);
    if (pos < raw.size() && raw[pos] == '[') {
        ++pos;
        skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ']') { err = "commands array is empty"; return false; }
        while (pos < raw.size()) {
            std::string item;
            if (!read_json_string_at(raw, pos, item, err)) return false;
            if (!out.empty()) out += ";";
            out += item;
            skip_ws(raw, pos);
            if (pos < raw.size() && raw[pos] == ',') { ++pos; skip_ws(raw, pos); continue; }
            if (pos < raw.size() && raw[pos] == ']') { ++pos; break; }
            err = "expected ',' or ']' in commands array";
            return false;
        }
        skip_ws(raw, pos);
        if (pos != raw.size()) { err = "extra data after JSON array"; return false; }
        return true;
    }

    if (pos >= raw.size() || raw[pos] != '{') { err = "macro JSON must be an object or commands array"; return false; }
    ++pos;
    skip_ws(raw, pos);
    bool found_commands = false;
    if (pos < raw.size() && raw[pos] == '}') { err = "macro object is missing commands"; return false; }
    while (pos < raw.size()) {
        std::string key;
        if (!read_json_string_at(raw, pos, key, err)) return false;
        skip_ws(raw, pos);
        if (pos >= raw.size() || raw[pos] != ':') { err = "expected ':' after JSON key"; return false; }
        ++pos;
        skip_ws(raw, pos);
        if (key == "commands") {
            found_commands = true;
            if (pos < raw.size() && raw[pos] == '"') {
                if (!read_json_string_at(raw, pos, out, err)) return false;
            } else if (pos < raw.size() && raw[pos] == '[') {
                ++pos;
                skip_ws(raw, pos);
                if (pos < raw.size() && raw[pos] == ']') { err = "commands array is empty"; return false; }
                while (pos < raw.size()) {
                    std::string item;
                    if (!read_json_string_at(raw, pos, item, err)) { err = "commands array must contain only strings"; return false; }
                    if (!out.empty()) out += ";";
                    out += item;
                    skip_ws(raw, pos);
                    if (pos < raw.size() && raw[pos] == ',') { ++pos; skip_ws(raw, pos); continue; }
                    if (pos < raw.size() && raw[pos] == ']') { ++pos; break; }
                    err = "expected ',' or ']' in commands array";
                    return false;
                }
            } else {
                err = "commands must be a string or an array of strings";
                return false;
            }
        } else {
            if (!skip_json_value(raw, pos, err)) return false;
        }
        skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') { ++pos; skip_ws(raw, pos); continue; }
        if (pos < raw.size() && raw[pos] == '}') { ++pos; break; }
        err = "expected ',' or '}' in macro object";
        return false;
    }
    skip_ws(raw, pos);
    if (pos != raw.size()) { err = "extra data after JSON object"; return false; }
    if (!found_commands) { err = "macro object is missing commands"; return false; }
    return true;
}

inline bool parse_uint32_strict(const std::string& s, std::uint32_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > 0xFFFFFFFFULL) return false;
    }
    if (v == 0) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

inline std::uint16_t button_bit(const std::string& token) {
    std::string name = upper(trim(token));
    if (name == "A" || name == "BTN_A") return ns::BTN_A;
    if (name == "B" || name == "BTN_B") return ns::BTN_B;
    if (name == "X" || name == "BTN_X") return ns::BTN_X;
    if (name == "Y" || name == "BTN_Y") return ns::BTN_Y;
    if (name == "L" || name == "BTN_L") return ns::BTN_L;
    if (name == "R" || name == "BTN_R") return ns::BTN_R;
    if (name == "ZL" || name == "BTN_ZL") return ns::BTN_ZL;
    if (name == "ZR" || name == "BTN_ZR") return ns::BTN_ZR;
    if (name == "MINUS" || name == "-" || name == "BTN_MINUS") return ns::BTN_MINUS;
    if (name == "PLUS" || name == "+" || name == "BTN_PLUS") return ns::BTN_PLUS;
    if (name == "LSTICK" || name == "LS" || name == "BTN_LSTICK") return ns::BTN_LSTICK;
    if (name == "RSTICK" || name == "RS" || name == "BTN_RSTICK") return ns::BTN_RSTICK;
    if (name == "HOME" || name == "GUIDE" || name == "BTN_HOME") return ns::BTN_HOME;
    if (name == "CAPTURE" || name == "SHARE" || name == "BTN_CAPTURE") return ns::BTN_CAPTURE;
    return 0;
}

inline bool apply_token(const std::string& raw_tok, Step& st, std::string& err,
                        bool& du, bool& dd, bool& dl, bool& dr,
                        bool& llu, bool& lld, bool& lll, bool& llr,
                        bool& rru, bool& rrd, bool& rrl, bool& rrr) {
    std::string tok = upper(trim(raw_tok));
    if (tok.empty()) return true;
    std::uint16_t bit = button_bit(tok);
    if (bit) { st.buttons |= bit; return true; }

    if (tok == "DPAD_UP" || tok == "DUP" || tok == "UP") { du = true; return true; }
    if (tok == "DPAD_DOWN" || tok == "DDOWN" || tok == "DOWN") { dd = true; return true; }
    if (tok == "DPAD_LEFT" || tok == "DLEFT" || tok == "LEFT") { dl = true; return true; }
    if (tok == "DPAD_RIGHT" || tok == "DRIGHT" || tok == "RIGHT") { dr = true; return true; }

    if (tok == "LSTICK_UP" || tok == "LS_UP") { llu = true; st.has_lstick = true; return true; }
    if (tok == "LSTICK_DOWN" || tok == "LS_DOWN") { lld = true; st.has_lstick = true; return true; }
    if (tok == "LSTICK_LEFT" || tok == "LS_LEFT") { lll = true; st.has_lstick = true; return true; }
    if (tok == "LSTICK_RIGHT" || tok == "LS_RIGHT") { llr = true; st.has_lstick = true; return true; }

    if (tok == "RSTICK_UP" || tok == "RS_UP") { rru = true; st.has_rstick = true; return true; }
    if (tok == "RSTICK_DOWN" || tok == "RS_DOWN") { rrd = true; st.has_rstick = true; return true; }
    if (tok == "RSTICK_LEFT" || tok == "RS_LEFT") { rrl = true; st.has_rstick = true; return true; }
    if (tok == "RSTICK_RIGHT" || tok == "RS_RIGHT") { rrr = true; st.has_rstick = true; return true; }

    err = "unknown macro input: " + raw_tok;
    return false;
}

inline bool parse_one_command(const std::string& part, Step& st, std::string& err) {
    std::size_t last_space = part.find_last_of(" \t");
    if (last_space == std::string::npos) { err = "missing duration in command: " + part; return false; }
    std::string cmd = trim(part.substr(0, last_space));
    std::string ms_s = trim(part.substr(last_space + 1));
    std::uint32_t ms = 0;
    if (!parse_uint32_strict(ms_s, ms)) { err = "invalid duration in command: " + part; return false; }
    st = Step{};
    st.duration_ms = ms;
    std::string up = upper(cmd);
    if (up == "WAIT") return true;
    if (cmd.empty()) { err = "missing input before duration in command: " + part; return false; }

    for (char& c : cmd) if (c == '+' || c == ',' || c == '|') c = ' ';
    std::istringstream iss(cmd);
    std::string tok;
    bool du = false, dd = false, dl = false, dr = false;
    bool llu = false, lld = false, lll = false, llr = false;
    bool rru = false, rrd = false, rrl = false, rrr = false;
    int token_count = 0;
    while (iss >> tok) {
        ++token_count;
        if (!apply_token(tok, st, err, du, dd, dl, dr, llu, lld, lll, llr, rru, rrd, rrl, rrr)) return false;
    }
    if (token_count == 0) { err = "empty input in command: " + part; return false; }
    if (du && dd) { err = "DPAD_UP and DPAD_DOWN conflict in command: " + part; return false; }
    if (dl && dr) { err = "DPAD_LEFT and DPAD_RIGHT conflict in command: " + part; return false; }
    if (llu && lld) { err = "LSTICK_UP and LSTICK_DOWN conflict in command: " + part; return false; }
    if (lll && llr) { err = "LSTICK_LEFT and LSTICK_RIGHT conflict in command: " + part; return false; }
    if (rru && rrd) { err = "RSTICK_UP and RSTICK_DOWN conflict in command: " + part; return false; }
    if (rrl && rrr) { err = "RSTICK_LEFT and RSTICK_RIGHT conflict in command: " + part; return false; }

    if (du && dr) st.hat = ns::HAT_NE;
    else if (du && dl) st.hat = ns::HAT_NW;
    else if (dd && dr) st.hat = ns::HAT_SE;
    else if (dd && dl) st.hat = ns::HAT_SW;
    else if (du) st.hat = ns::HAT_N;
    else if (dd) st.hat = ns::HAT_S;
    else if (dr) st.hat = ns::HAT_E;
    else if (dl) st.hat = ns::HAT_W;

    if (st.has_lstick) { st.lx = lll ? 0 : (llr ? 255 : 128); st.ly = llu ? 0 : (lld ? 255 : 128); }
    if (st.has_rstick) { st.rx = rrl ? 0 : (rrr ? 255 : 128); st.ry = rru ? 0 : (rrd ? 255 : 128); }
    return true;
}

inline bool validate_text(const std::string& raw_text, std::vector<Step>& steps,
                          std::vector<std::string>* normalized = nullptr) {
    last_error_storage().clear();
    steps.clear();
    if (normalized) normalized->clear();

    std::string text, err;
    if (!extract_commands_text(raw_text, text, err)) { set_error(err); return false; }
    for (char& c : text) if (c == '\n' || c == '\r') c = ';';

    std::size_t pos = 0;
    std::size_t loop_block_start = 0;
    while (pos < text.size()) {
        std::size_t semi = text.find(';', pos);
        std::string part = trim(text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        pos = (semi == std::string::npos) ? text.size() : semi + 1;
        if (part.empty() || part[0] == '#') continue;

        std::size_t last_space = part.find_last_of(" \t");
        std::string maybe_cmd = last_space == std::string::npos ? upper(part) : upper(trim(part.substr(0, last_space)));
        if (maybe_cmd == "LOOP") {
            if (last_space == std::string::npos) { set_error("missing count in LOOP command: " + part); return false; }
            std::uint32_t count = 0;
            if (!parse_uint32_strict(trim(part.substr(last_space + 1)), count)) { set_error("invalid LOOP count in command: " + part); return false; }
            if (steps.size() == loop_block_start) { set_error("LOOP has no previous commands to repeat: " + part); return false; }
            const std::size_t block_len = steps.size() - loop_block_start;
            if (count > 1 && block_len > (MAX_EXPANDED_STEPS - steps.size()) / (count - 1)) {
                set_error("LOOP expansion is too large; reduce LOOP count or split the macro");
                return false;
            }
            std::vector<Step> block(steps.begin() + static_cast<std::ptrdiff_t>(loop_block_start), steps.end());
            for (std::uint32_t i = 1; i < count; ++i) steps.insert(steps.end(), block.begin(), block.end());
            loop_block_start = steps.size();
            if (normalized) normalized->push_back(part);
            continue;
        }

        Step st;
        if (!parse_one_command(part, st, err)) { set_error(err); return false; }
        if (steps.size() + 1 > MAX_EXPANDED_STEPS) { set_error("macro expands to too many steps"); return false; }
        steps.push_back(st);
        if (normalized) normalized->push_back(part);
    }

    if (steps.empty()) { set_error("no valid macro commands found"); return false; }
    return true;
}

inline std::vector<Step> parse_text(const std::string& raw_text) {
    std::vector<Step> steps;
    validate_text(raw_text, steps, nullptr);
    return steps;
}

inline std::string read_text_file_limited(const std::string& path, std::string* err = nullptr) {
    if (err) err->clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { if (err) *err = "could not open file"; set_error("cannot open macro file"); return {}; }
    std::streamoff len = f.tellg();
    if (len < 0) { if (err) *err = "could not read file size"; set_error("cannot read macro file size"); return {}; }
    if (static_cast<std::uint64_t>(len) > JSON_MAX_BYTES) { if (err) *err = "macro JSON exceeds 50MB limit"; set_error("macro JSON exceeds 50MB limit"); return {}; }
    f.seekg(0, std::ios::beg);
    std::string raw(static_cast<std::size_t>(len), '\0');
    if (len > 0) f.read(&raw[0], len);
    if (!f && len > 0) { if (err) *err = "failed while reading macro file"; set_error("failed while reading macro file"); return {}; }
    return raw;
}

inline std::string escape_json(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += '?';
                else out += static_cast<char>(c);
                break;
        }
    }
    return out;
}

inline bool json_find_string_value(const std::string& raw, const std::string& key, std::string& out) {
    std::size_t pos = 0;
    std::string err;
    while (pos < raw.size()) {
        if (raw[pos] == '"') {
            std::size_t key_pos = pos;
            std::string k;
            if (!read_json_string_at(raw, pos, k, err)) return false;
            skip_ws(raw, pos);
            if (k == key && pos < raw.size() && raw[pos] == ':') {
                ++pos;
                skip_ws(raw, pos);
                if (pos < raw.size() && raw[pos] == '"') return read_json_string_at(raw, pos, out, err);
                return false;
            }
            pos = key_pos + 1;
        } else {
            ++pos;
        }
    }
    return false;
}

inline std::string extract_name_or_default(const std::string& raw, const std::string& fallback_name) {
    std::string name;
    if (json_find_string_value(raw, "name", name)) {
        name = trim(name);
        if (!name.empty()) return name;
    }
    return fallback_name;
}

enum class InvalidPrettyMode {
    ReturnRaw,
    FallbackWait
};

inline std::string pretty_json(const std::string& raw_text,
                               const std::string& fallback_name = "Macro",
                               InvalidPrettyMode invalid_mode = InvalidPrettyMode::ReturnRaw) {
    std::vector<Step> steps;
    std::vector<std::string> lines;
    if (!validate_text(raw_text, steps, &lines)) {
        if (invalid_mode == InvalidPrettyMode::ReturnRaw) return raw_text;
        lines = {"WAIT 200"};
    }
    std::string name = extract_name_or_default(raw_text, fallback_name);
    std::string out;
    out += "{\n";
    out += "  \"name\": \"" + escape_json(name) + "\",\n";
    out += "  \"commands\": [\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += "    \"" + escape_json(lines[i]) + "\"";
        if (i + 1 < lines.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}";
    return out;
}

inline std::string pretty_json_with_forced_name(const std::string& raw_text, const std::string& forced_name) {
    std::vector<Step> steps;
    std::vector<std::string> lines;
    if (!validate_text(raw_text, steps, &lines)) return raw_text;
    std::string out;
    out += "{\n";
    out += "  \"name\": \"" + escape_json(forced_name) + "\",\n";
    out += "  \"commands\": [\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += "    \"" + escape_json(lines[i]) + "\"";
        if (i + 1 < lines.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}";
    return out;
}

inline bool validate_to_pretty_json(const std::string& raw_text,
                                    std::string& pretty,
                                    std::string& err,
                                    const std::string& fallback_name = "Macro") {
    std::vector<Step> steps;
    if (!validate_text(raw_text, steps, nullptr)) { err = last_error(); return false; }
    pretty = pretty_json(raw_text, fallback_name);
    err.clear();
    return true;
}

inline std::uint64_t total_ms(const std::vector<Step>& steps) {
    std::uint64_t total = 0;
    for (const auto& st : steps) {
        if (std::numeric_limits<std::uint64_t>::max() - total < st.duration_ms) return std::numeric_limits<std::uint64_t>::max();
        total += st.duration_ms;
    }
    return total;
}

inline bool step_at(const std::vector<Step>& steps, std::uint64_t elapsed_ms, Step& out) {
    std::uint64_t cursor = 0;
    for (const auto& st : steps) {
        std::uint64_t next = cursor + st.duration_ms;
        if (elapsed_ms < next) { out = st; return true; }
        cursor = next;
    }
    return false;
}

inline bool report_at(const std::vector<Step>& steps, std::uint64_t elapsed_ms, ns::HIDReport& out) {
    out.reset();
    Step st{};
    if (!step_at(steps, elapsed_ms, st)) return false;
    out.buttons = st.buttons;
    out.hat = st.hat;
    if (st.has_lstick) { out.lx = st.lx; out.ly = st.ly; }
    if (st.has_rstick) { out.rx = st.rx; out.ry = st.ry; }
    return true;
}

using NormalizeHotkeyFn = std::string (*)(const std::string&);

inline std::string normalize_hotkey_or_trim(const std::string& s, NormalizeHotkeyFn normalize) {
    return normalize ? normalize(s) : trim(s);
}

inline bool find_json_array_range_for_key(const std::string& raw, const std::string& key,
                                          std::size_t& begin, std::size_t& end) {
    std::size_t pos = 0;
    std::string err;
    while (pos < raw.size()) {
        if (raw[pos] == '"') {
            std::string k;
            if (!read_json_string_at(raw, pos, k, err)) return false;
            skip_ws(raw, pos);
            if (k == key && pos < raw.size() && raw[pos] == ':') {
                ++pos;
                skip_ws(raw, pos);
                if (pos >= raw.size() || raw[pos] != '[') return false;
                begin = pos;
                int depth = 0;
                bool in_str = false;
                bool esc = false;
                for (; pos < raw.size(); ++pos) {
                    char c = raw[pos];
                    if (in_str) {
                        if (esc) esc = false;
                        else if (c == '\\') esc = true;
                        else if (c == '"') in_str = false;
                        continue;
                    }
                    if (c == '"') in_str = true;
                    else if (c == '[') ++depth;
                    else if (c == ']') {
                        if (--depth == 0) { end = pos; return true; }
                    }
                }
                return false;
            }
        } else {
            ++pos;
        }
    }
    return false;
}

inline std::vector<std::string> split_top_level_objects(const std::string& raw,
                                                        std::size_t begin,
                                                        std::size_t end) {
    std::vector<std::string> out;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    std::size_t obj_start = std::string::npos;
    for (std::size_t i = begin + 1; i < end; ++i) {
        char c = raw[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') {
            if (depth++ == 0) obj_start = i;
        } else if (c == '}') {
            if (--depth == 0 && obj_start != std::string::npos) {
                out.push_back(raw.substr(obj_start, i - obj_start + 1));
                obj_start = std::string::npos;
            }
        }
    }
    return out;
}

inline std::string entry_to_object_json(const Entry& e, NormalizeHotkeyFn normalize = nullptr, int indent_spaces = 4) {
    std::vector<Step> steps;
    std::vector<std::string> lines;
    if (!validate_text(e.json, steps, &lines)) lines = {"WAIT 200"};
    std::string pad(static_cast<std::size_t>(indent_spaces), ' ');
    std::string pad2(static_cast<std::size_t>(indent_spaces + 2), ' ');
    std::string name = trim(e.name).empty() ? extract_name_or_default(e.json, "Macro") : e.name;
    std::string out;
    out += pad + "{\n";
    out += pad2 + "\"name\": \"" + escape_json(name) + "\",\n";
    out += pad2 + "\"hotkey\": \"" + escape_json(normalize_hotkey_or_trim(e.hotkey, normalize)) + "\",\n";
    out += pad2 + "\"commands\": [\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += pad2 + "  \"" + escape_json(lines[i]) + "\"";
        if (i + 1 < lines.size()) out += ",";
        out += "\n";
    }
    out += pad2 + "]\n";
    out += pad + "}";
    return out;
}

inline std::string entries_to_json(const std::vector<Entry>& entries, NormalizeHotkeyFn normalize = nullptr) {
    std::string out;
    out += "{\n";
    out += "  \"macros\": [\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out += entry_to_object_json(entries[i], normalize, 4);
        if (i + 1 < entries.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

inline bool parse_entries_text(const std::string& raw,
                               std::vector<Entry>& out,
                               std::string& err,
                               NormalizeHotkeyFn normalize = nullptr) {
    out.clear();
    err.clear();
    if (raw.size() > JSON_MAX_BYTES) { err = "macro JSON exceeds 50MB limit"; return false; }
    std::string t = trim(raw);
    if (t.empty()) return true;

    std::size_t arr_begin = 0, arr_end = 0;
    if (find_json_array_range_for_key(t, "macros", arr_begin, arr_end)) {
        auto objects = split_top_level_objects(t, arr_begin, arr_end);
        for (const std::string& obj : objects) {
            std::string pretty;
            if (!validate_to_pretty_json(obj, pretty, err, "Macro")) return false;
            Entry e;
            e.json = pretty;
            e.name = extract_name_or_default(obj, "Macro");
            json_find_string_value(obj, "hotkey", e.hotkey);
            e.hotkey = normalize_hotkey_or_trim(e.hotkey, normalize);
            out.push_back(std::move(e));
        }
        return true;
    }

    std::string pretty;
    if (!validate_to_pretty_json(t, pretty, err, "Macro")) return false;
    Entry e;
    e.json = pretty;
    e.name = extract_name_or_default(t, "Macro");
    json_find_string_value(t, "hotkey", e.hotkey);
    e.hotkey = normalize_hotkey_or_trim(e.hotkey, normalize);
    out.push_back(std::move(e));
    return true;
}

struct RecordFrame {
    std::uint16_t buttons = 0;
    std::uint8_t hat = ns::HAT_NEUTRAL;
    std::int8_t lx = 0, ly = 0, rx = 0, ry = 0;
};

inline bool operator==(const RecordFrame& a, const RecordFrame& b) {
    return a.buttons == b.buttons && a.hat == b.hat &&
           a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

inline bool operator!=(const RecordFrame& a, const RecordFrame& b) { return !(a == b); }

inline std::string buttons_to_text(std::uint16_t buttons) {
    struct BtnName { std::uint16_t bit; const char* name; };
    static const BtnName names[] = {
        {ns::BTN_A, "A"}, {ns::BTN_B, "B"}, {ns::BTN_X, "X"}, {ns::BTN_Y, "Y"},
        {ns::BTN_L, "L"}, {ns::BTN_R, "R"}, {ns::BTN_ZL, "ZL"}, {ns::BTN_ZR, "ZR"},
        {ns::BTN_MINUS, "MINUS"}, {ns::BTN_PLUS, "PLUS"}, {ns::BTN_LSTICK, "LSTICK"},
        {ns::BTN_RSTICK, "RSTICK"}, {ns::BTN_HOME, "HOME"}, {ns::BTN_CAPTURE, "CAPTURE"}
    };
    std::string out;
    for (const auto& n : names) {
        if (buttons & n.bit) {
            if (!out.empty()) out += "+";
            out += n.name;
        }
    }
    return out;
}

inline RecordFrame record_frame_from_report(const ns::HIDReport& report) {
    auto axis_dir = [](std::uint8_t v) -> std::int8_t {
        if (v < 80) return -1;
        if (v > 176) return 1;
        return 0;
    };
    RecordFrame f{};
    f.buttons = report.buttons;
    f.hat = report.hat;
    f.lx = axis_dir(report.lx);
    f.ly = axis_dir(report.ly);
    f.rx = axis_dir(report.rx);
    f.ry = axis_dir(report.ry);
    return f;
}

inline void append_token(std::string& out, const char* token) {
    if (!out.empty()) out += "+";
    out += token;
}

inline std::string record_frame_to_text(const RecordFrame& f) {
    std::string out = buttons_to_text(f.buttons);
    switch (f.hat) {
        case ns::HAT_N:  append_token(out, "DPAD_UP"); break;
        case ns::HAT_NE: append_token(out, "DPAD_UP"); append_token(out, "DPAD_RIGHT"); break;
        case ns::HAT_E:  append_token(out, "DPAD_RIGHT"); break;
        case ns::HAT_SE: append_token(out, "DPAD_DOWN"); append_token(out, "DPAD_RIGHT"); break;
        case ns::HAT_S:  append_token(out, "DPAD_DOWN"); break;
        case ns::HAT_SW: append_token(out, "DPAD_DOWN"); append_token(out, "DPAD_LEFT"); break;
        case ns::HAT_W:  append_token(out, "DPAD_LEFT"); break;
        case ns::HAT_NW: append_token(out, "DPAD_UP"); append_token(out, "DPAD_LEFT"); break;
        default: break;
    }
    if (f.lx < 0) append_token(out, "LSTICK_LEFT");
    else if (f.lx > 0) append_token(out, "LSTICK_RIGHT");
    if (f.ly < 0) append_token(out, "LSTICK_UP");
    else if (f.ly > 0) append_token(out, "LSTICK_DOWN");
    if (f.rx < 0) append_token(out, "RSTICK_LEFT");
    else if (f.rx > 0) append_token(out, "RSTICK_RIGHT");
    if (f.ry < 0) append_token(out, "RSTICK_UP");
    else if (f.ry > 0) append_token(out, "RSTICK_DOWN");
    return out;
}

struct Recorder {
    bool recording = false;
    RecordFrame last_frame{};
    bool have_frame = false;
    bool has_input = false;
    std::uint64_t last_change_us = 0;
    std::string commands;

    void start(std::uint64_t now_us) {
        recording = true;
        last_frame = RecordFrame{};
        have_frame = false;
        has_input = false;
        last_change_us = now_us;
        commands.clear();
    }

    void append(const RecordFrame& frame, std::uint64_t duration_ms) {
        if (duration_ms < 10) return;
        if (!commands.empty()) commands += "; ";
        std::string combo = record_frame_to_text(frame);
        if (combo.empty()) {
            commands += "WAIT " + std::to_string(duration_ms);
        } else {
            has_input = true;
            commands += combo + " " + std::to_string(duration_ms);
        }
    }

    void sample(const ns::HIDReport& report, std::uint64_t now_us, bool macro_playback_running = false) {
        if (!recording || macro_playback_running) return;
        RecordFrame frame = record_frame_from_report(report);
        if (!have_frame) {
            last_frame = frame;
            have_frame = true;
            last_change_us = now_us;
            return;
        }
        if (frame != last_frame) {
            append(last_frame, (now_us - last_change_us) / 1000ULL);
            last_frame = frame;
            last_change_us = now_us;
        }
    }

    std::string stop(std::uint64_t now_us) {
        if (recording && have_frame) append(last_frame, (now_us - last_change_us) / 1000ULL);
        recording = false;
        have_frame = false;
        if (!has_input) {
            commands.clear();
            return "";
        }
        return pretty_json(commands, "Recorded Macro");
    }
};

struct Runtime {
    std::vector<Step> steps;
    bool running = false;
    std::uint64_t start_us = 0;
};

inline void runtime_start(Runtime& rt, std::vector<Step> parsed_steps, std::uint64_t now_us) {
    rt.steps = std::move(parsed_steps);
    rt.running = true;
    rt.start_us = now_us;
}

inline bool runtime_start_text(Runtime& rt, const std::string& raw_text, std::uint64_t now_us) {
    std::vector<Step> steps;
    if (!validate_text(raw_text, steps, nullptr)) return false;
    runtime_start(rt, std::move(steps), now_us);
    return true;
}

inline bool runtime_running(Runtime& rt, std::uint64_t now_us, std::uint64_t grace_ms = 120) {
    if (!rt.running) return false;
    std::uint64_t elapsed_ms = (now_us - rt.start_us) / 1000ULL;
    if (elapsed_ms > total_ms(rt.steps) + grace_ms) { rt.running = false; return false; }
    return true;
}

inline bool runtime_step(Runtime& rt, std::uint64_t now_us, Step& out) {
    if (!rt.running) return false;
    std::uint64_t elapsed_ms = (now_us - rt.start_us) / 1000ULL;
    if (!step_at(rt.steps, elapsed_ms, out)) { rt.running = false; return false; }
    return true;
}

inline bool runtime_report(Runtime& rt, std::uint64_t now_us, ns::HIDReport& out) {
    if (!rt.running) return false;
    std::uint64_t elapsed_ms = (now_us - rt.start_us) / 1000ULL;
    bool active = report_at(rt.steps, elapsed_ms, out);
    if (!active) rt.running = false;
    return active;
}

} // namespace macro
} // namespace ns
