#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ns::s2nfc {

inline constexpr std::size_t TAGMO_DUMP_SIZE = 532;
inline constexpr std::size_t RAW_DUMP_SIZE = 540;
inline constexpr std::size_t ORIGINALITY_SIGNATURE_SIZE = 32;
inline constexpr std::size_t EXTENDED_DUMP_SIZE = RAW_DUMP_SIZE + ORIGINALITY_SIGNATURE_SIZE;
inline constexpr std::size_t V3_DUMP_SIZE = 2048;
inline constexpr std::size_t V3_COMPAT_SPLIT = 0x80;
inline constexpr std::size_t V3_COMPAT_SHIFT = 0x40;
inline constexpr std::size_t V3_SRAM_OFFSET = 0x3C0;
inline constexpr std::size_t V3_SRAM_SIZE = 64;
inline constexpr std::size_t V3_SRAM_DATA_SIZE = 62;
inline constexpr std::size_t V3_NS_REG_OFFSET = 0x3B6;
inline constexpr std::uint8_t V3_SRAM_RF_READY = 0x08;
inline constexpr std::size_t V3_WRITE_END = 0x248;
inline constexpr std::size_t V3_DEVICE_COMMAND_SIZE = 74;
inline constexpr std::size_t V3_DEVICE_RESULT_SIZE = 19 + V3_SRAM_SIZE;
inline constexpr std::size_t V3_EXTENDED_CLEAR_SIZE = 355;
inline constexpr std::size_t V3_EXTENDED_UPDATE_SIZE = 167;
inline constexpr std::size_t STATUS_PAYLOAD_SIZE = 61;
inline constexpr std::size_t READ_METADATA_SIZE = 63;
inline constexpr std::size_t READ_TRAILER_SIZE = 19;
inline constexpr std::size_t READ_PAYLOAD_SIZE = READ_METADATA_SIZE + RAW_DUMP_SIZE + READ_TRAILER_SIZE;
inline constexpr std::size_t V3_OPERATION_PREFIX_SIZE = 60;
inline constexpr std::size_t V3_SECTOR_READ_PREFIX_SIZE = 64;
inline constexpr std::size_t V3_SECTOR_READ_MAX_SIZE = V3_SECTOR_READ_PREFIX_SIZE + V3_DUMP_SIZE;
inline constexpr std::size_t READ_CHUNK_DATA_SIZE = 70;
inline constexpr std::size_t READ_CHUNK_HEADER_SIZE = 3;
inline constexpr std::size_t READ_CHUNK_PAYLOAD_SIZE = READ_CHUNK_HEADER_SIZE + READ_CHUNK_DATA_SIZE;
inline constexpr std::size_t WRITE_STAGING_SIZE = 454;

struct Signature;
using V3ReadPrefixResolver = bool (*)(Signature& output);

inline V3ReadPrefixResolver g_v3_read_prefix_resolver = nullptr;
inline void set_v3_read_prefix_resolver(V3ReadPrefixResolver resolver) noexcept {
    g_v3_read_prefix_resolver = resolver;
}

struct Signature : std::array<std::uint8_t, ORIGINALITY_SIGNATURE_SIZE> {
    using Base = std::array<std::uint8_t, ORIGINALITY_SIGNATURE_SIZE>;
    using Base::operator=;

    Signature() {
        Base::fill(0);
        if (g_v3_read_prefix_resolver) (void)g_v3_read_prefix_resolver(*this);
    }
    constexpr explicit Signature(const Base& value) : Base(value) {}

    void fill(std::uint8_t value) {
        Base::fill(value);
        if (value == 0 && g_v3_read_prefix_resolver)
            (void)g_v3_read_prefix_resolver(*this);
    }
};

inline constexpr Signature FALLBACK_ORIGINALITY_SIGNATURE{
    Signature::Base{
        0x7D, 0xFD, 0xF0, 0x79, 0x36, 0x51, 0xAB, 0xD7,
        0x46, 0x6E, 0x39, 0xC1, 0x91, 0xBA, 0xBE, 0xB8,
        0x56, 0xCE, 0xED, 0xF1, 0xCE, 0x44, 0xCC, 0x75,
        0xEA, 0xFB, 0x27, 0x09, 0x4D, 0x08, 0x7A, 0xE8,
    }};

struct WriteApplyResult {
    bool ok = false;
    std::size_t record_count = 0;
    std::size_t data_bytes = 0;
    std::string error;
};

std::array<std::uint8_t, 7> uid_from_raw(std::span<const std::uint8_t> raw);
std::array<std::uint8_t, 7> uid_from_dump(std::span<const std::uint8_t> dump);
bool validate_raw_dump(std::span<const std::uint8_t> raw, std::string* error = nullptr);
bool validate_v3_dump(std::span<const std::uint8_t> image, std::string* error = nullptr);

std::uint16_t crc16_mcrf4xx(std::span<const std::uint8_t> bytes);
bool v3_sram_response_valid(std::span<const std::uint8_t> image);

bool build_read_buffer_payload(std::span<const std::uint8_t> raw,
                               std::span<const std::uint8_t> signature,
                               std::span<const std::uint8_t> operation_metadata,
                               bool write_mode,
                               std::span<std::uint8_t> output,
                               std::string* error = nullptr);

WriteApplyResult apply_write_staging(std::span<const std::uint8_t> staging,
                                     std::span<const std::uint8_t> coverage,
                                     std::span<std::uint8_t> raw);

bool build_v3_read_buffer(std::span<const std::uint8_t> image,
                          std::span<const std::uint8_t> signature,
                          std::span<const std::uint8_t> request,
                          std::vector<std::uint8_t>& output,
                          std::string* error = nullptr);
bool build_v3_sector_read_buffer(std::span<const std::uint8_t> image,
                                 std::span<const std::uint8_t> signature,
                                 std::span<const std::uint8_t> request,
                                 std::vector<std::uint8_t>& output,
                                 std::string* error = nullptr);
bool build_v3_device_result(std::span<const std::uint8_t> image,
                            std::vector<std::uint8_t>& output,
                            std::string* error = nullptr);
bool build_buffer_chunk(std::span<const std::uint8_t> buffer,
                        std::uint16_t offset,
                        std::span<std::uint8_t> output,
                        std::size_t& output_size,
                        std::string* error = nullptr);

bool is_v3_device_command(std::span<const std::uint8_t> data,
                          std::span<const std::uint8_t> image);
bool apply_v3_device_command(std::span<const std::uint8_t> data,
                             std::span<std::uint8_t> image);
bool is_v3_write_start(std::span<const std::uint8_t> data,
                       std::span<const std::uint8_t> image);
std::size_t v3_extended_expected_size(std::span<const std::uint8_t> data,
                                       std::span<const std::uint8_t> image);
WriteApplyResult apply_v3_write_staging(std::span<const std::uint8_t> staging,
                                         std::span<const std::uint8_t> coverage,
                                         std::span<std::uint8_t> image);
WriteApplyResult apply_v3_extended_staging(std::span<const std::uint8_t> staging,
                                            std::span<const std::uint8_t> coverage,
                                            std::size_t expected_size,
                                            std::span<std::uint8_t> image);

// --- Host-Replayable State Machine Implementations ---

struct Ntag215Runtime {
    uint8_t nfc_status = 0x09;
    uint8_t nfc_detail = 0x00;
    bool operation_active = false;
    bool write_mode = false;
    std::array<uint8_t, WRITE_STAGING_SIZE> write_staging{};
    std::array<uint8_t, WRITE_STAGING_SIZE> write_coverage{};
    bool write_committed = false;
    std::array<uint8_t, 9> operation_metadata{};
    std::vector<uint8_t> op_buffer;
    bool tag_ejected = false;
    uint64_t represent_cooldown_until_ms = 0;

    void init(std::span<const uint8_t> raw, const Signature& sig);
    void reset_transaction();
    bool step(uint64_t now_ms, uint8_t sub, std::span<const uint8_t> req,
              uint8_t* payload, std::size_t& payload_len, uint8_t& direction,
              std::vector<uint8_t>& image);
};

enum class V3ExtendedPhase {
    IDLE,
    AWAIT_UPDATE,
    UPDATE_COMMITTED
};

struct AmiiboV3Runtime {
    uint8_t nfc_status = 0x09;
    uint8_t nfc_detail = 0x00;
    bool operation_active = false;
    bool device_cmd_staged = false;
    bool write_mode = false;
    bool extended_mode = false;
    std::size_t extended_expected_size = 0;
    V3ExtendedPhase extended_phase = V3ExtendedPhase::IDLE;
    uint64_t extended_deadline_ms = 0;
    std::array<uint8_t, WRITE_STAGING_SIZE> write_staging{};
    std::array<uint8_t, WRITE_STAGING_SIZE> write_coverage{};
    bool write_committed = false;
    std::vector<uint8_t> op_buffer;
    bool tag_ejected = false;
    uint64_t represent_cooldown_until_ms = 0;
    Signature signature{};
    bool signature_set = false;

    void init(std::span<const uint8_t> image, const Signature& sig);
    void reset_transaction();
    bool step(uint64_t now_ms, uint8_t sub, std::span<const uint8_t> req,
              uint8_t* payload, std::size_t& payload_len, uint8_t& direction,
              std::vector<uint8_t>& image);
};

enum class TagType {
    NONE,
    NTAG215,
    V3
};

class S2NfcRuntime {
public:
    S2NfcRuntime() = default;

    bool set_tag_data(std::span<const uint8_t> data, bool has_real_signature = false, const Signature& sig = Signature{});
    void clear();

    bool step(uint64_t now_ms, uint8_t sub, std::span<const uint8_t> req,
              uint8_t* payload, std::size_t& payload_len, uint8_t& direction);

    bool is_placed(uint64_t now_ms = 0) const;
    bool is_v3() const { return tag_type_ == TagType::V3; }
    bool is_modified() const { return modified_; }
    void clear_modified() { modified_ = false; }
    TagType type() const { return tag_type_; }
    const std::vector<uint8_t>& image() const { return tag_image_; }
    std::vector<uint8_t>& image() { return tag_image_; }
    const Signature& signature() const { return signature_; }
    bool has_real_signature() const { return has_real_signature_; }

    uint8_t nfc_status() const;
    uint8_t nfc_detail() const;

private:
    TagType tag_type_ = TagType::NONE;
    std::vector<uint8_t> tag_image_;
    Signature signature_{};
    bool has_real_signature_ = false;
    bool modified_ = false;
    Ntag215Runtime ntag215_;
    AmiiboV3Runtime v3_;
};

} // namespace ns::s2nfc
