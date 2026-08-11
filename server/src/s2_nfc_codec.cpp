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

bool all_covered(std::span<const std::uint8_t> coverage, std::size_t size) {
    return coverage.size() >= size
        && std::all_of(coverage.begin(), coverage.begin() + static_cast<std::ptrdiff_t>(size),
                       [](std::uint8_t value) { return value != 0; });
}

bool command_identity_matches(std::span<const std::uint8_t> data,
                              std::span<const std::uint8_t> image) {
    return data.size() >= 11 && image.size() == V3_DUMP_SIZE
        && std::equal(image.begin(), image.begin() + 7, data.begin() + 2)
        && data[9] == 0x01;
}

bool sector1_capability_valid(std::span<const std::uint8_t> value) {
    return value.size() >= 4 && value[0] == 0xA5 && value[1] == 0x00
        && value[2] != 0x00 && value[3] == 0x00;
}

bool range_nonzero(std::span<const std::uint8_t> image,
                   std::size_t offset, std::size_t size) {
    return offset <= image.size() && size <= image.size() - offset
        && std::any_of(image.begin() + static_cast<std::ptrdiff_t>(offset),
                       image.begin() + static_cast<std::ptrdiff_t>(offset + size),
                       [](std::uint8_t value) { return value != 0; });
}

struct V3ExtendedLayout {
    std::uint8_t sector0_page = 0;
    std::uint8_t sector1_capability_page = 0;
};

bool extended_update_layout(std::span<const std::uint8_t> data,
                            V3ExtendedLayout& layout) {
    if (data.size() < 68
            || data[22] != 0x03
            || data[23] != 0x00 || data[24] != 0x04 || data[25] != 0x04
            || data[30] != 0x00 || data[32] != 0x20
            || data[65] != 0x01 || data[67] != 0x60) {
        return false;
    }
    const std::uint8_t sector0_page = data[31];
    const std::uint8_t capability_page = data[13];
    const std::uint16_t data_page = static_cast<std::uint16_t>(capability_page) + 1u;
    if (sector0_page < 0x92
            || static_cast<std::uint16_t>(sector0_page) + 7u > 0xE1
            || data_page > 0xFF
            || data_page + 23u > 0xFF
            || data[66] != static_cast<std::uint8_t>(data_page)) {
        return false;
    }
    layout.sector0_page = sector0_page;
    layout.sector1_capability_page = capability_page;
    return true;
}

std::uint8_t current_capability_generation(std::span<const std::uint8_t> image,
                                           const V3ExtendedLayout& layout) {
    const std::size_t capability_offset =
        0x400u + static_cast<std::size_t>(layout.sector1_capability_page) * 4u;
    const auto stored = image.subspan(capability_offset, 4);
    if (sector1_capability_valid(stored)) return stored[2];
    const std::size_t sector0_offset = static_cast<std::size_t>(layout.sector0_page) * 4u;
    return range_nonzero(image, sector0_offset, 0x20)
        || range_nonzero(image, capability_offset + 4, 0x60) ? 1u : 0u;
}

std::uint16_t crc16_mcrf4xx(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFFu;
    for (const std::uint8_t value : bytes) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>(
                (crc >> 1u) ^ ((crc & 1u) ? 0x8408u : 0u));
        }
    }
    return crc;
}

} // namespace

std::array<std::uint8_t, 7> uid_from_raw(std::span<const std::uint8_t> raw) {
    std::array<std::uint8_t, 7> uid{};
    if (raw.size() < 8) return uid;
    uid = {raw[0], raw[1], raw[2], raw[4], raw[5], raw[6], raw[7]};
    return uid;
}

std::array<std::uint8_t, 7> uid_from_dump(std::span<const std::uint8_t> dump) {
    if (dump.size() == V3_DUMP_SIZE) {
        std::array<std::uint8_t, 7> uid{};
        std::copy_n(dump.begin(), uid.size(), uid.begin());
        return uid;
    }
    return uid_from_raw(dump);
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

bool validate_v3_dump(std::span<const std::uint8_t> image, std::string* error) {
    if (image.size() != V3_DUMP_SIZE) {
        set_error(error, "amiibo v3 dump must be exactly 2048 bytes");
        return false;
    }
    // NTAG I2C Plus 2K stores the seven UID bytes contiguously. Bytes 7 and 8
    // are internal manufacturer bytes rather than the NTAG215 BCC interleave.
    if (image[0] != 0x04 || image[7] != 0x00 || image[8] != 0x44) {
        set_error(error, "invalid NTAG I2C Plus 2K UID/manufacturer header");
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

    std::fill(output.begin(), output.begin() + READ_PAYLOAD_SIZE, std::uint8_t{0});
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

bool build_v3_read_buffer(std::span<const std::uint8_t> image,
                          std::span<const std::uint8_t> signature,
                          std::span<const std::uint8_t> request,
                          std::vector<std::uint8_t>& output,
                          std::string* error) {
    if (!validate_v3_dump(image, error)) return false;
    if (signature.size() != ORIGINALITY_SIGNATURE_SIZE) {
        set_error(error, "NTAG originality signature must be exactly 32 bytes");
        return false;
    }
    if (request.size() < 13) {
        set_error(error, "v3 read descriptor is truncated");
        return false;
    }
    const std::size_t range_count = request[10];
    if (range_count == 0 || 11 + range_count * 2 > request.size()) {
        set_error(error, "v3 read descriptor has invalid page ranges");
        return false;
    }

    std::size_t highest = 0;
    std::size_t data_size = 0;
    for (std::size_t i = 0; i < range_count; ++i) {
        const std::uint8_t first = request[11 + i * 2];
        const std::uint8_t last = request[12 + i * 2];
        if (last < first) {
            set_error(error, "v3 read descriptor contains a reversed page range");
            return false;
        }
        const std::size_t end = (static_cast<std::size_t>(last) + 1u) * 4u;
        highest = std::max(highest, end);
        data_size += (static_cast<std::size_t>(last) - first + 1u) * 4u;
    }
    if (highest > V3_DUMP_SIZE) {
        set_error(error, "v3 read descriptor exceeds the 2 KiB image");
        return false;
    }

    std::array<std::uint8_t, RAW_DUMP_SIZE> compatibility{};
    std::array<std::uint8_t, V3_DUMP_SIZE> served{};
    std::copy(image.begin(), image.end(), served.begin());
    served[V3_NS_REG_OFFSET] |= V3_SRAM_RF_READY;
    const std::uint8_t* source = served.data();
    std::size_t source_size = served.size();
    if (highest <= RAW_DUMP_SIZE) {
        std::copy_n(served.begin(), V3_COMPAT_SPLIT, compatibility.begin());
        std::copy_n(served.begin() + V3_COMPAT_SPLIT + V3_COMPAT_SHIFT,
                    RAW_DUMP_SIZE - V3_COMPAT_SPLIT,
                    compatibility.begin() + V3_COMPAT_SPLIT);
        source = compatibility.data();
        source_size = compatibility.size();
    }

    output.assign(V3_OPERATION_PREFIX_SIZE + data_size, 0);
    output[0] = 0x04;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    std::copy_n(image.begin(), 7, output.begin() + 8);
    output[18] = 0x06; // native controller chip identity for figure-v3
    std::copy(signature.begin(), signature.end(), output.begin() + 19);
    if (request.size() >= 19)
        std::copy_n(request.begin() + 10, 9, output.begin() + 51);

    std::size_t cursor = V3_OPERATION_PREFIX_SIZE;
    for (std::size_t i = 0; i < range_count; ++i) {
        const std::uint8_t first = request[11 + i * 2];
        const std::uint8_t last = request[12 + i * 2];
        const std::size_t from = static_cast<std::size_t>(first) * 4u;
        const std::size_t length =
            (static_cast<std::size_t>(last) - first + 1u) * 4u;
        if (from + length > source_size) {
            set_error(error, "v3 read range is unavailable in the selected memory view");
            output.clear();
            return false;
        }
        std::copy_n(source + from, length, output.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += length;
    }
    return true;
}

bool build_buffer_chunk(std::span<const std::uint8_t> buffer,
                        std::uint16_t offset,
                        std::span<std::uint8_t> output,
                        std::size_t& output_size,
                        std::string* error) {
    output_size = 0;
    if (buffer.empty() || offset >= buffer.size()) {
        set_error(error, "read-buffer offset is out of range");
        return false;
    }
    const std::size_t length =
        std::min<std::size_t>(READ_CHUNK_DATA_SIZE, buffer.size() - offset);
    if (output.size() < READ_CHUNK_HEADER_SIZE + length) {
        set_error(error, "read-chunk output buffer is too small");
        return false;
    }
    output[0] = offset + length == buffer.size() ? 1u : 0u;
    write_u16le(output.data() + 1, static_cast<std::uint16_t>(length));
    std::copy_n(buffer.begin() + offset, length, output.begin() + READ_CHUNK_HEADER_SIZE);
    output_size = READ_CHUNK_HEADER_SIZE + length;
    return true;
}

bool build_v3_sector_read_buffer(std::span<const std::uint8_t> image,
                                 std::span<const std::uint8_t> signature,
                                 std::span<const std::uint8_t> request,
                                 std::vector<std::uint8_t>& output,
                                 std::string* error) {
    if (!validate_v3_dump(image, error)) return false;
    if (signature.size() != ORIGINALITY_SIGNATURE_SIZE) {
        set_error(error, "NTAG originality signature must be exactly 32 bytes");
        return false;
    }
    if (request.size() < 17 || request.size() > 29
            || !std::equal(image.begin(), image.begin() + 7, request.begin() + 2)
            || request[9] != 0x01) {
        set_error(error, "invalid v3 sector-read identity or envelope");
        return false;
    }
    const std::size_t range_count = request[10];
    const std::size_t ranges_end = 11 + range_count * 3;
    if (range_count == 0 || range_count > 4 || ranges_end + 6 != request.size()
            || std::any_of(request.begin() + static_cast<std::ptrdiff_t>(ranges_end),
                           request.end(), [](std::uint8_t v) { return v != 0; })) {
        set_error(error, "invalid v3 sector-read range list");
        return false;
    }

    std::size_t prefix_size = request.size() >= 29 ? 76 : V3_SECTOR_READ_PREFIX_SIZE;
    std::size_t result_size = prefix_size;
    for (std::size_t i = 0; i < range_count; ++i) {
        const std::uint8_t sector = request[11 + i * 3];
        const std::uint8_t first = request[12 + i * 3];
        const std::uint8_t last = request[13 + i * 3];
        if (sector > 1 || last < first) {
            set_error(error, "invalid v3 sector/page range");
            return false;
        }
        const std::size_t address =
            static_cast<std::size_t>(sector) * 0x400u + static_cast<std::size_t>(first) * 4u;
        const std::size_t length =
            (static_cast<std::size_t>(last) - first + 1u) * 4u;
        if (address + length > image.size()) {
            set_error(error, "v3 sector/page range exceeds the image");
            return false;
        }
        result_size += length;
    }

    output.assign(result_size, 0);
    output[0] = 0x15;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    std::copy_n(image.begin(), 7, output.begin() + 8);
    output[18] = 0x06;
    std::copy(signature.begin(), signature.end(), output.begin() + 19);
    std::copy(request.begin() + 10, request.end(), output.begin() + 51);

    static constexpr std::array<std::uint8_t, 4> FIRST_USE_CAPABILITY{
        0xA5, 0x00, 0x01, 0x00};
    std::size_t cursor = prefix_size;
    for (std::size_t i = 0; i < range_count; ++i) {
        const std::uint8_t sector = request[11 + i * 3];
        const std::uint8_t first = request[12 + i * 3];
        const std::uint8_t last = request[13 + i * 3];
        const std::size_t address =
            static_cast<std::size_t>(sector) * 0x400u + static_cast<std::size_t>(first) * 4u;
        const std::size_t length =
            (static_cast<std::size_t>(last) - first + 1u) * 4u;
        std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(address), length,
                    output.begin() + static_cast<std::ptrdiff_t>(cursor));
        const bool capability_range = range_count == 2 && i == 1 && sector == 1
            && request[11] == 0
            && static_cast<std::uint16_t>(request[13]) - request[12] + 1u == 8u
            && static_cast<std::uint16_t>(last) - first + 1u == 25u;
        if (capability_range
                && !sector1_capability_valid(
                    std::span<const std::uint8_t>(output).subspan(cursor, 4))) {
            std::copy(FIRST_USE_CAPABILITY.begin(), FIRST_USE_CAPABILITY.end(),
                      output.begin() + static_cast<std::ptrdiff_t>(cursor));
        }
        cursor += length;
    }
    return true;
}

bool build_v3_device_result(std::span<const std::uint8_t> image,
                            std::vector<std::uint8_t>& output,
                            std::string* error) {
    if (!validate_v3_dump(image, error)) return false;
    output.assign(V3_DEVICE_RESULT_SIZE, 0);
    output[0] = 0x18;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    std::copy_n(image.begin(), 7, output.begin() + 8);
    output[18] = 0x06;
    std::copy_n(image.begin() + V3_SRAM_OFFSET, V3_SRAM_SIZE, output.begin() + 19);
    return true;
}

bool is_v3_device_command(std::span<const std::uint8_t> data,
                          std::span<const std::uint8_t> image) {
    return data.size() == V3_DEVICE_COMMAND_SIZE
        && command_identity_matches(data, image) && data[10] == 0x01;
}

bool apply_v3_device_command(std::span<const std::uint8_t> data,
                             std::span<std::uint8_t> image) {
    if (!is_v3_device_command(data, image) || image.size() != V3_DUMP_SIZE) {
        return false;
    }
    std::copy_n(data.begin() + 10, V3_SRAM_SIZE, image.begin() + V3_SRAM_OFFSET);
    return true;
}

bool is_v3_write_start(std::span<const std::uint8_t> data,
                       std::span<const std::uint8_t> image) {
    return data.size() >= 22 && command_identity_matches(data, image)
        && data[10] == 0x06 && data[21] >= 1 && data[21] <= 16;
}

std::size_t v3_extended_expected_size(std::span<const std::uint8_t> data,
                                      std::span<const std::uint8_t> image) {
    if (data.size() < 26 || !command_identity_matches(data, image)
            || data[10] != 0x06) {
        return 0;
    }
    if (std::all_of(data.begin() + 11, data.begin() + 22,
                    [](std::uint8_t v) { return v == 0; })
            && data[22] == 0x02 && data[23] == 0x00
            && data[24] == 0x92 && data[25] == 0xF0) {
        return V3_EXTENDED_CLEAR_SIZE;
    }

    V3ExtendedLayout layout{};
    if (!extended_update_layout(data, layout)) return 0;
    const auto next_capability = data.subspan(18, 4);
    const std::uint8_t expected_generation =
        static_cast<std::uint8_t>(current_capability_generation(image, layout) + 1u);
    const bool header_ok = data[11] == 0x01 && data[12] == 0x01
        && std::all_of(data.begin() + 14, data.begin() + 18,
                       [](std::uint8_t v) { return v == 0xFF; });
    return header_ok && sector1_capability_valid(next_capability)
        && next_capability[2] == expected_generation
        ? V3_EXTENDED_UPDATE_SIZE : 0;
}

WriteApplyResult apply_v3_write_staging(std::span<const std::uint8_t> staging,
                                        std::span<const std::uint8_t> coverage,
                                        std::span<std::uint8_t> image) {
    WriteApplyResult result{};
    if (staging.size() != WRITE_STAGING_SIZE) {
        result.error = "v3 write staging image must be exactly 454 bytes";
        return result;
    }
    if (!all_covered(coverage, WRITE_STAGING_SIZE)) {
        result.error = "v3 write staging image is incomplete";
        return result;
    }
    std::string validation_error;
    if (!validate_v3_dump(image, &validation_error)) {
        result.error = std::move(validation_error);
        return result;
    }
    if (!is_v3_write_start(staging, image)) {
        result.error = std::equal(image.begin(), image.begin() + 7, staging.begin() + 2)
            ? "invalid v3 write header" : "v3 write UID does not match the selected Amiibo";
        return result;
    }

    const std::size_t record_count = staging[21];
    std::vector<std::uint8_t> candidate(image.begin(), image.end());
    std::size_t cursor = 22;
    std::size_t data_bytes = 0;
    for (std::size_t record = 0; record < record_count; ++record) {
        if (cursor + 2 > staging.size()) {
            result.error = "truncated v3 write-record header";
            return result;
        }
        const std::uint8_t page = staging[cursor++];
        const std::size_t length = staging[cursor++];
        const std::size_t address = static_cast<std::size_t>(page) * 4u;
        if (page == 0 || length == 0 || cursor + length > staging.size()
                || address < 20 || address + length > V3_WRITE_END) {
            result.error = "v3 write record targets a protected/out-of-range page";
            return result;
        }
        std::copy_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor), length,
                    candidate.begin() + static_cast<std::ptrdiff_t>(address));
        cursor += length;
        data_bytes += length;
    }
    if (std::any_of(staging.begin() + static_cast<std::ptrdiff_t>(cursor), staging.end(),
                    [](std::uint8_t value) { return value != 0; })) {
        result.error = "unexpected non-zero bytes after v3 write records";
        return result;
    }
    std::copy_n(staging.begin() + 17, 4, candidate.begin() + 16);
    std::copy(candidate.begin(), candidate.end(), image.begin());
    result.ok = true;
    result.record_count = record_count;
    result.data_bytes = data_bytes;
    return result;
}

WriteApplyResult apply_v3_extended_staging(std::span<const std::uint8_t> staging,
                                           std::span<const std::uint8_t> coverage,
                                           std::size_t expected_size,
                                           std::span<std::uint8_t> image) {
    WriteApplyResult result{};
    if ((expected_size != V3_EXTENDED_CLEAR_SIZE
            && expected_size != V3_EXTENDED_UPDATE_SIZE)
            || staging.size() < expected_size
            || !all_covered(coverage, expected_size)) {
        result.error = "v3 extended write staging image is incomplete";
        return result;
    }
    std::string validation_error;
    if (!validate_v3_dump(image, &validation_error)) {
        result.error = std::move(validation_error);
        return result;
    }
    const auto operation = staging.first(expected_size);
    if (v3_extended_expected_size(operation, image) != expected_size) {
        result.error = "invalid v3 extended write envelope";
        return result;
    }

    struct Record { std::uint8_t sector; std::uint8_t page; std::uint8_t length; };
    std::array<Record, 3> records{};
    std::size_t record_count = 0;
    V3ExtendedLayout layout{};
    if (expected_size == V3_EXTENDED_CLEAR_SIZE) {
        records[0] = {0x00, 0x92, 0xF0};
        records[1] = {0x00, 0xCE, 0x50};
        record_count = 2;
    } else {
        if (!extended_update_layout(operation, layout)) {
            result.error = "invalid v3 extended update allocation";
            return result;
        }
        records[0] = {0x00, 0x04, 0x04};
        records[1] = {0x00, layout.sector0_page, 0x20};
        records[2] = {0x01,
                      static_cast<std::uint8_t>(layout.sector1_capability_page + 1u),
                      0x60};
        record_count = 3;
    }

    std::size_t cursor = 23;
    std::size_t data_bytes = 0;
    for (std::size_t record = 0; record < record_count; ++record) {
        const auto expected = records[record];
        if (cursor + 3 > expected_size
                || operation[cursor] != expected.sector
                || operation[cursor + 1] != expected.page
                || operation[cursor + 2] != expected.length) {
            result.error = "invalid v3 extended write record";
            return result;
        }
        cursor += 3;
        if (cursor + expected.length > expected_size) {
            result.error = "truncated v3 extended write record";
            return result;
        }
        const std::size_t address = static_cast<std::size_t>(expected.sector) * 0x400u
            + static_cast<std::size_t>(expected.page) * 4u;
        if (address + expected.length > image.size()) {
            result.error = "v3 extended write record exceeds the image";
            return result;
        }
        cursor += expected.length;
        data_bytes += expected.length;
    }
    if (std::any_of(operation.begin() + static_cast<std::ptrdiff_t>(cursor), operation.end(),
                    [](std::uint8_t value) { return value != 0; })) {
        result.error = "unexpected non-zero bytes after v3 extended records";
        return result;
    }

    std::vector<std::uint8_t> candidate(image.begin(), image.end());
    cursor = 23;
    if (expected_size == V3_EXTENDED_UPDATE_SIZE) {
        const std::size_t capability_offset =
            0x400u + static_cast<std::size_t>(layout.sector1_capability_page) * 4u;
        std::copy_n(operation.begin() + 18, 4,
                    candidate.begin() + static_cast<std::ptrdiff_t>(capability_offset));
    }
    for (std::size_t record = 0; record < record_count; ++record) {
        const auto expected = records[record];
        cursor += 3;
        const std::size_t address = static_cast<std::size_t>(expected.sector) * 0x400u
            + static_cast<std::size_t>(expected.page) * 4u;
        if (expected.sector == 0 && expected.page == 0x04) {
            std::copy_n(operation.begin() + static_cast<std::ptrdiff_t>(cursor + 2), 2,
                        candidate.begin() + static_cast<std::ptrdiff_t>(address + 2));
        } else {
            std::copy_n(operation.begin() + static_cast<std::ptrdiff_t>(cursor),
                        expected.length,
                        candidate.begin() + static_cast<std::ptrdiff_t>(address));
        }
        cursor += expected.length;
    }
    std::copy(candidate.begin(), candidate.end(), image.begin());
    result.ok = true;
    result.record_count = record_count;
    result.data_bytes = data_bytes;
    return result;
}

bool v3_sram_response_valid(std::span<const std::uint8_t> image) {
    if (image.size() != V3_DUMP_SIZE) return false;
    const auto sram = image.subspan(V3_SRAM_OFFSET, V3_SRAM_SIZE);
    const std::uint16_t expected = crc16_mcrf4xx(sram.first(V3_SRAM_SIZE - 2));
    const std::uint16_t stored =
        static_cast<std::uint16_t>(sram[V3_SRAM_SIZE - 2]) << 8u
        | sram[V3_SRAM_SIZE - 1];
    return expected == stored;
}

} // namespace ns::s2nfc
