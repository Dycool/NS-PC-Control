#pragma once

// Shared macro helpers for NS-PC-Control clients/server.

#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <expected>

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

std::string& last_error_storage();
void set_error(const std::string& e);
const std::string& last_error();

std::string trim(std::string s);
std::string upper(std::string s);
std::expected<std::string, std::string> extract_commands_text(const std::string& raw_in);
bool parse_uint32_strict(const std::string& s, std::uint32_t& out);
std::uint16_t button_bit(const std::string& token);
std::expected<void, std::string> apply_token(const std::string& raw_tok, Step& st,
                 bool& du, bool& dd, bool& dl, bool& dr,
                 bool& llu, bool& lld, bool& lll, bool& llr,
                 bool& rru, bool& rrd, bool& rrl, bool& rrr);
std::expected<void, std::string> parse_one_command(const std::string& part, Step& st);
bool validate_text(const std::string& raw_text, std::vector<Step>& steps,
                   std::vector<std::string>* normalized = nullptr);
std::vector<Step> parse_text(const std::string& raw_text);
std::string read_text_file_limited(const std::string& path, std::string* err = nullptr);
std::string extract_name_or_default(const std::string& raw, const std::string& fallback_name);

enum class InvalidPrettyMode {
    ReturnRaw,
    FallbackWait
};

std::string pretty_json(const std::string& raw_text,
                        const std::string& fallback_name = "Macro",
                        InvalidPrettyMode invalid_mode = InvalidPrettyMode::ReturnRaw);
std::string pretty_json_with_forced_name(const std::string& raw_text, const std::string& forced_name);
bool validate_to_pretty_json(const std::string& raw_text,
                             std::string& pretty,
                             std::string& err,
                             const std::string& fallback_name = "Macro");
std::uint64_t total_ms(const std::vector<Step>& steps);
bool step_at(const std::vector<Step>& steps, std::uint64_t elapsed_ms, Step& out);
bool report_at(const std::vector<Step>& steps, std::uint64_t elapsed_ms, ns::HoriHIDReport& out);

using NormalizeHotkeyFn = std::string (*)(const std::string&);

std::string normalize_hotkey_or_trim(const std::string& s, NormalizeHotkeyFn normalize);
std::string entry_to_object_json(const Entry& e, NormalizeHotkeyFn normalize = nullptr, int indent_spaces = 4);
std::string entries_to_json(const std::vector<Entry>& entries, NormalizeHotkeyFn normalize = nullptr);
bool parse_entries_text(const std::string& raw,
                        std::vector<Entry>& out,
                        std::string& err,
                        NormalizeHotkeyFn normalize = nullptr);

struct RecordFrame {
    std::uint16_t buttons = 0;
    std::uint8_t hat = ns::HAT_NEUTRAL;
    std::int8_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool operator==(const RecordFrame&) const = default;
};
std::string buttons_to_text(std::uint16_t buttons);
RecordFrame record_frame_from_report(const ns::HoriHIDReport& report);
void append_token(std::string& out, const char* token);
std::string record_frame_to_text(const RecordFrame& f);

struct Recorder {
    bool recording = false;
    RecordFrame last_frame{};
    bool have_frame = false;
    bool has_input = false;
    std::uint64_t last_change_us = 0;
    std::string commands;

    void start(std::uint64_t now_us);
    void append(const RecordFrame& frame, std::uint64_t duration_ms);
    void sample(const ns::HoriHIDReport& report, std::uint64_t now_us, bool macro_playback_running = false);
    std::string stop(std::uint64_t now_us);
};

struct Runtime {
    std::vector<Step> steps;
    bool running = false;
    std::uint64_t start_us = 0;
};

void runtime_start(Runtime& rt, std::vector<Step> parsed_steps, std::uint64_t now_us);
bool runtime_start_text(Runtime& rt, const std::string& raw_text, std::uint64_t now_us);
bool runtime_running(Runtime& rt, std::uint64_t now_us, std::uint64_t grace_ms = 120);
bool runtime_step(Runtime& rt, std::uint64_t now_us, Step& out);
bool runtime_report(Runtime& rt, std::uint64_t now_us, ns::HoriHIDReport& out);

} // namespace macro
} // namespace ns
