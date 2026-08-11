#pragma once

#include <algorithm>
#include <array>
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
inline constexpr std::size_t READ_CHUNK_DATA_SIZE = 70;
inline constexpr std::size_t READ_CHUNK_HEADER_SIZE = 3;
inline constexpr std::size_t READ_CHUNK_PAYLOAD_SIZE =
    READ_CHUNK_HEADER_SIZE + READ_CHUNK_DATA_SIZE;
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

// Known-good fallback used by mature Switch 1 controller emulators when a
// normal 540-byte dump does not include the NTAG21x READ_SIG result. A real
// 572-byte dump should be preferred because the actual signature is per-tag.
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

// Builds the exact 622-byte USB payload observed for command 0x01/0x15.
// In read mode it contains 63 bytes of metadata, the 540-byte dump and a
// 19-byte trailer. The nine operation-metadata bytes are copied from the
// preceding 0x06 request, matching both the observed read and write flows. In
// write-preparation mode only metadata and the tag capability-container bytes
// are returned.
bool build_read_buffer_payload(std::span<const std::uint8_t> raw,
                               std::span<const std::uint8_t> signature,
                               std::span<const std::uint8_t> operation_metadata,
                               bool write_mode,
                               std::span<std::uint8_t> output,
                               std::string* error = nullptr);

// Decodes the 454-byte 0x14 staging stream used by the controller. The format
// is D0 07, UID, temporary/final lock bytes, a record count, then repeated
// (page, byte-length, data) records. Only a fully received staging image is
// accepted. The raw dump is updated atomically on success.
WriteApplyResult apply_write_staging(std::span<const std::uint8_t> staging,
                                     std::span<const std::uint8_t> coverage,
                                     std::span<std::uint8_t> raw);

// Figure-v3 (NTAG I2C Plus 2K) operations. A v3 read starts with the same
// identity/signature prefix but byte 18 advertises chip type 0x06. The console
// then retrieves the descriptor-selected pages in offset-addressed 70-byte
// chunks. The stored dump remains a flat 2048-byte image; SRAM_RF_READY is
// raised only on the served copy, matching physical-chip session-register
// behaviour.
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
bool v3_sram_response_valid(std::span<const std::uint8_t> image);

} // namespace ns::s2nfc
