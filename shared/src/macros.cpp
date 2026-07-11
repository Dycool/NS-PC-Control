#include "shared/macros.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace ns {
namespace macro {

std::string& last_error_storage() {
    static thread_local std::string err;
    return err;
}

void set_error(const std::string& e) { last_error_storage() = e; }
const std::string& last_error() { return last_error_storage(); }

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}



std::expected<std::string, std::string> extract_commands_text(const std::string& raw_in) {
    std::string out;
    if (raw_in.size() > JSON_MAX_BYTES) { return std::unexpected("macro JSON exceeds 50MB limit"); }
    std::string raw = trim(raw_in);
    out.clear();
    if (raw.empty()) { return std::unexpected("empty macro"); }

    if (raw[0] != '{' && raw[0] != '[') { out = raw; return out; }

    try {
        json j = json::parse(raw);
        if (j.is_array()) {
            if (j.empty()) { return std::unexpected("commands array is empty"); }
            for (const auto& item : j) {
                if (!item.is_string()) { return std::unexpected("commands array must contain only strings"); }
                if (!out.empty()) out += ";";
                out += item.get<std::string>();
            }
            return out;
        } else if (j.is_object()) {
            if (!j.contains("commands")) { return std::unexpected("macro object is missing commands"); }
            auto& cmds = j["commands"];
            if (cmds.is_string()) {
                out = cmds.get<std::string>();
            } else if (cmds.is_array()) {
                if (cmds.empty()) { return std::unexpected("commands array is empty"); }
                for (const auto& item : cmds) {
                    if (!item.is_string()) { return std::unexpected("commands array must contain only strings"); }
                    if (!out.empty()) out += ";";
                    out += item.get<std::string>();
                }
            } else {
                return std::unexpected("commands must be a string or an array of strings");
            }
            return out;
        }
        return std::unexpected("macro JSON must be an object or commands array");
    } catch (const json::exception& e) {
        return std::unexpected("JSON parse error: " + std::string(e.what()));
    }
}

bool parse_uint32_strict(const std::string& s, std::uint32_t& out) {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
        if (v > 0xFFFFFFFFULL) return false;
    }
    // Allow zero-duration steps (WAIT 0 / BTN 0) for parser consistency.
    out = static_cast<std::uint32_t>(v);
    return true;
}


std::uint16_t button_bit(const std::string& token) {
    struct TokenMap { std::string_view key; std::uint16_t bit; };
    static constexpr TokenMap BUTTONS[] = {
        {"A", ns::BTN_A}, {"BTN_A", ns::BTN_A},
        {"B", ns::BTN_B}, {"BTN_B", ns::BTN_B},
        {"X", ns::BTN_X}, {"BTN_X", ns::BTN_X},
        {"Y", ns::BTN_Y}, {"BTN_Y", ns::BTN_Y},
        {"L", ns::BTN_L}, {"BTN_L", ns::BTN_L},
        {"R", ns::BTN_R}, {"BTN_R", ns::BTN_R},
        {"ZL", ns::BTN_ZL}, {"BTN_ZL", ns::BTN_ZL},
        {"ZR", ns::BTN_ZR}, {"BTN_ZR", ns::BTN_ZR},
        {"MINUS", ns::BTN_MINUS}, {"-", ns::BTN_MINUS}, {"BTN_MINUS", ns::BTN_MINUS},
        {"PLUS", ns::BTN_PLUS}, {"+", ns::BTN_PLUS}, {"BTN_PLUS", ns::BTN_PLUS},
        {"LSTICK", ns::BTN_LSTICK}, {"LS", ns::BTN_LSTICK}, {"BTN_LSTICK", ns::BTN_LSTICK},
        {"RSTICK", ns::BTN_RSTICK}, {"RS", ns::BTN_RSTICK}, {"BTN_RSTICK", ns::BTN_RSTICK},
        {"HOME", ns::BTN_HOME}, {"GUIDE", ns::BTN_HOME}, {"BTN_HOME", ns::BTN_HOME},
        {"CAPTURE", ns::BTN_CAPTURE}, {"SHARE", ns::BTN_CAPTURE}, {"BTN_CAPTURE", ns::BTN_CAPTURE}
    };
    std::string name = upper(trim(token));
    for (const auto& entry : BUTTONS) {
        if (entry.key == name) return entry.bit;
    }
    return 0;
}

static std::uint8_t extra_button_bit(const std::string& token) {
    struct TokenMap { std::string_view key; std::uint8_t bit; };
    static constexpr TokenMap BUTTONS[] = {
        {"C", ns::EXT_BUTTON_C}, {"BTN_C", ns::EXT_BUTTON_C},
        {"GL", ns::EXT_BUTTON_GL}, {"BTN_GL", ns::EXT_BUTTON_GL},
        {"GR", ns::EXT_BUTTON_GR}, {"BTN_GR", ns::EXT_BUTTON_GR},
        {"SL", ns::EXT_BUTTON_SL}, {"BTN_SL", ns::EXT_BUTTON_SL},
        {"SR", ns::EXT_BUTTON_SR}, {"BTN_SR", ns::EXT_BUTTON_SR},
    };
    const std::string name = upper(trim(token));
    for (const auto& entry : BUTTONS) {
        if (entry.key == name) return entry.bit;
    }
    return 0;
}

std::expected<void, std::string> apply_token(const std::string& raw_tok, Step& st,
                        bool& du, bool& dd, bool& dl, bool& dr,
                        bool& llu, bool& lld, bool& lll, bool& llr,
                        bool& rru, bool& rrd, bool& rrl, bool& rrr) {
    std::string tok = upper(trim(raw_tok));
    if (tok.empty()) return {};
    std::uint16_t bit = button_bit(tok);
    if (bit) { st.buttons |= bit; return {}; }
    std::uint8_t extra_bit = extra_button_bit(tok);
    if (extra_bit) { st.extra_buttons |= extra_bit; return {}; }

    struct ActionMap {
        std::string_view key;
        bool& target;
        bool* has_stick = nullptr;
    };
    const ActionMap ACTIONS[] = {
        {"DPAD_UP", du}, {"DUP", du}, {"UP", du},
        {"DPAD_DOWN", dd}, {"DDOWN", dd}, {"DOWN", dd},
        {"DPAD_LEFT", dl}, {"DLEFT", dl}, {"LEFT", dl},
        {"DPAD_RIGHT", dr}, {"DRIGHT", dr}, {"RIGHT", dr},
        {"LSTICK_UP", llu, &st.has_lstick}, {"LS_UP", llu, &st.has_lstick},
        {"LSTICK_DOWN", lld, &st.has_lstick}, {"LS_DOWN", lld, &st.has_lstick},
        {"LSTICK_LEFT", lll, &st.has_lstick}, {"LS_LEFT", lll, &st.has_lstick},
        {"LSTICK_RIGHT", llr, &st.has_lstick}, {"LS_RIGHT", llr, &st.has_lstick},
        {"RSTICK_UP", rru, &st.has_rstick}, {"RS_UP", rru, &st.has_rstick},
        {"RSTICK_DOWN", rrd, &st.has_rstick}, {"RS_DOWN", rrd, &st.has_rstick},
        {"RSTICK_LEFT", rrl, &st.has_rstick}, {"RS_LEFT", rrl, &st.has_rstick},
        {"RSTICK_RIGHT", rrr, &st.has_rstick}, {"RS_RIGHT", rrr, &st.has_rstick}
    };

    for (const auto& action : ACTIONS) {
        if (action.key == tok) {
            action.target = true;
            if (action.has_stick) *action.has_stick = true;
            return {};
        }
    }

    return std::unexpected("unknown macro input: " + raw_tok);
}

std::expected<void, std::string> parse_one_command(const std::string& part, Step& st) {
    std::size_t last_space = part.find_last_of(" \t");
    if (last_space == std::string::npos) return std::unexpected("missing duration in command: " + part);
    std::string cmd = trim(part.substr(0, last_space));
    std::string ms_s = trim(part.substr(last_space + 1));
    std::uint32_t ms = 0;
    if (!parse_uint32_strict(ms_s, ms)) return std::unexpected("invalid duration in command: " + part);
    st = Step{};
    st.duration_ms = ms;
    std::string up = upper(cmd);
    if (up == "WAIT") return {};
    if (cmd.empty()) return std::unexpected("missing input before duration in command: " + part);

    for (char& c : cmd) if (c == '+' || c == ',' || c == '|') c = ' ';
    std::istringstream iss(cmd);
    std::string tok;
    bool du = false, dd = false, dl = false, dr = false;
    bool llu = false, lld = false, lll = false, llr = false;
    bool rru = false, rrd = false, rrl = false, rrr = false;
    int token_count = 0;
    while (iss >> tok) {
        ++token_count;
        auto apply_res = apply_token(tok, st, du, dd, dl, dr, llu, lld, lll, llr, rru, rrd, rrl, rrr);
        if (!apply_res) return std::unexpected(apply_res.error());
    }
    if (token_count == 0) return std::unexpected("empty input in command: " + part);
    if (du && dd) return std::unexpected("DPAD_UP and DPAD_DOWN conflict in command: " + part);
    if (dl && dr) return std::unexpected("DPAD_LEFT and DPAD_RIGHT conflict in command: " + part);
    if (llu && lld) return std::unexpected("LSTICK_UP and LSTICK_DOWN conflict in command: " + part);
    if (lll && llr) return std::unexpected("LSTICK_LEFT and LSTICK_RIGHT conflict in command: " + part);
    if (rru && rrd) return std::unexpected("RSTICK_UP and RSTICK_DOWN conflict in command: " + part);
    if (rrl && rrr) return std::unexpected("RSTICK_LEFT and RSTICK_RIGHT conflict in command: " + part);

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
    return {};
}

bool validate_text(const std::string& raw_text, std::vector<Step>& steps,
                          std::vector<std::string>* normalized) {
    last_error_storage().clear();
    steps.clear();
    if (normalized) normalized->clear();

    std::string text;
    auto ext_res = extract_commands_text(raw_text);
    if (!ext_res) { set_error(ext_res.error()); return false; }
    text = *ext_res;
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
        auto parse_res = parse_one_command(part, st);
        if (!parse_res) { set_error(parse_res.error()); return false; }
        if (steps.size() + 1 > MAX_EXPANDED_STEPS) { set_error("macro expands to too many steps"); return false; }
        steps.push_back(st);
        if (normalized) normalized->push_back(part);
    }

    if (steps.empty()) { set_error("no valid macro commands found"); return false; }
    return true;
}

std::vector<Step> parse_text(const std::string& raw_text) {
    std::vector<Step> steps;
    validate_text(raw_text, steps, nullptr);
    return steps;
}

std::string read_text_file_limited(const std::string& path, std::string* err) {
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

std::string extract_name_or_default(const std::string& raw, const std::string& fallback_name) {
    std::string raw_trim = trim(raw);
    if (raw_trim.empty() || (raw_trim[0] != '{' && raw_trim[0] != '[')) return fallback_name;
    try {
        json j = json::parse(raw_trim);
        if (j.is_object() && j.contains("name") && j["name"].is_string()) {
            std::string name = trim(j["name"].get<std::string>());
            if (!name.empty()) return name;
        }
    } catch (...) {}
    return fallback_name;
}

std::string pretty_json(const std::string& raw_text,
                               const std::string& fallback_name,
                               InvalidPrettyMode invalid_mode) {
    std::vector<Step> steps;
    std::vector<std::string> lines;
    if (!validate_text(raw_text, steps, &lines)) {
        if (invalid_mode == InvalidPrettyMode::ReturnRaw) return raw_text;
        lines = {"WAIT 200"};
    }
    std::string name = extract_name_or_default(raw_text, fallback_name);
    
    json j;
    j["name"] = name;
    j["commands"] = lines;
    return j.dump(2);
}

std::string pretty_json_with_forced_name(const std::string& raw_text, const std::string& forced_name) {
    std::vector<Step> steps;
    std::vector<std::string> lines;
    if (!validate_text(raw_text, steps, &lines)) return raw_text;
    
    json j;
    j["name"] = forced_name;
    j["commands"] = lines;
    return j.dump(2);
}

bool validate_to_pretty_json(const std::string& raw_text,
                                    std::string& pretty,
                                    std::string& err,
                                    const std::string& fallback_name) {
    std::vector<Step> steps;
    if (!validate_text(raw_text, steps, nullptr)) { err = last_error(); return false; }
    pretty = pretty_json(raw_text, fallback_name);
    err.clear();
    return true;
}

std::uint64_t total_ms(const std::vector<Step>& steps) {
    std::uint64_t total = 0;
    for (const auto& st : steps) {
        if (std::numeric_limits<std::uint64_t>::max() - total < st.duration_ms) return std::numeric_limits<std::uint64_t>::max();
        total += st.duration_ms;
    }
    return total;
}

bool step_at(const std::vector<Step>& steps, std::uint64_t elapsed_ms, Step& out) {
    std::uint64_t cursor = 0;
    for (const auto& st : steps) {
        std::uint64_t next = cursor + st.duration_ms;
        if (elapsed_ms < next) { out = st; return true; }
        cursor = next;
    }
    return false;
}

bool report_at(const std::vector<Step>& steps, std::uint64_t elapsed_ms, ns::HoriHIDReport& out) {
    out.reset();
    Step st{};
    if (!step_at(steps, elapsed_ms, st)) return false;
    out.buttons = st.buttons;
    out.vendor = st.extra_buttons;
    out.hat = st.hat;
    if (st.has_lstick) { out.lx = st.lx; out.ly = st.ly; }
    if (st.has_rstick) { out.rx = st.rx; out.ry = st.ry; }
    return true;
}

std::string normalize_hotkey_or_trim(const std::string& s, NormalizeHotkeyFn normalize) {
    return normalize ? normalize(s) : trim(s);
}

std::string entry_to_object_json(const Entry& e, NormalizeHotkeyFn normalize, int indent_spaces) {
    std::vector<Step> steps;
    std::vector<std::string> lines;
    if (!validate_text(e.json, steps, &lines)) lines = {"WAIT 200"};
    std::string name = trim(e.name).empty() ? extract_name_or_default(e.json, "Macro") : e.name;
    
    json j;
    j["name"] = name;
    j["hotkey"] = normalize_hotkey_or_trim(e.hotkey, normalize);
    j["commands"] = lines;
    return j.dump(indent_spaces);
}

std::string entries_to_json(const std::vector<Entry>& entries, NormalizeHotkeyFn normalize) {
    json arr = json::array();
    for (const auto& e : entries) {
        std::vector<Step> steps;
        std::vector<std::string> lines;
        if (!validate_text(e.json, steps, &lines)) lines = {"WAIT 200"};
        std::string name = trim(e.name).empty() ? extract_name_or_default(e.json, "Macro") : e.name;
        
        json j;
        j["name"] = name;
        j["hotkey"] = normalize_hotkey_or_trim(e.hotkey, normalize);
        j["commands"] = lines;
        arr.push_back(j);
    }
    json root;
    root["macros"] = arr;
    return root.dump(2);
}

bool parse_entries_text(const std::string& raw,
                        std::vector<Entry>& out,
                        std::string& err,
                        NormalizeHotkeyFn normalize) {
    out.clear();
    err.clear();
    if (raw.size() > JSON_MAX_BYTES) { err = "macro JSON exceeds 50MB limit"; return false; }
    std::string t = trim(raw);
    if (t.empty()) return true;

    try {
        json j = json::parse(t);
        json arr = j;
        if (j.is_object() && j.contains("macros") && j["macros"].is_array()) {
            arr = j["macros"];
        } else if (!j.is_array()) {
            arr = json::array({ j });
        }
        
        for (const auto& item : arr) {
            std::string obj_str = item.is_string() ? item.get<std::string>() : item.dump();
            std::string pretty;
            if (!validate_to_pretty_json(obj_str, pretty, err, "Macro")) return false;
            
            Entry e;
            e.json = pretty;
            e.name = extract_name_or_default(obj_str, "Macro");
            if (item.is_object() && item.contains("hotkey") && item["hotkey"].is_string()) {
                e.hotkey = item["hotkey"].get<std::string>();
            }
            e.hotkey = normalize_hotkey_or_trim(e.hotkey, normalize);
            out.push_back(std::move(e));
        }
        return true;
    } catch (const json::exception& e) {
        err = "JSON parse error: " + std::string(e.what());
        return false;
    }
}




std::string buttons_to_text(std::uint16_t buttons) {
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

static std::string extra_buttons_to_text(std::uint8_t buttons) {
    struct BtnName { std::uint8_t bit; const char* name; };
    static const BtnName names[] = {
        {ns::EXT_BUTTON_C, "C"}, {ns::EXT_BUTTON_GL, "GL"}, {ns::EXT_BUTTON_GR, "GR"},
        {ns::EXT_BUTTON_SL, "SL"}, {ns::EXT_BUTTON_SR, "SR"},
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

RecordFrame record_frame_from_report(const ns::HoriHIDReport& report) {
    auto axis_dir = [](std::uint8_t v) -> std::int8_t {
        if (v < 80) return -1;
        if (v > 176) return 1;
        return 0;
    };
    RecordFrame f{};
    f.buttons = report.buttons;
    f.extra_buttons = static_cast<std::uint8_t>(report.vendor & ns::EXT_BUTTON_MASK);
    f.hat = report.hat;
    f.lx = axis_dir(report.lx);
    f.ly = axis_dir(report.ly);
    f.rx = axis_dir(report.rx);
    f.ry = axis_dir(report.ry);
    return f;
}

void append_token(std::string& out, const char* token) {
    if (!out.empty()) out += "+";
    out += token;
}

std::string record_frame_to_text(const RecordFrame& f) {
    std::string out = buttons_to_text(f.buttons);
    const std::string extras = extra_buttons_to_text(f.extra_buttons);
    if (!extras.empty()) {
        if (!out.empty()) out += "+";
        out += extras;
    }
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



void runtime_start(Runtime& rt, std::vector<Step> parsed_steps, std::uint64_t now_us) {
    rt.steps = std::move(parsed_steps);
    rt.running = true;
    rt.start_us = now_us;
}

bool runtime_start_text(Runtime& rt, const std::string& raw_text, std::uint64_t now_us) {
    std::vector<Step> steps;
    if (!validate_text(raw_text, steps, nullptr)) return false;
    runtime_start(rt, std::move(steps), now_us);
    return true;
}

bool runtime_running(Runtime& rt, std::uint64_t now_us, std::uint64_t grace_ms) {
    if (!rt.running) return false;
    std::uint64_t elapsed_ms = (now_us - rt.start_us) / 1000ULL;
    if (elapsed_ms > total_ms(rt.steps) + grace_ms) { rt.running = false; return false; }
    return true;
}

bool runtime_step(Runtime& rt, std::uint64_t now_us, Step& out) {
    if (!rt.running) return false;
    std::uint64_t elapsed_ms = (now_us - rt.start_us) / 1000ULL;
    if (!step_at(rt.steps, elapsed_ms, out)) { rt.running = false; return false; }
    return true;
}

bool runtime_report(Runtime& rt, std::uint64_t now_us, ns::HoriHIDReport& out) {
    if (!rt.running) return false;
    std::uint64_t elapsed_ms = (now_us - rt.start_us) / 1000ULL;
    bool active = report_at(rt.steps, elapsed_ms, out);
    if (!active) rt.running = false;
    return active;
}


void Recorder::start(std::uint64_t now_us) {
    recording = true;
    last_frame = RecordFrame{};
    have_frame = false;
    has_input = false;
    last_change_us = now_us;
    commands.clear();
}

void Recorder::append(const RecordFrame& frame, std::uint64_t duration_ms) {
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

void Recorder::sample(const ns::HoriHIDReport& report, std::uint64_t now_us, bool macro_playback_running) {
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

std::string Recorder::stop(std::uint64_t now_us) {
    if (recording && have_frame) append(last_frame, (now_us - last_change_us) / 1000ULL);
    recording = false;
    have_frame = false;
    if (!has_input) {
        commands.clear();
        return "";
    }
    return pretty_json(commands, "Recorded Macro");
}

} // namespace macro
} // namespace ns
