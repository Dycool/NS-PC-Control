#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ns::s2nfc {

inline constexpr std::size_t RAW_DUMP_SIZE = 540;
inline constexpr std::size_t ORIGINALITY_SIGNATURE_SIZE = 32;
inline constexpr std::size_t EXTENDED_DUMP_SIZE = RAW_DUMP_SIZE + ORIGINALITY_SIGNATURE_SIZE;
inline constexpr std::size_t STATUS_PAYLOAD_SIZE = 61;
inline constexpr std::size_t READ_METADATA_SIZE = 63;
inline constexpr std::size_t READ_TRAILER_SIZE = 19;
inline constexpr std::size_t READ_PAYLOAD_SIZE = READ_METADATA_SIZE + RAW_DUMP_SIZE + READ_TRAILER_SIZE;
inline constexpr std::size_t WRITE_STAGING_SIZE = 454;

using Signature = std::array<std::uint8_t, ORIGINALITY_SIGNATURE_SIZE>;

// Known-good fallback used by mature Switch 1 controller emulators when a
// normal 540-byte dump does not include the NTAG21x READ_SIG result. A real
// 572-byte dump should be preferred because the actual signature is per-tag.
inline constexpr Signature FALLBACK_ORIGINALITY_SIGNATURE = {
    0x7D, 0xFD, 0xF0, 0x79, 0x36, 0x51, 0xAB, 0xD7,
    0x46, 0x6E, 0x39, 0xC1, 0x91, 0xBA, 0xBE, 0xB8,
    0x56, 0xCE, 0xED, 0xF1, 0xCE, 0x44, 0xCC, 0x75,
    0xEA, 0xFB, 0x27, 0x09, 0x4D, 0x08, 0x7A, 0xE8,
};

struct WriteApplyResult {
    bool ok = false;
    std::size_t record_count = 0;
    std::size_t data_bytes = 0;
    std::string error;
};

std::array<std::uint8_t, 7> uid_from_raw(std::span<const std::uint8_t> raw);
bool validate_raw_dump(std::span<const std::uint8_t> raw, std::string* error = nullptr);

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

} // namespace ns::s2nfc
