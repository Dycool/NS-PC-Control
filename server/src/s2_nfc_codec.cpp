#include "s2_nfc_codec.hpp"

#include <algorithm>
#include <cstring>

namespace ns::s2nfc {
namespace {

void write_u16le(std::uint8_t* dst, std::uint16_t value) {
    dst[0] = static_cast<std::uint8_t>(value & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void set_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

} // namespace

std::array<std::uint8_t, 7> uid_from_raw(std::span<const std::uint8_t> raw) {
    std::array<std::uint8_t, 7> uid{};
    if (raw.size() < 8) return uid;
    uid = {raw[0], raw[1], raw[2], raw[4], raw[5], raw[6], raw[7]};
    return uid;
}

bool validate_raw_dump(std::span<const std::uint8_t> raw, std::string* error) {
    if (raw.size() != RAW_DUMP_SIZE) {
        set_error(error, "raw NTAG215 dump must be exactly 540 bytes");
        return false;
    }

    // ISO14443 cascade BCC bytes as stored in NTAG page 0/page 2.
    const std::uint8_t bcc0 = static_cast<std::uint8_t>(0x88u ^ raw[0] ^ raw[1] ^ raw[2]);
    const std::uint8_t bcc1 = static_cast<std::uint8_t>(raw[4] ^ raw[5] ^ raw[6] ^ raw[7]);
    if (raw[3] != bcc0 || raw[8] != bcc1) {
        set_error(error, "UID/BCC bytes are inconsistent; this is not a valid raw NTAG215 image");
        return false;
    }
    return true;
}

bool build_read_buffer_payload(std::span<const std::uint8_t> raw,
                               std::span<const std::uint8_t> signature,
                               std::span<const std::uint8_t> operation_metadata,
                               bool write_mode,
                               std::span<std::uint8_t> output,
                               std::string* error) {
    if (!validate_raw_dump(raw, error)) return false;
    if (signature.size() != ORIGINALITY_SIGNATURE_SIZE) {
        set_error(error, "NTAG originality signature must be exactly 32 bytes");
        return false;
    }
    if (output.size() < READ_PAYLOAD_SIZE) {
        set_error(error, "0x15 output buffer is smaller than 622 bytes");
        return false;
    }
    if (operation_metadata.size() != 9) {
        set_error(error, "NFC operation metadata must be exactly 9 bytes");
        return false;
    }

    std::fill(output.begin(), output.begin() + READ_PAYLOAD_SIZE, 0);
    auto* payload = output.data();
    payload[0] = 0x01;
    write_u16le(payload + 1, write_mode ? 0x0040 : 0x0258);
    payload[3] = 0x04;
    payload[7] = 0x01;
    payload[8] = 0x02;
    payload[9] = 0x00;

    const auto uid = uid_from_raw(raw);
    payload[10] = 0x07;
    std::copy(uid.begin(), uid.end(), payload + 11);
    std::copy(signature.begin(), signature.end(), payload + 22);

    // Bytes 54..62 are echoed from bytes 10..18 of the preceding 0x06
    // operation descriptor. This accounts for both the read vector
    // (03 00 3B 3C 77 78 86 00 00) and the write vector
    // (01 03 03 00 00 00 00 00 00) without hard-coding a game/tag profile.
    std::copy(operation_metadata.begin(), operation_metadata.end(), payload + 54);

    if (write_mode) {
        // First four post-metadata bytes observed before the console starts the
        // six 0x14 write-buffer chunks: NTAG page 3 / capability container.
        std::copy(raw.begin() + 12, raw.begin() + 16, payload + READ_METADATA_SIZE);
    } else {
        std::copy(raw.begin(), raw.end(), payload + READ_METADATA_SIZE);
    }
    return true;
}

WriteApplyResult apply_write_staging(std::span<const std::uint8_t> staging,
                                     std::span<const std::uint8_t> coverage,
                                     std::span<std::uint8_t> raw) {
    WriteApplyResult result{};
    if (staging.size() != WRITE_STAGING_SIZE) {
        result.error = "write staging image must be exactly 454 bytes";
        return result;
    }
    if (coverage.size() != WRITE_STAGING_SIZE
            || std::any_of(coverage.begin(), coverage.end(), [](std::uint8_t v) { return v == 0; })) {
        result.error = "write staging image is incomplete";
        return result;
    }
    std::string validation_error;
    if (!validate_raw_dump(raw, &validation_error)) {
        result.error = std::move(validation_error);
        return result;
    }
    if (staging[0] != 0xD0 || staging[1] != 0x07) {
        result.error = "write staging header is not D0 07";
        return result;
    }

    const auto uid = uid_from_raw(raw);
    if (!std::equal(uid.begin(), uid.end(), staging.begin() + 2)) {
        result.error = "write staging UID does not match the selected Amiibo";
        return result;
    }

    const std::size_t record_count = staging[21];
    if (record_count == 0 || record_count > 16) {
        result.error = "invalid write-record count";
        return result;
    }

    std::array<std::uint8_t, RAW_DUMP_SIZE> candidate{};
    std::copy(raw.begin(), raw.end(), candidate.begin());

    std::size_t cursor = 22;
    std::size_t data_bytes = 0;
    for (std::size_t record = 0; record < record_count; ++record) {
        if (cursor + 2 > staging.size()) {
            result.error = "truncated write-record header";
            return result;
        }
        const std::uint8_t page = staging[cursor++];
        const std::size_t length = staging[cursor++];
        if (page == 0 || length == 0 || cursor + length > staging.size()) {
            result.error = "invalid or truncated write record";
            return result;
        }

        const std::size_t address = static_cast<std::size_t>(page) * 4u;
        // The captured controller protocol only writes Amiibo memory from page
        // 5 through the end of page 129. UID/manufacturer pages and NTAG config
        // pages are never part of this transfer.
        if (address < 20 || address + length > 520) {
            result.error = "write record targets a protected/out-of-range NTAG page";
            return result;
        }
        std::copy_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor), length,
                    candidate.begin() + static_cast<std::ptrdiff_t>(address));
        cursor += length;
        data_bytes += length;
    }

    if (std::any_of(staging.begin() + static_cast<std::ptrdiff_t>(cursor), staging.end(),
                    [](std::uint8_t value) { return value != 0; })) {
        result.error = "unexpected non-zero bytes after write records";
        return result;
    }

    // Bytes 13..16 are the temporary lock state used during the physical tag
    // transaction. The persistent image must contain the final lock bytes from
    // 17..20 after all page records have been applied.
    std::copy_n(staging.begin() + 17, 4, candidate.begin() + 16);
    std::copy(candidate.begin(), candidate.end(), raw.begin());

    result.ok = true;
    result.record_count = record_count;
    result.data_bytes = data_bytes;
    return result;
}

} // namespace ns::s2nfc
