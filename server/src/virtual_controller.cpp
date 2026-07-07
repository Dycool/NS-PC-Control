#include "virtual_controller.hpp"
#include "app_state.hpp"
#include "bluetooth_manager.hpp"
#include "gadget_wakeup.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <print>
#include <format>
#include <span>
#include <cstring>
#include <sstream>
#include <string_view>

using namespace ns;

uint8_t pro_timer_from_us(uint64_t t_us) { return (uint8_t)((t_us / 5000ULL) & 0xFF); }

// Plain modern descriptor: reports 0x30/0x21/0x81 and the 0x01/0x10/0x80 outputs,
// max report 64 bytes. This is the exact descriptor the console-matched wired
// session enumerated with; it keeps the interrupt endpoints at 64 bytes.
extern const uint8_t VIRTUAL_CONTROLLER_REPORT_DESC[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0xC0
};
extern const size_t VIRTUAL_CONTROLLER_REPORT_DESC_SIZE = sizeof(VIRTUAL_CONTROLLER_REPORT_DESC);

// NFC descriptor: identical to the plain one plus the 0x31 NFC/IR input report
// (49-byte standard prefix + 313 bytes of MCU data). FunctionFS advertises this
// from boot so amiibo scans do not require USB re-enumeration; legacy f_hid can
// still select it only when nfc_gadget_active grows report_length.
extern const uint8_t VIRTUAL_CONTROLLER_REPORT_DESC_NFC[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    // Report 0x31: NFC/IR MCU input. Same standard 49-byte prefix as 0x30,
    // followed by 313 bytes of MCU data.
    0x06, 0x00, 0xFF, 0x85, 0x31, 0x09, 0x07, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x03,
    0xC0
};
extern const size_t VIRTUAL_CONTROLLER_REPORT_DESC_NFC_SIZE = sizeof(VIRTUAL_CONTROLLER_REPORT_DESC_NFC);

const uint8_t* active_report_descriptor(size_t& len) {
    if (g_ctx.nfc_gadget_active.load(std::memory_order_relaxed)) {
        len = VIRTUAL_CONTROLLER_REPORT_DESC_NFC_SIZE;
        return VIRTUAL_CONTROLLER_REPORT_DESC_NFC;
    }
    len = VIRTUAL_CONTROLLER_REPORT_DESC_SIZE;
    return VIRTUAL_CONTROLLER_REPORT_DESC;
}

size_t active_hidg_report_length() {
    return g_ctx.nfc_gadget_active.load(std::memory_order_relaxed) ? HIDG_MAX_REPORT_SIZE : PRO_REPORT_SIZE;
}

extern const uint8_t LEGACY_REPORT_DESC[85] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x01,0x75,0x01,
    0x95,0x0D,0x05,0x09,0x19,0x01,0x29,0x0D,0x81,0x02,0x95,0x03,0x81,0x01,0x05,0x01,
    0x25,0x07,0x46,0x3B,0x01,0x75,0x04,0x95,0x01,0x65,0x14,0x09,0x39,0x81,0x42,
    0x65,0x00,0x95,0x01,0x81,0x01,0x26,0xFF,0x00,0x46,0xFF,0x00,0x09,0x30,0x09,
    0x31,0x09,0x32,0x09,0x35,0x75,0x08,0x95,0x04,0x81,0x02,0x06,0x00,0xFF,0x09,
    0x20,0x75,0x08,0x95,0x01,0x81,0x02,0xC0
};


static const char* nfc_state_name(NfcRuntimeState state) {
    switch (state) {
        case NfcRuntimeState::Off: return "off";
        case NfcRuntimeState::ReportMode31: return "report-0x31";
        case NfcRuntimeState::McuConfigured: return "mcu-configured";
        case NfcRuntimeState::McuStateChanged: return "mcu-state-changed";
        case NfcRuntimeState::Discovery: return "tag-discovery";
        case NfcRuntimeState::PollingForTag: return "polling-for-tag";
        case NfcRuntimeState::TagReadRequested: return "ntag-read-requested";
        case NfcRuntimeState::TagReadQueued: return "ntag-read-queued";
        case NfcRuntimeState::TagWriteSetup: return "ntag-write-setup";
        case NfcRuntimeState::TagWriteReceiving: return "ntag-write-receiving";
        case NfcRuntimeState::TagWriteApplied: return "ntag-write-applied";
        case NfcRuntimeState::UnknownMcuCommand: return "unknown-mcu-command";
    }
    return "unknown";
}

static std::string hex_bytes(std::span<const uint8_t> data, size_t max_len = 32) {
    std::string out;
    const size_t n = std::min(data.size(), max_len);
    out.reserve(n * 2 + (data.size() > max_len ? 3 : 0));
    for (size_t i = 0; i < n; ++i) out += std::format("{:02x}", data[i]);
    if (data.size() > max_len) out += "...";
    return out;
}

static void nfc_transition(ControllerRuntime& rt, NfcRuntimeState next, std::string_view reason) {
    if (rt.nfc_state == next) return;
    if (g_ctx.verbose) {
        std::println("[nfc] port={} {} -> {} ({})", rt.ctrl + 1, nfc_state_name(rt.nfc_state), nfc_state_name(next), reason);
    }
    rt.nfc_state = next;
}

static const char* mcu_cmd_name(uint8_t cmd, uint8_t subcmd) {
    if (cmd == 0x01) return "status/keepalive";
    if (cmd == 0x02 && subcmd == 0x04) return "start tag discovery";
    if (cmd == 0x02 && subcmd == 0x01) return "tag polling";
    if (cmd == 0x02 && subcmd == 0x06) return "ntag read/write setup";
    if (cmd == 0x02 && subcmd == 0x08) return "ntag write chunk";
    if (cmd == 0x02) return "nfc command";
    return "unknown";
}

namespace {
constexpr uint8_t MCU_POWER_SUSPENDED = 0x00;
constexpr uint8_t MCU_POWER_READY = 0x01;
constexpr uint8_t MCU_POWER_CONFIGURED_NFC = 0x04;

uint8_t nfc_crc8(std::span<const uint8_t> data) {
    uint8_t crc = 0;
    for (uint8_t b : data) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

using NfcMcuFrame = std::array<uint8_t, NFC_MCU_DATA_SIZE>;

void nfc_append_part(NfcMcuFrame& out, size_t& pos, std::span<const uint8_t> part) {
    const size_t n = std::min(part.size(), out.size() > pos ? out.size() - pos : 0);
    if (n) std::memcpy(out.data() + pos, part.data(), n);
    pos += n;
}

void nfc_append_hex(NfcMcuFrame& out, size_t& pos, std::string_view hex) {
    for (size_t i = 0; i + 1 < hex.size() && pos < out.size(); i += 2) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
            return 0;
        };
        out[pos++] = static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1]));
    }
}

template <typename... Parts>
NfcMcuFrame nfc_pack(uint8_t background, Parts&&... parts) {
    NfcMcuFrame out{};
    out.fill(background);
    size_t pos = 0;
    (nfc_append_part(out, pos, std::span<const uint8_t>(std::forward<Parts>(parts))), ...);
    out.back() = nfc_crc8(std::span<const uint8_t>(out.data(), out.size() - 1));
    return out;
}

NfcMcuFrame nfc_pack_hex_only(std::string_view hex, uint8_t background = 0) {
    NfcMcuFrame out{};
    out.fill(background);
    size_t pos = 0;
    nfc_append_hex(out, pos, hex);
    out.back() = nfc_crc8(std::span<const uint8_t>(out.data(), out.size() - 1));
    return out;
}

template <typename... Parts>
NfcMcuFrame nfc_pack_hex(std::string_view first_hex, Parts&&... parts) {
    NfcMcuFrame out{};
    out.fill(0);
    size_t pos = 0;
    nfc_append_hex(out, pos, first_hex);
    (nfc_append_part(out, pos, std::span<const uint8_t>(std::forward<Parts>(parts))), ...);
    out.back() = nfc_crc8(std::span<const uint8_t>(out.data(), out.size() - 1));
    return out;
}

void nfc_queue_clear(ControllerRuntime& rt) {
    rt.nfc_queue_head = 0;
    rt.nfc_queue_count = 0;
}

void nfc_queue_push(ControllerRuntime& rt, const NfcMcuFrame& frame, bool force = false) {
    if (rt.nfc_queue_count >= NFC_MCU_QUEUE_CAP) {
        if (!force) {
            if (g_ctx.verbose) std::println("[nfc] port={} dropping MCU response; queue full", rt.ctrl + 1);
            return;
        }
        // Keep the newest forced response by overwriting the oldest one.
        rt.nfc_queue_head = static_cast<uint8_t>((rt.nfc_queue_head + 1) % NFC_MCU_QUEUE_CAP);
        rt.nfc_queue_count = static_cast<uint8_t>(NFC_MCU_QUEUE_CAP - 1);
    }
    const uint8_t tail = static_cast<uint8_t>((rt.nfc_queue_head + rt.nfc_queue_count) % NFC_MCU_QUEUE_CAP);
    rt.nfc_queue[tail] = frame;
    ++rt.nfc_queue_count;
}

NfcMcuFrame nfc_no_response_frame() {
    return nfc_pack_hex_only("ff");
}

NfcMcuFrame nfc_status_frame(uint8_t power_state) {
    const std::array<uint8_t, 1> state{power_state};
    return nfc_pack_hex("0100000008001b", state);
}

std::array<uint8_t, 7> amiibo_uid_from_dump(std::span<const uint8_t> dump) {
    std::array<uint8_t, 7> uid{};
    if (dump.size() >= 8) {
        uid[0] = dump[0]; uid[1] = dump[1]; uid[2] = dump[2];
        uid[3] = dump[4]; uid[4] = dump[5]; uid[5] = dump[6]; uid[6] = dump[7];
    }
    return uid;
}

bool uid_is_zero(std::span<const uint8_t> uid) {
    for (uint8_t b : uid) if (b != 0) return false;
    return true;
}

void nfc_queue_status(ControllerRuntime& rt) {
    if (rt.nfc_power_state == MCU_POWER_SUSPENDED) nfc_queue_push(rt, nfc_no_response_frame());
    else nfc_queue_push(rt, nfc_status_frame(rt.nfc_power_state));
}

NfcMcuFrame nfc_poll_status_frame(ControllerRuntime& rt, const std::vector<uint8_t>* dump) {
    std::array<uint8_t, 1> seq{rt.nfc_seq_no};
    std::array<uint8_t, 1> ack{rt.nfc_ack_seq_no};
    std::array<uint8_t, 1> state{static_cast<uint8_t>(rt.nfc_tag_state)};

    if (rt.nfc_tag_state == NfcTagState::ProcessingWrite && rt.nfc_write_processing_countdown > 0) {
        --rt.nfc_write_processing_countdown;
        if (rt.nfc_write_processing_countdown == 0) rt.nfc_tag_state = NfcTagState::None;
    }

    if (dump && dump->size() >= ns::AMIIBO_DUMP_BYTES && rt.nfc_tag_state != NfcTagState::None) {
        const auto uid = amiibo_uid_from_dump(*dump);
        if (rt.nfc_tag_state == NfcTagState::Poll) {
            if (rt.nfc_last_uid_valid && uid == rt.nfc_last_uid) rt.nfc_tag_state = NfcTagState::PollAgain;
            else { rt.nfc_last_uid = uid; rt.nfc_last_uid_valid = true; }
        } else if (rt.nfc_tag_state == NfcTagState::PollAgain) {
            if (!rt.nfc_last_uid_valid || uid != rt.nfc_last_uid) {
                rt.nfc_tag_state = NfcTagState::Poll;
                rt.nfc_last_uid = uid;
                rt.nfc_last_uid_valid = true;
            }
        }
        state[0] = static_cast<uint8_t>(rt.nfc_tag_state);
        return nfc_pack_hex("2a0005", seq, ack,
                            std::span<const uint8_t>(std::array<uint8_t, 2>{0x09, 0x31}), state,
                            std::span<const uint8_t>(std::array<uint8_t, 8>{0x00,0x00,0x00,0x01,0x01,0x02,0x00,0x07}),
                            uid);
    }

    rt.nfc_last_uid_valid = false;
    return nfc_pack_hex("2a000500000931", state);
}

NfcMcuFrame nfc_write_setup_frame(std::span<const uint8_t> dump) {
    NfcMcuFrame frame{};
    frame.fill(0);
    size_t pos = 0;
    const auto uid = amiibo_uid_from_dump(dump);
    nfc_append_hex(frame, pos, "3a0007010008400200000001020007");
    nfc_append_part(frame, pos, uid);
    nfc_append_hex(frame, pos, "00000000fdb0c0a434c9bf31690030aaef56444b0f602627366d5a281adc697fde0d6cbc010303000000000000f110ffee");
    frame.back() = nfc_crc8(std::span<const uint8_t>(frame.data(), frame.size() - 1));
    return frame;
}

void nfc_queue_read_frames(ControllerRuntime& rt, std::span<const uint8_t> dump) {
    if (dump.size() < ns::AMIIBO_DUMP_BYTES) return;
    const auto uid = amiibo_uid_from_dump(dump);

    NfcMcuFrame frame1{};
    frame1.fill(0);
    size_t pos = 0;
    nfc_append_hex(frame1, pos, "3a0007010001310200000001020007");
    nfc_append_part(frame1, pos, uid);
    nfc_append_hex(frame1, pos, "000000007DFDF0793651ABD7466E39C191BABEB856CEEDF1CE44CC75EAFB27094D087AE803003B3C7778860000");
    nfc_append_part(frame1, pos, dump.subspan(0, 245));
    frame1.back() = nfc_crc8(std::span<const uint8_t>(frame1.data(), frame1.size() - 1));

    NfcMcuFrame frame2{};
    frame2.fill(0);
    pos = 0;
    nfc_append_hex(frame2, pos, "3a000702000927");
    nfc_append_part(frame2, pos, dump.subspan(245, ns::AMIIBO_DUMP_BYTES - 245));
    frame2.back() = nfc_crc8(std::span<const uint8_t>(frame2.data(), frame2.size() - 1));

    NfcMcuFrame trailer{};
    trailer.fill(0);
    pos = 0;
    nfc_append_hex(trailer, pos, "2a000500000931040000000101020007");
    nfc_append_part(trailer, pos, uid);
    trailer.back() = nfc_crc8(std::span<const uint8_t>(trailer.data(), trailer.size() - 1));

    nfc_queue_clear(rt);
    nfc_queue_push(rt, frame1, true);
    nfc_queue_push(rt, frame2, true);
    nfc_queue_push(rt, trailer, true);
    nfc_transition(rt, NfcRuntimeState::TagReadQueued, "queued amiibo read response frames");
}

void nfc_entered_31_input_mode(ControllerRuntime& rt) {
    nfc_queue_clear(rt);
    rt.nfc_power_state = MCU_POWER_READY;
    nfc_queue_status(rt);
}

void nfc_set_power_state(ControllerRuntime& rt, uint8_t power_state) {
    nfc_queue_clear(rt);
    rt.nfc_power_state = (power_state == MCU_POWER_SUSPENDED) ? MCU_POWER_SUSPENDED : MCU_POWER_READY;
    nfc_queue_status(rt);
}

void nfc_set_config(ControllerRuntime& rt, std::span<const uint8_t> config) {
    if (rt.nfc_power_state == MCU_POWER_READY && config.size() > 2) {
        if (config[2] == MCU_POWER_READY || config[2] == MCU_POWER_CONFIGURED_NFC) {
            rt.nfc_power_state = config[2];
        }
    }
    if (rt.nfc_power_state == MCU_POWER_CONFIGURED_NFC) {
        rt.nfc_tag_state = NfcTagState::None;
        rt.nfc_seq_no = 0;
        rt.nfc_ack_seq_no = 0;
        rt.nfc_last_uid_valid = false;
        nfc_queue_status(rt);
    }
}
} // namespace

uint8_t CTRL_MAC_BE[4][6] = {
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA0}, {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA1},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA2}, {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA3}
};

std::string CTRL_SERIAL[4] = { "NSGP260606A0", "NSGP260606A1", "NSGP260606A2", "NSGP260606A3" };

const uint8_t VIRTUAL_BODY_RGB[4][3] = {
    {0xE6, 0x00, 0x12}, // virtual Pro Controller 1: red
    {0xFF, 0xCC, 0x00}, // virtual Pro Controller 2: yellow
    {0x00, 0x64, 0xFF}, // virtual Pro Controller 3: blue
    {0x00, 0xC8, 0x53}, // virtual Pro Controller 4: green
};

// ===========================================================================
// Joy-Con (R) mode (--joycon)
//
// The console accepts wired Joy-Con input when the USB session is typed Pro
// Controller in the 0x80-01 status reply while device info (subcmd 0x02),
// SPI 0x6012 and the per-report conn_info nibble claim Joy-Con (R). Found by
// the identity experiments on 2026-07-04; keeping 0x80-01 = Pro is what stops
// the console from downgrading the session to a pairing-only channel.
// ===========================================================================

static uint8_t g_port_controller_type[HID_PORT_COUNT] = {
    NS_TYPE_PRO, NS_TYPE_PRO, NS_TYPE_PRO, NS_TYPE_PRO
};
constexpr size_t SPI_FLASH_SIZE = 0x10000;
uint8_t g_spi_flash[4][SPI_FLASH_SIZE];
bool g_spi_initialized[4] = {};

uint8_t controller_type_for_port(int ctrl) {
    return ctrl >= 0 && ctrl < HID_PORT_COUNT ? g_port_controller_type[ctrl] : NS_TYPE_PRO;
}

void set_controller_type_for_port(int ctrl, uint8_t type) {
    if (ctrl < 0 || ctrl >= HID_PORT_COUNT) return;
    if (type != NS_TYPE_JOYCON_L && type != NS_TYPE_JOYCON_R) type = NS_TYPE_PRO;
    if (g_port_controller_type[ctrl] == type) return;
    g_port_controller_type[ctrl] = type;
    g_spi_initialized[ctrl] = false;
    init_spi_flash(ctrl);
}

void apply_controller_type_input(uint8_t type, HIDReport& r) {
    if (type == NS_TYPE_JOYCON_R) {
        // Single-stick clients normally drive the left stick; a right Joy-Con
        // exposes that physical stick in the right-stick report field.
        if (r.input.rx == 128 && r.input.ry == 128) {
            r.input.rx = r.input.lx; r.input.ry = r.input.ly;
        }
        r.input.buttons &= BTN_Y | BTN_B | BTN_A | BTN_X | BTN_R | BTN_ZR |
                           BTN_PLUS | BTN_RSTICK | BTN_HOME;
        r.input.hat = HAT_NEUTRAL;
        r.input.lx = r.input.ly = 128;
    } else if (type == NS_TYPE_JOYCON_L) {
        r.input.buttons &= BTN_L | BTN_ZL | BTN_MINUS | BTN_LSTICK | BTN_CAPTURE;
        r.input.rx = r.input.ry = 128;
    }
}

void apply_controller_type_report(uint8_t type, uint8_t* buf) {
    if (type != NS_TYPE_JOYCON_L && type != NS_TYPE_JOYCON_R) return;
    // buf is a 0x30/0x21 report: conn_info at [2], buttons at [3..5].
    // conn_info low nibble 0xF = Joy-Con ((v >> 1) & 3 == 3) + USB-powered bit.
    buf[2] = (buf[2] & 0xF0) | 0x0F;
    // Map the side's shoulder/trigger pair onto SR/SL so the normal
    // single-Joy-Con "press SL+SR" registration gesture remains available.
    uint8_t& side = type == NS_TYPE_JOYCON_R ? buf[3] : buf[5];
    if (side & 0x40) side |= 0x10;
    if (side & 0x80) side |= 0x20;
}

bool read_random_bytes(uint8_t* dst, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t r = read(fd, dst + off, len - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        close(fd); if (off == len) return true;
    }
    uint64_t seed = (uint64_t)now_us() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < len; ++i) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; dst[i] = (uint8_t)seed; }
    return true;
}

void randomize_controller_identity() {
    uint8_t rnd[16]{}; read_random_bytes(rnd, sizeof(rnd));
    for (int i = 0; i < 4; ++i) {
        CTRL_MAC_BE[i][0] = 0x02; CTRL_MAC_BE[i][1] = 0x4E; CTRL_MAC_BE[i][2] = 0x53;
        CTRL_MAC_BE[i][3] = rnd[(i * 3) % 16]; CTRL_MAC_BE[i][4] = rnd[(i * 3 + 1) % 16]; CTRL_MAC_BE[i][5] = rnd[(i * 3 + 2) % 16] + i;
        CTRL_SERIAL[i] = "NSGP" + std::format("{:02X}{:02X}{:02X}{:02X}", CTRL_MAC_BE[i][2], CTRL_MAC_BE[i][3], CTRL_MAC_BE[i][4], CTRL_MAC_BE[i][5]);
    }
    g_ctx.usb_serial = std::format("{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
}

void init_spi_flash(int ctrl) {
    if (ctrl < 0 || ctrl >= 4 || g_spi_initialized[ctrl]) return;
    uint8_t* flash = g_spi_flash[ctrl]; std::memset(flash, 0xFF, SPI_FLASH_SIZE);
    flash[0x6012] = controller_type_for_port(ctrl); flash[0x6013] = 0xA0; flash[0x601B] = 0x02;

    auto put16 = [&](uint16_t a, int16_t v) { flash[a] = v; flash[a+1] = v >> 8; };
    auto pack12 = [](uint8_t* d, uint16_t x, uint16_t y) { d[0] = x; d[1] = (x >> 8) | (y << 4); d[2] = y >> 4; };

    pack12(flash + 0x603D, 0x600, 0x600); pack12(flash + 0x6040, 0x800, 0x800);
    pack12(flash + 0x6043, 0x600, 0x600); pack12(flash + 0x6046, 0x800, 0x800);
    pack12(flash + 0x6049, 0x600, 0x600); pack12(flash + 0x604C, 0x600, 0x600);

    const int16_t imu[12] = {0, 0, 0, 0x4000, 0x4000, 0x4000, 0, 0, 0, 0x343B, 0x343B, 0x343B};
    for (int i = 0; i < 12; ++i) put16(0x6020 + i * 2, imu[i]);
    put16(0x6080, 0); put16(0x6082, 0); put16(0x6084, 0); pack12(flash + 0x6089, 0x0A0, 0x100);
    for (size_t i = 6; i < 0x24; ++i) flash[0x6086 + i] = (i & 1) ? 0x30 : 0x0F;

    // Joy-Cons deliberately share the same per-player palette as Pro Controllers.
    std::memcpy(flash + 0x6050, VIRTUAL_BODY_RGB[ctrl], 3); std::memset(flash + 0x6053, 0xFF, 3);
    std::memcpy(flash + 0x6056, VIRTUAL_BODY_RGB[ctrl], 3); std::memset(flash + 0x6059, 0xFF, 3);
    flash[0x605C] = 0x00; g_spi_initialized[ctrl] = true;
}

void set_identity_in_0x81(uint8_t* r81, int ctrl) {
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    r81[4] = mac[5]; r81[5] = mac[4]; r81[6] = mac[3]; r81[7] = mac[2]; r81[8] = mac[1]; r81[9] = mac[0];
}

size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl) {
    memset(out, 0, PRO_REPORT_SIZE); out[0] = 0x81; out[1] = subtype;
    if (subtype == 0x01) {
        out[3] = NS_TYPE_PRO;
        set_identity_in_0x81(out, ctrl);
    }
    return PRO_REPORT_SIZE;
}

void build_get_device_info_response(uint8_t* out, int ctrl) {
    memset(out, 0, 36); out[0] = 0x03; out[1] = 0x49; out[2] = controller_type_for_port(ctrl); out[3] = 0x02;
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    out[4] = mac[5]; out[5] = mac[4]; out[6] = mac[3]; out[7] = mac[2]; out[8] = mac[1]; out[9] = mac[0];
    out[10] = 0x01; out[11] = 0x02;
}

uint8_t pro_conn_info_from_hid(const HIDReport& src) {
    if ((src.reserved[1] & EXT_STATUS_BATTERY_VALID) == 0 || src.reserved[0] > 100) return PRO_BAT_CON;

    const int pct = src.reserved[0];
    uint8_t coarse_level = 0;
    if (pct >= 90) coarse_level = 4;      // full
    else if (pct >= 70) coarse_level = 3; // high
    else if (pct >= 45) coarse_level = 2; // medium
    else if (pct >= 20) coarse_level = 1; // low
    else coarse_level = 0;                // critical/empty

    // Pro Controller bat_con byte:
    //   bits 7..5 = coarse battery level (0..4)
    //   bit 4     = charging/external power
    //   bit 0     = controller connected/host-powered flag used by our capture profile
    uint8_t bat_con = static_cast<uint8_t>((coarse_level << 5) | (PRO_BAT_CON & 0x01));
    if (src.reserved[1] & EXT_STATUS_BATTERY_CHARGING) bat_con |= 0x10;
    return bat_con;
}

// The console drives a flashing/cycling player-LED pattern (upper nibble = flash bits) while
// the "Change Grip/Order" / controller-pairing screen is open; a steady player assignment uses
// only the lower (solid) nibble. Our emulated pad always reports full battery, so the console
// never flashes for low power — a flash pattern reliably means a controller-management screen.
bool player_leds_indicate_pairing(uint8_t player_leds) {
    return (player_leds & 0xF0) != 0;
}

void fill_neutral_controls(ProInputReport30& r) {
    r.conn_info = PRO_BAT_CON; r.buttons[0] = 0x00; r.buttons[1] = 0x80; r.buttons[2] = 0x00;
    r.left_stick[0] = r.right_stick[0] = 0x00; r.left_stick[1] = r.right_stick[1] = 0x08; r.left_stick[2] = r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT; memset(r.vendor_rest, 0, sizeof(r.vendor_rest));
}

void fill_neutral_controls(ProInputReport21& r) {
    r.conn_info = PRO_BAT_CON; r.buttons[0] = 0x00; r.buttons[1] = 0x80; r.buttons[2] = 0x00;
    r.left_stick[0] = r.right_stick[0] = 0x00; r.left_stick[1] = r.right_stick[1] = 0x08; r.left_stick[2] = r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT;
}

uint16_t axis8_to_12(uint8_t v) {
    if (v == 128) return 0x800;
    int32_t delta = (int32_t)v - 128;
    return (uint16_t)std::clamp(0x800 + (delta * 0x600) / (delta > 0 ? 127 : 128), 0x200, 0xE00);
}

uint8_t invert_axis8_centered(uint8_t v) { return v == 128 ? 128 : (uint8_t)(255 - v); }

void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8) {
    uint16_t x = axis8_to_12(x8), y = axis8_to_12(invert_axis8_centered(y8));
    out[0] = x; out[1] = (x >> 8) | (y << 4); out[2] = y >> 4;
}

void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]) {
    if (hat > 7) return;
    static const uint8_t hat_map[8] = { 0x02, 0x02 | 0x04, 0x04, 0x01 | 0x04, 0x01, 0x01 | 0x08, 0x08, 0x02 | 0x08 };
    buttons[2] |= hat_map[hat];
}

void map_buttons(uint16_t in_btns, uint8_t hat, uint8_t out_btns[3]) {
    out_btns[0] = (in_btns & BTN_Y ? 0x01 : 0) | (in_btns & BTN_X ? 0x02 : 0) | (in_btns & BTN_B ? 0x04 : 0) |
                  (in_btns & BTN_A ? 0x08 : 0) | (in_btns & BTN_R ? 0x40 : 0) | (in_btns & BTN_ZR ? 0x80 : 0);
    out_btns[1] = 0x80 | (in_btns & BTN_MINUS ? 0x01 : 0) | (in_btns & BTN_PLUS ? 0x02 : 0) |
                  (in_btns & BTN_RSTICK ? 0x04 : 0) | (in_btns & BTN_LSTICK ? 0x08 : 0) |
                  (in_btns & BTN_HOME ? 0x10 : 0) | (in_btns & BTN_CAPTURE ? 0x20 : 0);
    out_btns[2] = (in_btns & BTN_L ? 0x40 : 0) | (in_btns & BTN_ZL ? 0x80 : 0);
    hat_to_pro_buttons(hat, out_btns);
}

void apply_input_controls_to_pro21(const HIDReport& src, ProInputReport21& out) {
    out.conn_info = pro_conn_info_from_hid(src);
    out.vibrator = PRO_VIBRATOR_REPORT; map_buttons(src.input.buttons, src.input.hat, out.buttons);
    pack_stick_12(out.left_stick, src.input.lx, src.input.ly); pack_stick_12(out.right_stick, src.input.rx, src.input.ry);
}

void build_standard_report(const HIDReport& src, const MotionReport motion_samples[3], bool has_motion, bool imu_enabled, uint8_t timer, ProInputReport30& out) {
    memset(&out, 0, sizeof(out)); out.id = RID_INPUT_STANDARD; out.timer = timer; out.conn_info = pro_conn_info_from_hid(src); out.vibrator = PRO_VIBRATOR_REPORT;
    map_buttons(src.input.buttons, src.input.hat, out.buttons);
    pack_stick_12(out.left_stick, src.input.lx, src.input.ly); pack_stick_12(out.right_stick, src.input.rx, src.input.ry);
    MotionReport imu[3]{};
    if (imu_enabled && has_motion && motion_samples) std::memcpy(imu, motion_samples, sizeof(imu));
    out.accel_y_0 = imu[0].ax; out.accel_x_0 = imu[0].ay; out.accel_z_0 = imu[0].az;
    out.gyro_y_0  = imu[0].gx; out.gyro_x_0  = imu[0].gy; out.gyro_z_0  = imu[0].gz;
    out.accel_y_1 = imu[1].ax; out.accel_x_1 = imu[1].ay; out.accel_z_1 = imu[1].az;
    out.gyro_y_1  = imu[1].gx; out.gyro_x_1  = imu[1].gy; out.gyro_z_1  = imu[1].gz;
    out.accel_y_2 = imu[2].ax; out.accel_x_2 = imu[2].ay; out.accel_z_2 = imu[2].az;
    out.gyro_y_2  = imu[2].gx; out.gyro_x_2  = imu[2].gy; out.gyro_z_2  = imu[2].gz;
}

void build_nfc_ir_report(ControllerRuntime& rt, const HIDReport& src, const MotionReport motion_samples[3],
                         bool has_motion, bool imu_enabled, uint8_t timer, ProInputReport31& out) {
    memset(&out, 0, sizeof(out));
    out.id = RID_INPUT_NFC_IR;
    out.timer = timer;
    out.conn_info = pro_conn_info_from_hid(src);
    out.vibrator = PRO_VIBRATOR_REPORT;
    map_buttons(src.input.buttons, src.input.hat, out.buttons);
    pack_stick_12(out.left_stick, src.input.lx, src.input.ly);
    pack_stick_12(out.right_stick, src.input.rx, src.input.ry);

    MotionReport imu[3]{};
    if (imu_enabled && has_motion && motion_samples) std::memcpy(imu, motion_samples, sizeof(imu));
    out.accel_y_0 = imu[0].ax; out.accel_x_0 = imu[0].ay; out.accel_z_0 = imu[0].az;
    out.gyro_y_0  = imu[0].gx; out.gyro_x_0  = imu[0].gy; out.gyro_z_0  = imu[0].gz;
    out.accel_y_1 = imu[1].ax; out.accel_x_1 = imu[1].ay; out.accel_z_1 = imu[1].az;
    out.gyro_y_1  = imu[1].gx; out.gyro_x_1  = imu[1].gy; out.gyro_z_1  = imu[1].gz;
    out.accel_y_2 = imu[2].ax; out.accel_x_2 = imu[2].ay; out.accel_z_2 = imu[2].az;
    out.gyro_y_2  = imu[2].gx; out.gyro_x_2  = imu[2].gy; out.gyro_z_2  = imu[2].gz;

    NfcMcuFrame frame = nfc_no_response_frame();
    if (rt.nfc_queue_count > 0) {
        frame = rt.nfc_queue[rt.nfc_queue_head];
        rt.nfc_queue_head = static_cast<uint8_t>((rt.nfc_queue_head + 1) % NFC_MCU_QUEUE_CAP);
        --rt.nfc_queue_count;
    }
    std::memcpy(out.nfc_ir, frame.data(), frame.size());
}

void reset_nfc_runtime(ControllerRuntime& rt, bool full_reset) {
    rt.nfc_report_mode = false;
    rt.nfc_mcu_configured = false;
    rt.nfc_mcu_resumed = false;
    rt.nfc_state = NfcRuntimeState::Off;
    rt.nfc_tag_state = NfcTagState::None;
    rt.nfc_power_state = MCU_POWER_SUSPENDED;
    rt.nfc_last_mcu_cmd = 0xFF;
    rt.nfc_last_mcu_subcmd = 0xFF;
    rt.nfc_last_poll_log_us = 0;
    rt.nfc_poll_logged_without_tag = false;
    rt.nfc_poll_logged_with_tag = false;
    rt.nfc_last_uid_valid = false;
    rt.nfc_seq_no = 0;
    rt.nfc_ack_seq_no = 0;
    rt.nfc_write_buffer.clear();
    rt.nfc_remove_polls_remaining = 0;
    rt.nfc_write_processing_countdown = 0;
    nfc_queue_clear(rt);
    if (full_reset) rt.nfc_mcu_packet_count = 0;
}

int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, std::span<const uint8_t> cmd_data, ProInputReport21* reply) {
    std::ranges::fill(reply->reply_data, 0); reply->ack = 0x80; reply->subcmd_id = subcmd;
    switch (subcmd) {
    case CMD_BT_MANUAL_PAIRING:
        reply->ack = 0x81;
        if (controller_type_for_port(rt.ctrl) != NS_TYPE_PRO) {
            reply->reply_data[0] = 0x03; reply->reply_data[1] = 0x01;
            return 2;
        }
        if (!cmd_data.empty() && (cmd_data[0] == 0x02 || cmd_data[0] == 0x03)) { std::ranges::fill_n(reply->reply_data, 16, 0x00); return 16; }
        return 0;
    case CMD_TRIGGER_BUTTONS: reply->ack = 0x83; reply->reply_data[0] = 0x00; return 1;
    case CMD_SET_SHIP_MODE: return 0;
    case CMD_SET_NFC_IR_CONFIG: {
        rt.nfc_mcu_configured = true;
        reply->ack = 0xA0;
        static constexpr uint8_t mcu_config_reply[] = {
            0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xC8
        };
        std::memcpy(reply->reply_data, mcu_config_reply, sizeof(mcu_config_reply));
        nfc_set_config(rt, cmd_data);
        nfc_transition(rt, NfcRuntimeState::McuConfigured, std::format("subcmd 0x21/write MCU data={}", hex_bytes(cmd_data, 24)));
        return sizeof(mcu_config_reply);
    }
    case CMD_SET_MCU_STATE:
        rt.nfc_mcu_resumed = cmd_data.empty() || cmd_data[0] != 0x00;
        nfc_set_power_state(rt, cmd_data.empty() ? MCU_POWER_READY : cmd_data[0]);
        nfc_transition(rt, NfcRuntimeState::McuStateChanged, std::format("subcmd 0x22/set MCU state data={}", hex_bytes(cmd_data, 24)));
        return 0;
    case CMD_SET_IMU_SENS: return 0;
    case CMD_GET_DEVICE_INFO: {
        uint8_t info[36]; build_get_device_info_response(info, rt.ctrl); reply->ack = 0x82;
        std::ranges::copy(info, reply->reply_data); return 36;
    }
    case CMD_SET_DATA_FORMAT:
        rt.full_report_enabled = true;
        if (!cmd_data.empty()) {
            // FunctionFS can advertise report 0x31 permanently and submit the
            // 362-byte NFC report only when the console switches to NFC/IR mode.
            // The old f_hid path can still only do this while report_length is
            // enlarged, so usb_transport_supports_nfc_reports() covers both.
            const bool want_nfc = (cmd_data[0] == RID_INPUT_NFC_IR)
                && usb_transport_supports_nfc_reports();
            rt.input_report_mode = want_nfc ? RID_INPUT_NFC_IR : RID_INPUT_STANDARD;
            rt.nfc_report_mode = want_nfc;
            if (want_nfc) {
                nfc_entered_31_input_mode(rt);
                nfc_transition(rt, NfcRuntimeState::ReportMode31, "input report mode 0x31 requested");
            } else if (rt.nfc_state != NfcRuntimeState::Off) {
                nfc_transition(rt, NfcRuntimeState::Off, std::format("input report mode switched to 0x{:02x}", cmd_data[0]));
                reset_nfc_runtime(rt);
            } else if (g_ctx.verbose && cmd_data[0] != RID_INPUT_STANDARD) {
                std::println("[nfc] port={} input report mode 0x{:02x}", rt.ctrl + 1, static_cast<unsigned>(cmd_data[0]));
            }
        }
        return 0;
    case CMD_SPI_FLASH_READ: {
        if (cmd_data.size() < 5) { reply->ack = 0x00; return 0; }
        uint32_t addr = cmd_data[0] | (cmd_data[1] << 8) | (cmd_data[2] << 16) | (cmd_data[3] << 24);
        uint8_t size = std::min((size_t)cmd_data[4], (size_t)44); reply->ack = 0x90;
        std::memcpy(reply->reply_data, cmd_data.data(), 5);
        if (addr < SPI_FLASH_SIZE) {
            size_t to_copy = std::min((size_t)size, (size_t)(SPI_FLASH_SIZE - addr));
            std::memcpy(reply->reply_data + 5, g_spi_flash[rt.ctrl] + addr, to_copy);
            if (to_copy < size) std::memset(reply->reply_data + 5 + to_copy, 0xFF, size - to_copy);
        } else std::memset(reply->reply_data + 5, 0xFF, size);
        return 5 + size;
    }
    case CMD_SET_PLAYER_LIGHTS: case 0x33: return 0;
    case CMD_ENABLE_IMU: rt.imu_enabled = (cmd_data.empty() || cmd_data[0] != 0); return 0;
    case CMD_ENABLE_VIBRATION: rt.vibration_enabled = (cmd_data.empty() || cmd_data[0] != 0); return 0;
    default: return 0;
    }
}


void handle_nfc_ir_output_report(ControllerRuntime& rt, std::span<const uint8_t> packet,
                                 int client_idx, int subpad, bool amiibo_armed, uint32_t amiibo_version) {
    if (packet.empty() || packet[0] != RID_OUTPUT_NFC_IR) return;
    ++rt.nfc_mcu_packet_count;

    const uint8_t mcu_cmd = packet.size() > 10 ? packet[10] : 0xFF;
    const uint8_t mcu_subcmd = packet.size() > 11 ? packet[11] : 0xFF;
    const std::span<const uint8_t> mcu_data(packet.data() + std::min<size_t>(12, packet.size()),
                                            packet.size() > 12 ? packet.size() - 12 : 0);
    const bool changed = (mcu_cmd != rt.nfc_last_mcu_cmd || mcu_subcmd != rt.nfc_last_mcu_subcmd);
    rt.nfc_last_mcu_cmd = mcu_cmd;
    rt.nfc_last_mcu_subcmd = mcu_subcmd;

    if (mcu_cmd == 0x01) {
        nfc_queue_status(rt);
        if (changed && g_ctx.verbose) {
            std::println("[nfc] port={} MCU status request len={} data={}", rt.ctrl + 1, packet.size(), hex_bytes(packet, 48));
        }
        return;
    }

    if (mcu_cmd != 0x02) {
        if (changed) {
            nfc_transition(rt, NfcRuntimeState::UnknownMcuCommand, std::format("0x11 MCU cmd=0x{:02x} sub=0x{:02x}", mcu_cmd, mcu_subcmd));
            if (g_ctx.verbose) {
                std::println("[nfc] port={} 0x11 {} cmd=0x{:02x} sub=0x{:02x} len={} data={}",
                             rt.ctrl + 1, mcu_cmd_name(mcu_cmd, mcu_subcmd), static_cast<unsigned>(mcu_cmd),
                             static_cast<unsigned>(mcu_subcmd), packet.size(), hex_bytes(packet, 48));
            }
        }
        return;
    }

    if (rt.nfc_power_state != MCU_POWER_CONFIGURED_NFC && mcu_subcmd != 0x04) {
        if (g_ctx.verbose && changed) {
            std::println("[nfc] port={} NFC command outside configured NFC mode cmd=0x{:02x} sub=0x{:02x} power=0x{:02x} data={}",
                         rt.ctrl + 1, mcu_cmd, mcu_subcmd, rt.nfc_power_state, hex_bytes(packet, 48));
        }
        return;
    }

    if (mcu_subcmd == 0x04) {
        nfc_transition(rt, NfcRuntimeState::Discovery, "0x11 MCU NFC status/discovery request");
        std::vector<uint8_t> dump;
        uint32_t version = 0;
        bool have_dump = (client_idx >= 0 && subpad >= 0)
            ? server_amiibo_get_dump(client_idx, subpad, now_us(), dump, &version)
            : false;
        if (have_dump && rt.nfc_remove_polls_remaining > 0
                && (rt.nfc_tag_state == NfcTagState::Poll || rt.nfc_tag_state == NfcTagState::PollAgain)) {
            dump.assign(ns::AMIIBO_DUMP_BYTES, 0);
            --rt.nfc_remove_polls_remaining;
            if (g_ctx.verbose && changed) {
                std::println("[nfc] port={} temporarily reports removed amiibo after write ({} polls left)",
                             rt.ctrl + 1, rt.nfc_remove_polls_remaining);
            }
        }
        nfc_queue_push(rt, nfc_poll_status_frame(rt, have_dump ? &dump : nullptr));
        if (g_ctx.verbose && have_dump && changed) {
            std::println("[nfc] port={} slot={} pad={} reports virtual tag present v{}", rt.ctrl + 1, client_idx + 1, subpad + 1, version);
        }
    } else if (mcu_subcmd == 0x01) {
        nfc_transition(rt, NfcRuntimeState::PollingForTag, "0x11 MCU start tag polling");
        rt.nfc_tag_state = NfcTagState::Poll;
        const uint64_t now = now_us();
        const bool should_log = changed
            || (amiibo_armed && !rt.nfc_poll_logged_with_tag)
            || (!amiibo_armed && !rt.nfc_poll_logged_without_tag)
            || elapsed_us_saturated(now, rt.nfc_last_poll_log_us) > 2'000'000ULL;
        if (g_ctx.verbose && should_log) {
            if (amiibo_armed) {
                std::println("[nfc] port={} slot={} pad={} expects amiibo; uploaded dump v{} is ready",
                             rt.ctrl + 1, client_idx + 1, subpad + 1, amiibo_version);
                rt.nfc_poll_logged_with_tag = true;
            } else {
                std::println("[nfc] port={} expects amiibo; no uploaded UDP amiibo dump for mapped slot/pad yet", rt.ctrl + 1);
                rt.nfc_poll_logged_without_tag = true;
            }
            rt.nfc_last_poll_log_us = now;
        }
    } else if (mcu_subcmd == 0x06) {
        nfc_transition(rt, NfcRuntimeState::TagReadRequested, "0x11 MCU NTAG read/write request");
        if (g_ctx.verbose) {
            std::println("[nfc] port={} NTAG read/write request len={} data={}", rt.ctrl + 1, packet.size(), hex_bytes(packet, 64));
        }

        const std::span<const uint8_t> target_uid = mcu_data.size() >= 13 ? mcu_data.subspan(6, 7) : std::span<const uint8_t>{};
        const bool write_setup_request = !target_uid.empty() && !uid_is_zero(target_uid);

        std::vector<uint8_t> dump;
        uint32_t version = 0;
        const bool have_dump = (client_idx >= 0 && subpad >= 0)
            ? server_amiibo_get_dump(client_idx, subpad, now_us(), dump, &version)
            : false;
        if (!have_dump) {
            if (write_setup_request && client_idx >= 0 && subpad >= 0) {
                server_amiibo_request_write_upload(client_idx, subpad);
                if (g_ctx.verbose) {
                    std::println("[nfc] port={} write/setup request for uid={} but no server copy; asking UDP client to resend selected .bin",
                                 rt.ctrl + 1, hex_bytes(target_uid, 7));
                }
            } else if (g_ctx.verbose) {
                std::println("[nfc] port={} cannot serve read request: no uploaded amiibo dump", rt.ctrl + 1);
            }
            return;
        }

        if (!write_setup_request) {
            // Do not consume the dump here: like a real amiibo left on the
            // reader, the tag must survive re-reads (games read then verify).
            // It idles out via the sliding arm window instead.
            nfc_queue_read_frames(rt, dump);
            if (g_ctx.verbose) {
                std::println("[nfc] port={} queued amiibo read dump slot={} pad={} v{}",
                             rt.ctrl + 1, client_idx + 1, subpad + 1, version);
            }
        } else {
            rt.nfc_tag_state = NfcTagState::AwaitingWrite;
            rt.nfc_ack_seq_no = 0;
            rt.nfc_write_buffer.clear();
            nfc_queue_clear(rt);
            nfc_queue_push(rt, nfc_write_setup_frame(dump), true);
            nfc_transition(rt, NfcRuntimeState::TagWriteSetup, "queued amiibo write setup response");
            if (g_ctx.verbose) {
                std::println("[nfc] port={} write/setup request for uid={} slot={} pad={} v{}",
                             rt.ctrl + 1, hex_bytes(target_uid, 7), client_idx + 1, subpad + 1, version);
            }
        }

    } else if (mcu_subcmd == 0x08) {
        nfc_transition(rt, NfcRuntimeState::TagWriteReceiving, "0x11 MCU NTAG write chunk");
        std::vector<uint8_t> dump;
        uint32_t version = 0;
        const bool have_dump = (client_idx >= 0 && subpad >= 0)
            ? server_amiibo_get_dump(client_idx, subpad, now_us(), dump, &version)
            : false;
        if (!have_dump) {
            if (client_idx >= 0 && subpad >= 0) server_amiibo_request_write_upload(client_idx, subpad);
            if (g_ctx.verbose) std::println("[nfc] port={} cannot process write chunk: no uploaded amiibo dump; requesting resend", rt.ctrl + 1);
            return;
        }
        if (mcu_data.size() < 4) {
            if (g_ctx.verbose) std::println("[nfc] port={} short write chunk data={}", rt.ctrl + 1, hex_bytes(mcu_data, 32));
            return;
        }

        const uint8_t seq = mcu_data[0];
        const uint8_t end_marker = mcu_data[2];
        const uint8_t payload_len = mcu_data[3];
        if (mcu_data.size() < static_cast<size_t>(4u + payload_len)) {
            if (g_ctx.verbose) std::println("[nfc] port={} truncated write chunk seq={} len={} have={}", rt.ctrl + 1, seq, payload_len, mcu_data.size());
            return;
        }

        if (seq == 0 && end_marker == 0x08) {
            // Rare single-packet write transaction: byte 3 is the whole payload length.
            rt.nfc_write_buffer.assign(mcu_data.begin() + 4, mcu_data.begin() + 4 + payload_len);
            if (g_ctx.verbose) std::println("[nfc] port={} accepted single-packet write bytes={}", rt.ctrl + 1, rt.nfc_write_buffer.size());
        } else if (seq <= rt.nfc_ack_seq_no) {
            if (g_ctx.verbose) std::println("[nfc] port={} repeated write chunk seq={} ack={}", rt.ctrl + 1, seq, rt.nfc_ack_seq_no);
        } else if (seq == static_cast<uint8_t>(rt.nfc_ack_seq_no + 1)) {
            rt.nfc_write_buffer.insert(rt.nfc_write_buffer.end(), mcu_data.begin() + 4, mcu_data.begin() + 4 + payload_len);
            rt.nfc_ack_seq_no = seq;
            if (g_ctx.verbose) std::println("[nfc] port={} accepted write chunk seq={} bytes={} total={}", rt.ctrl + 1, seq, payload_len, rt.nfc_write_buffer.size());
        } else {
            if (g_ctx.verbose) std::println("[nfc] port={} write chunk sequence gap: got={} expected={}", rt.ctrl + 1, seq, rt.nfc_ack_seq_no + 1);
            rt.nfc_ack_seq_no = 0;
            rt.nfc_write_buffer.clear();
        }

        rt.nfc_tag_state = NfcTagState::Writing;
        nfc_queue_push(rt, nfc_poll_status_frame(rt, &dump), true);

        if (end_marker == 0x08) {
            uint32_t new_version = 0;
            const bool changed = server_amiibo_apply_write_command(client_idx, subpad, rt.nfc_write_buffer, &new_version);
            rt.nfc_ack_seq_no = 0;
            rt.nfc_tag_state = NfcTagState::ProcessingWrite;
            rt.nfc_write_processing_countdown = 4;
            rt.nfc_remove_polls_remaining = 4;
            // Only the per-chunk ack above goes out now; the console learns
            // about ProcessingWrite through its own 0x04 status polls, which
            // also drive the 4-poll countdown (matches real MCU behavior).
            if (changed) nfc_transition(rt, NfcRuntimeState::TagWriteApplied, "applied virtual amiibo write-back");
            if (g_ctx.verbose) {
                std::println("[nfc] port={} write transaction complete changed={} v{} buffered={} bytes",
                             rt.ctrl + 1, changed, new_version, rt.nfc_write_buffer.size());
            }
            rt.nfc_write_buffer.clear();
        }
    } else if (mcu_subcmd == 0x02) {
        nfc_transition(rt, NfcRuntimeState::McuConfigured, "0x11 MCU stop tag polling");
        rt.nfc_tag_state = NfcTagState::None;
        rt.nfc_last_uid_valid = false;
        if (client_idx >= 0 && subpad >= 0) server_amiibo_mark_present(client_idx, subpad, false);
    } else if (changed) {
        nfc_transition(rt, NfcRuntimeState::UnknownMcuCommand, std::format("0x11 MCU NFC sub=0x{:02x}", mcu_subcmd));
        if (g_ctx.verbose) {
            std::println("[nfc] port={} 0x11 {} cmd=0x{:02x} sub=0x{:02x} len={} data={}",
                         rt.ctrl + 1, mcu_cmd_name(mcu_cmd, mcu_subcmd), static_cast<unsigned>(mcu_cmd),
                         static_cast<unsigned>(mcu_subcmd), packet.size(), hex_bytes(packet, 64));
        }
    }
}

bool rumble_half_is_all_zero(const uint8_t* f) { return f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0; }
bool rumble_half_is_neutral_carrier(const uint8_t* f) { return f[0] == 0x00 && f[1] == 0x01 && f[2] == 0x40 && f[3] == 0x40; }

struct DecodedPrecisionRumbleHalf { uint8_t low = 0; uint8_t high = 0; };

uint8_t rumble_scale_capture_delta(int v) {
    v = (v * RUMBLE_GAIN_PERCENT) / 100;
    return (uint8_t)std::clamp(v > 0 ? std::max((int)RUMBLE_MIN_NONZERO, v) : 0, 0, 255);
}

DecodedPrecisionRumbleHalf rumble_decode_half_precision_to_dual(const uint8_t* f) {
    DecodedPrecisionRumbleHalf out{};
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f)) return out;
    int high_delta = std::abs((int)(f[1] & 0x7F) - 0x01), low_delta = std::abs((int)(f[3] & 0x7F) - 0x40);
    out.high = rumble_scale_capture_delta(high_delta * 3 + std::abs((int)f[0]) / 3);
    out.low  = rumble_scale_capture_delta(low_delta * 3 + std::abs((int)(f[2] & 0x7F) - 0x40) / 2);
    return out;
}

uint8_t rumble_decode_half_to_u8(const uint8_t* f) {
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f)) return 0;
    static const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};
    int max_diff = 0, sum_diff = 0;
    for (int i = 0; i < 4; ++i) {
        int d = std::abs((int)f[i] - neutral[i]);
        max_diff = std::max(max_diff, d); sum_diff += d;
    }
    int strength = (max_diff * 2 + sum_diff / 4) * RUMBLE_GAIN_PERCENT / 100;
    return (uint8_t)std::clamp(strength > 0 ? std::max((int)RUMBLE_MIN_NONZERO, strength) : 0, 0, 255);
}

void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4 || len < 10) return;
    const uint8_t* rb = packet + 2;
    DecodedPrecisionRumbleHalf left = rumble_decode_half_precision_to_dual(rb), right = rumble_decode_half_precision_to_dual(rb + 4);
    uint8_t low = std::max(left.low, right.low), high = std::max(left.high, right.high);

    if (low == 0 && high == 0 && !(rumble_half_is_all_zero(rb) || rumble_half_is_neutral_carrier(rb)) && !(rumble_half_is_all_zero(rb + 4) || rumble_half_is_neutral_carrier(rb + 4))) {
        low = rumble_decode_half_to_u8(rb); high = rumble_decode_half_to_u8(rb + 4);
    }

    bool neutral = (low == 0 && high == 0);
    if (neutral && !publish_neutral) return;

    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    if (neutral && !g_ctx.clients[client_idx].rumble_active[sub_idx]) return;

    RumblePacket& ev = g_ctx.clients[client_idx].rumble[sub_idx];
    ev.magic = RUMBLE_MAGIC; ev.subpad = (uint8_t)sub_idx;
    ev.low_freq = neutral ? 0 : low; ev.high_freq = neutral ? 0 : high; ev.duration_10ms = neutral ? 0 : 1;

    PrecisionRumblePacket& precision_ev = g_ctx.clients[client_idx].precision_rumble[sub_idx];
    precision_ev.magic = PRECISION_RUMBLE_MAGIC; precision_ev.subpad = (uint8_t)sub_idx;
    precision_ev.low_freq = ev.low_freq; precision_ev.high_freq = ev.high_freq; precision_ev.duration_10ms = ev.duration_10ms;
    memcpy(precision_ev.precision, rb, sizeof(precision_ev.precision));

    g_ctx.clients[client_idx].rumble_active[sub_idx] = !neutral;
    g_ctx.clients[client_idx].rumble_seq[sub_idx]++;
}
