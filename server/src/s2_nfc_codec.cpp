#include "s2_nfc_codec.hpp"

#include <algorithm>
#include <cstring>
#include <format>

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

void build_compat540(std::span<const std::uint8_t> image, std::span<std::uint8_t> out) {
    std::copy_n(image.begin(), V3_COMPAT_SPLIT, out.begin());
    std::copy_n(image.begin() + V3_COMPAT_SPLIT + V3_COMPAT_SHIFT,
                RAW_DUMP_SIZE - V3_COMPAT_SPLIT,
                out.begin() + V3_COMPAT_SPLIT);
}

} // namespace

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
    if (image[0] != 0x04 || image[7] != 0x00 || image[8] != 0x44) {
        set_error(error, "invalid NTAG I2C Plus 2K UID/manufacturer header");
        return false;
    }
    return true;
}

bool v3_sram_response_valid(std::span<const std::uint8_t> image) {
    if (image.size() != V3_DUMP_SIZE) return false;
    const auto sram = image.subspan(V3_SRAM_OFFSET, V3_SRAM_SIZE);
    const std::uint16_t expected = crc16_mcrf4xx(sram.subspan(0, V3_SRAM_DATA_SIZE));
    const std::uint16_t stored = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(sram[V3_SRAM_DATA_SIZE]) << 8u) | sram[V3_SRAM_DATA_SIZE + 1]);
    return stored == expected;
}

bool build_read_buffer_payload(std::span<const std::uint8_t> raw,
                               std::span<const std::uint8_t> signature,
                               std::span<const std::uint8_t> operation_metadata,
                               bool write_mode,
                               std::span<std::uint8_t> output,
                               std::string* error) {
    if (!validate_raw_dump(raw, error)) return false;
    if (output.size() < READ_PAYLOAD_SIZE) {
        set_error(error, std::format("output buffer must be at least {} bytes", READ_PAYLOAD_SIZE));
        return false;
    }

    std::fill(output.begin(), output.begin() + READ_PAYLOAD_SIZE, static_cast<std::uint8_t>(0));

    output[0] = 0x04;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;

    const auto uid = uid_from_raw(raw);
    std::copy_n(uid.begin(), uid.size(), output.begin() + 8);

    if (signature.size() >= ORIGINALITY_SIGNATURE_SIZE) {
        std::copy_n(signature.begin(), ORIGINALITY_SIGNATURE_SIZE, output.begin() + 19);
    } else {
        std::copy_n(FALLBACK_ORIGINALITY_SIGNATURE.begin(),
                    ORIGINALITY_SIGNATURE_SIZE, output.begin() + 19);
    }

    if (operation_metadata.size() >= 9) {
        std::copy_n(operation_metadata.begin(), 9, output.begin() + 51);
    }

    if (write_mode) {
        std::copy_n(raw.begin() + 12, 8, output.begin() + 63);
        output[71] = 0x04;
        output[72] = 0x54;
        output[73] = 0x02;
        output[74] = 0x01;
    } else {
        std::copy_n(raw.begin(), RAW_DUMP_SIZE, output.begin() + 63);
        output[603] = 0x01;
        output[604] = 0x00;
        output[605] = 0x0F;
    }

    return true;
}

WriteApplyResult apply_write_staging(std::span<const std::uint8_t> staging,
                                     std::span<const std::uint8_t> coverage,
                                     std::span<std::uint8_t> raw) {
    WriteApplyResult result;
    if (!validate_raw_dump(raw, &result.error)) return result;
    if (staging.size() < WRITE_STAGING_SIZE) {
        result.error = "staging buffer is incomplete";
        return result;
    }
    if (!all_covered(coverage, WRITE_STAGING_SIZE)) {
        result.error = "staging stream did not receive all required chunks";
        return result;
    }
    if (staging[0] != 0xD0 || staging[1] != 0x07) {
        result.error = "staging header does not start with D0 07";
        return result;
    }

    const auto dump_uid = uid_from_raw(raw);
    if (!std::equal(dump_uid.begin(), dump_uid.end(), staging.begin() + 2)) {
        result.error = "staging UID does not match the active tag";
        return result;
    }

    const std::uint8_t record_count = staging[21];
    if (record_count == 0 || record_count > 16) {
        result.error = std::format("invalid staging record count {}", record_count);
        return result;
    }

    std::size_t cursor = 22;
    std::size_t data_bytes = 0;
    for (std::uint8_t i = 0; i < record_count; ++i) {
        if (cursor + 2 > WRITE_STAGING_SIZE) {
            result.error = "staging record header overruns staging buffer";
            return result;
        }
        const std::uint8_t page = staging[cursor++];
        const std::uint8_t length = staging[cursor++];
        if (page == 0 || length == 0 || cursor + length > WRITE_STAGING_SIZE) {
            result.error = std::format("invalid staging record page {} length {}", page, length);
            return result;
        }

        const std::size_t address = static_cast<std::size_t>(page) * 4u;
        if (address < 16 || address + length > RAW_DUMP_SIZE) {
            result.error = std::format("staging record target address 0x{:03x} length {} out of bounds",
                                       address, length);
            return result;
        }
        cursor += length;
        data_bytes += length;
    }

    for (std::size_t i = cursor; i < WRITE_STAGING_SIZE; ++i) {
        if (staging[i] != 0) {
            result.error = "staging buffer contains non-zero trailing padding";
            return result;
        }
    }

    std::copy_n(staging.begin() + 17, 4, raw.begin() + 16);
    cursor = 22;
    for (std::uint8_t i = 0; i < record_count; ++i) {
        const std::uint8_t page = staging[cursor++];
        const std::uint8_t length = staging[cursor++];
        const std::size_t address = static_cast<std::size_t>(page) * 4u;
        std::copy_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor),
                    length, raw.begin() + static_cast<std::ptrdiff_t>(address));
        cursor += length;
    }

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

    std::size_t highest = 0;
    if (request.size() >= 13) {
        const std::uint8_t blocks = request[10];
        if (blocks && 11 + static_cast<std::size_t>(blocks) * 2 <= request.size()) {
            for (std::uint8_t b = 0; b < blocks; ++b) {
                const std::uint8_t st = request[11 + static_cast<std::size_t>(b) * 2];
                const std::uint8_t en = request[12 + static_cast<std::size_t>(b) * 2];
                if (en < st) continue;
                const std::size_t end_byte = (static_cast<std::size_t>(en) + 1u) * 4u;
                if (end_byte > highest) highest = end_byte;
            }
        }
    }

    std::array<std::uint8_t, RAW_DUMP_SIZE> compat{};
    std::span<const std::uint8_t> source = image;
    std::size_t source_size = V3_DUMP_SIZE;
    if (highest <= RAW_DUMP_SIZE) {
        build_compat540(image, compat);
        source = compat;
        source_size = RAW_DUMP_SIZE;
    }

    output.assign(60, 0);
    output[0] = 0x04;
    output[4] = 0x01;
    output[5] = 0x02;
    output[6] = 0x00;
    output[7] = 0x07;
    std::copy_n(image.begin(), 7, output.begin() + 8);
    output[18] = 0x06; // V3 escalation byte!

    if (signature.size() >= ORIGINALITY_SIGNATURE_SIZE) {
        std::copy_n(signature.begin(), ORIGINALITY_SIGNATURE_SIZE, output.begin() + 19);
    }
    if (request.size() >= 19) {
        std::copy_n(request.begin() + 10, 9, output.begin() + 51);
    }

    bool copied = false;
    if (request.size() >= 13) {
        const std::uint8_t blocks = request[10];
        if (blocks && 11 + static_cast<std::size_t>(blocks) * 2 <= request.size()) {
            for (std::uint8_t b = 0; b < blocks; ++b) {
                const std::uint8_t start = request[11 + static_cast<std::size_t>(b) * 2];
                const std::uint8_t end = request[12 + static_cast<std::size_t>(b) * 2];
                if (end < start) continue;
                const std::size_t from = static_cast<std::size_t>(start) * 4u;
                const std::size_t len = (static_cast<std::size_t>(end - start) + 1u) * 4u;
                if (from + len > source_size) continue;
                const auto insert_at = output.size();
                output.resize(insert_at + len);
                std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(from), len,
                            output.begin() + static_cast<std::ptrdiff_t>(insert_at));
                copied = true;
            }
        }
    }

    if (!copied) {
        const std::size_t fallback = std::min<std::size_t>(source_size, 1024);
        output.resize(60 + fallback);
        std::copy_n(source.begin(), fallback, output.begin() + 60);
    }

    return true;
}

bool build_v3_sector_read_buffer(std::span<const std::uint8_t> image,
                                 std::span<const std::uint8_t> signature,
                                 std::span<const std::uint8_t> request,
                                 std::vector<std::uint8_t>& output,
                                 std::string* error) {
    if (!validate_v3_dump(image, error)) return false;
    if (request.size() < 17 || request.size() > 23
            || !std::equal(image.begin(), image.begin() + 7, request.begin() + 2)
            || request[9] != 0x01) {
        set_error(error, "malformed sector read request");
        return false;
    }

    const std::uint8_t range_count = request[10];
    if (range_count == 0 || range_count > 2) {
        set_error(error, "invalid sector range count");
        return false;
    }
    const std::size_t ranges_end = 11 + static_cast<std::size_t>(range_count) * 3;
    if (ranges_end + 6 != request.size()) {
        set_error(error, "sector read request size mismatch");
        return false;
    }
    for (std::size_t i = ranges_end; i < request.size(); ++i) {
        if (request[i] != 0) {
            set_error(error, "non-zero reserved bytes in sector read request");
            return false;
        }
    }

    output.assign(V3_SECTOR_READ_PREFIX_SIZE, 0);
    output[0] = 0x15;
    output[4] = 0x01;
    output[5] = 0x02;
    output[7] = 0x07;
    std::copy_n(image.begin(), 7, output.begin() + 8);
    output[18] = 0x06;
    if (signature.size() >= ORIGINALITY_SIGNATURE_SIZE) {
        std::copy_n(signature.begin(), ORIGINALITY_SIGNATURE_SIZE, output.begin() + 19);
    }
    std::copy_n(request.begin() + 10, request.size() - 10, output.begin() + 51);

    static constexpr std::uint8_t initial_sector1_capability[4] = {0xA5, 0x00, 0x01, 0x00};

    for (std::uint8_t i = 0; i < range_count; ++i) {
        const std::uint8_t sector = request[11 + static_cast<std::size_t>(i) * 3];
        const std::uint8_t first = request[12 + static_cast<std::size_t>(i) * 3];
        const std::uint8_t last = request[13 + static_cast<std::size_t>(i) * 3];
        if (sector > 1 || last < first) {
            set_error(error, "invalid sector or page range");
            return false;
        }
        const std::size_t length = (static_cast<std::size_t>(last - first) + 1u) * 4u;
        const std::size_t address = static_cast<std::size_t>(sector) * 0x400u + static_cast<std::size_t>(first) * 4u;
        if (address + length > V3_DUMP_SIZE) {
            set_error(error, "sector page range out of bounds");
            return false;
        }
        const auto cursor = output.size();
        output.resize(cursor + length);
        std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(address), length, output.begin() + static_cast<std::ptrdiff_t>(cursor));

        const bool air_riders_capability_range =
            range_count == 2 && i == 1 && sector == 1 && request[11] == 0
            && (static_cast<std::uint16_t>(request[13]) - request[12] + 1u == 8u)
            && (static_cast<std::uint16_t>(last) - first + 1u == 25u);
        if (air_riders_capability_range
                && !sector1_capability_valid(std::span<const std::uint8_t>(output.data() + cursor, 4))) {
            std::copy_n(initial_sector1_capability, 4, output.data() + cursor);
        }
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

    output[19 + V3_NS_REG_OFFSET - V3_SRAM_OFFSET] |= V3_SRAM_RF_READY;

    const std::uint16_t crc = crc16_mcrf4xx(
        std::span<const std::uint8_t>(output.data() + 19, V3_SRAM_DATA_SIZE));
    write_u16le(output.data() + 19 + V3_SRAM_DATA_SIZE, crc);
    return true;
}

bool build_buffer_chunk(std::span<const std::uint8_t> buffer,
                        std::uint16_t offset,
                        std::span<std::uint8_t> output,
                        std::size_t& output_size,
                        std::string* error) {
    if (output.size() < READ_CHUNK_PAYLOAD_SIZE) {
        set_error(error, "output buffer too small for chunk payload");
        return false;
    }

    output_size = 0;
    if (offset >= buffer.size()) {
        output[0] = 0x01; // last chunk flag
        output[1] = 0x00;
        output[2] = 0x00;
        output_size = READ_CHUNK_HEADER_SIZE;
        return true;
    }

    const std::size_t chunk_len = std::min<std::size_t>(READ_CHUNK_DATA_SIZE, buffer.size() - offset);
    const bool is_last = (offset + chunk_len >= buffer.size());

    output[0] = is_last ? 0x01 : 0x00;
    write_u16le(output.data() + 1, static_cast<std::uint16_t>(chunk_len));
    std::copy_n(buffer.begin() + offset, chunk_len, output.begin() + READ_CHUNK_HEADER_SIZE);

    output_size = READ_CHUNK_HEADER_SIZE + chunk_len;
    return true;
}

bool is_v3_device_command(std::span<const std::uint8_t> data,
                          std::span<const std::uint8_t> image) {
    return data.size() == V3_DEVICE_COMMAND_SIZE
        && command_identity_matches(data, image)
        && data[10] == 0x01;
}

bool apply_v3_device_command(std::span<const std::uint8_t> data,
                             std::span<std::uint8_t> image) {
    if (!is_v3_device_command(data, image)) return false;
    return true;
}

bool is_v3_write_start(std::span<const std::uint8_t> data,
                       std::span<const std::uint8_t> image) {
    return data.size() >= 22
        && command_identity_matches(data, image)
        && data[10] == 0x06
        && data[21] >= 1 && data[21] <= 16;
}

std::size_t v3_extended_expected_size(std::span<const std::uint8_t> data,
                                       std::span<const std::uint8_t> image) {
    if (data.size() < 26 || !command_identity_matches(data, image) || data[10] != 0x06)
        return 0;

    static constexpr std::uint8_t clear_header[11] = {0};
    if (std::equal(clear_header, clear_header + 11, data.begin() + 11)
            && data[22] == 0x02
            && data[23] == 0x00 && data[24] == 0x92 && data[25] == 0xF0) {
        return V3_EXTENDED_CLEAR_SIZE;
    }

    static constexpr std::uint8_t update_prefix[2] = {0x01, 0x01};
    static constexpr std::uint8_t update_suffix[4] = {0xFF, 0xFF, 0xFF, 0xFF};

    V3ExtendedLayout layout;
    if (!extended_update_layout(data, layout)) return 0;

    const auto next_capability = data.subspan(18, 4);
    const std::uint8_t expected_gen =
        static_cast<std::uint8_t>(current_capability_generation(image, layout) + 1u);

    if (std::equal(update_prefix, update_prefix + 2, data.begin() + 11)
            && std::equal(update_suffix, update_suffix + 4, data.begin() + 14)
            && sector1_capability_valid(next_capability)
            && next_capability[2] == expected_gen) {
        return V3_EXTENDED_UPDATE_SIZE;
    }

    return 0;
}

WriteApplyResult apply_v3_write_staging(std::span<const std::uint8_t> staging,
                                         std::span<const std::uint8_t> coverage,
                                         std::span<std::uint8_t> image) {
    WriteApplyResult result;
    if (!validate_v3_dump(image, &result.error)) return result;
    if (staging.size() < WRITE_STAGING_SIZE || !all_covered(coverage, WRITE_STAGING_SIZE)) {
        result.error = "incomplete staging stream";
        return result;
    }
    if (!is_v3_write_start(staging, image)) {
        result.error = "invalid v3 write start envelope";
        return result;
    }

    const std::uint8_t record_count = staging[21];
    if (record_count == 0 || record_count > 16) {
        result.error = "invalid record count";
        return result;
    }

    std::size_t cursor = 22;
    std::size_t data_bytes = 0;
    for (std::uint8_t i = 0; i < record_count; ++i) {
        if (cursor + 2 > WRITE_STAGING_SIZE) {
            result.error = "record overruns staging buffer";
            return result;
        }
        const std::uint8_t page = staging[cursor++];
        const std::uint8_t length = staging[cursor++];
        if (page == 0 || length == 0 || cursor + length > WRITE_STAGING_SIZE) {
            result.error = "invalid record page/length";
            return result;
        }
        const std::size_t address = static_cast<std::size_t>(page) * 4u;
        if (address < 20 || address + length > V3_WRITE_END) {
            result.error = "record address out of v3 mutable bounds";
            return result;
        }
        cursor += length;
        data_bytes += length;
    }

    for (std::size_t i = cursor; i < WRITE_STAGING_SIZE; ++i) {
        if (staging[i] != 0) {
            result.error = "non-zero trailing padding in staging buffer";
            return result;
        }
    }

    std::copy_n(staging.begin() + 17, 4, image.begin() + 16);
    cursor = 22;
    for (std::uint8_t i = 0; i < record_count; ++i) {
        const std::uint8_t page = staging[cursor++];
        const std::uint8_t length = staging[cursor++];
        const std::size_t address = static_cast<std::size_t>(page) * 4u;
        std::copy_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor), length,
                    image.begin() + static_cast<std::ptrdiff_t>(address));
        cursor += length;
    }

    result.ok = true;
    result.record_count = record_count;
    result.data_bytes = data_bytes;
    return result;
}

struct V3ExtendedRecordSpec {
    std::uint8_t sector;
    std::uint8_t page;
    std::uint8_t length;
};

static constexpr V3ExtendedRecordSpec clear_records[] = {
    {0x00, 0x92, 0xF0},
    {0x00, 0xCE, 0x50},
};

static constexpr V3ExtendedRecordSpec update_control_record = {
    0x00, 0x04, 0x04,
};

WriteApplyResult apply_v3_extended_staging(std::span<const std::uint8_t> staging,
                                            std::span<const std::uint8_t> coverage,
                                            std::size_t expected_size,
                                            std::span<std::uint8_t> image) {
    WriteApplyResult result;
    if (!validate_v3_dump(image, &result.error)) return result;
    if (expected_size != V3_EXTENDED_CLEAR_SIZE && expected_size != V3_EXTENDED_UPDATE_SIZE) {
        result.error = "invalid expected extended operation size";
        return result;
    }
    if (staging.size() < expected_size || !all_covered(coverage, expected_size)) {
        result.error = "incomplete extended staging stream";
        return result;
    }

    const std::size_t classified = v3_extended_expected_size(staging.subspan(0, expected_size), image);
    if (classified != expected_size) {
        result.error = "staging content does not match expected extended operation format";
        return result;
    }

    V3ExtendedRecordSpec dynamic_update_records[3];
    const V3ExtendedRecordSpec* records = clear_records;
    std::uint8_t record_count = 2;

    if (expected_size == V3_EXTENDED_UPDATE_SIZE) {
        V3ExtendedLayout layout;
        if (!extended_update_layout(staging.subspan(0, expected_size), layout)) {
            result.error = "invalid extended update layout";
            return result;
        }
        dynamic_update_records[0] = update_control_record;
        dynamic_update_records[1] = V3ExtendedRecordSpec{0x00, layout.sector0_page, 0x20};
        dynamic_update_records[2] = V3ExtendedRecordSpec{0x01, static_cast<std::uint8_t>(layout.sector1_capability_page + 1u), 0x60};
        records = dynamic_update_records;
        record_count = 3;
    }

    std::size_t cursor = 23;
    std::size_t data_bytes = 0;
    for (std::uint8_t i = 0; i < record_count; ++i) {
        const auto& spec = records[i];
        if (cursor + 3 > expected_size
                || staging[cursor] != spec.sector
                || staging[cursor + 1] != spec.page
                || staging[cursor + 2] != spec.length) {
            result.error = "record descriptor mismatch";
            return result;
        }
        cursor += 3;
        if (cursor + spec.length > expected_size) {
            result.error = "record data overruns expected size";
            return result;
        }
        const std::size_t address = static_cast<std::size_t>(spec.sector) * 0x400u + static_cast<std::size_t>(spec.page) * 4u;
        if (address + spec.length > V3_DUMP_SIZE) {
            result.error = "record address out of bounds";
            return result;
        }
        cursor += spec.length;
        data_bytes += spec.length;
    }

    for (std::size_t i = cursor; i < expected_size; ++i) {
        if (staging[i] != 0) {
            result.error = "trailing non-zero bytes in extended staging";
            return result;
        }
    }

    cursor = 23;
    if (expected_size == V3_EXTENDED_UPDATE_SIZE) {
        const std::size_t capability_offset = 0x400u + static_cast<std::size_t>(staging[13]) * 4u;
        std::copy_n(staging.begin() + 18, 4, image.begin() + static_cast<std::ptrdiff_t>(capability_offset));
    }

    for (std::uint8_t i = 0; i < record_count; ++i) {
        const auto& spec = records[i];
        cursor += 3;
        const std::size_t address = static_cast<std::size_t>(spec.sector) * 0x400u + static_cast<std::size_t>(spec.page) * 4u;
        if (spec.sector == 0 && spec.page == 0x04) {
            std::copy_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor + 2), 2,
                        image.begin() + static_cast<std::ptrdiff_t>(address + 2));
        } else {
            std::copy_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor), spec.length,
                        image.begin() + static_cast<std::ptrdiff_t>(address));
        }
        cursor += spec.length;
    }

    result.ok = true;
    result.record_count = record_count;
    result.data_bytes = data_bytes;
    return result;
}

// --- Ntag215Runtime State Machine Implementation ---

void Ntag215Runtime::init(std::span<const uint8_t> raw, const Signature& sig) {
    reset_transaction();
    write_staging.fill(0);
    write_coverage.fill(0);
    operation_metadata.fill(0);
    op_buffer.clear();
}

void Ntag215Runtime::reset_transaction() {
    operation_active = false;
    write_mode = false;
    nfc_status = 0x09;
    nfc_detail = 0x00;
    write_committed = false;
    tag_ejected = false;
    represent_cooldown_until_ms = 0;
}

bool Ntag215Runtime::step(uint64_t now_ms, uint8_t sub, std::span<const uint8_t> req,
                          uint8_t* payload, std::size_t& payload_len, uint8_t& direction,
                          std::vector<uint8_t>& image) {
    payload_len = 0;
    direction = 0x04; // Header-only ACK by default

    switch (sub) {
    case 0x03: // enter scan
        if (tag_ejected && now_ms >= represent_cooldown_until_ms) {
            tag_ejected = false;
        }
        operation_active = false;
        if (write_mode) {
            write_mode = false;
            write_staging.fill(0);
            write_coverage.fill(0);
        }
        if (!write_committed) {
            nfc_status = 0x09;
            nfc_detail = 0x00;
        }
        break;

    case 0x04: // leave scan / stop
        operation_active = false;
        write_mode = false;
        if (write_committed) {
            write_committed = false;
            tag_ejected = true;
            represent_cooldown_until_ms = now_ms + 3000;
            nfc_status = 0x07;
            nfc_detail = 0x41;
        } else if (!tag_ejected) {
            nfc_status = 0x09;
            nfc_detail = 0x00;
        }
        break;

    case 0x05: { // status
        direction = 0x01;
        std::memset(payload, 0, STATUS_PAYLOAD_SIZE);
        if (!tag_ejected && image.size() == RAW_DUMP_SIZE) {
            payload[0] = nfc_status;
            payload[1] = nfc_detail;
            payload[4] = 0x01;
            payload[5] = 0x01;
            payload[6] = 0x02;
            payload[7] = 0x07;
            const auto uid = uid_from_raw(image);
            std::copy_n(uid.begin(), 7, payload + 8);
        } else {
            payload[0] = 0x07;
            payload[1] = 0x41;
        }
        payload_len = STATUS_PAYLOAD_SIZE;
        break;
    }

    case 0x06: { // begin read/write operation
        const bool valid = req.size() >= 19 && req[0] == 0xD0 && req[1] == 0x07 && !tag_ejected;
        if (valid) {
            const auto uid = uid_from_raw(image);
            bool is_zero_uid = true;
            for (size_t i = 2; i < 9; ++i) if (req[i] != 0) { is_zero_uid = false; break; }

            write_mode = !is_zero_uid && std::equal(uid.begin(), uid.end(), req.begin() + 2);
            nfc_status = 0x04;
            nfc_detail = 0x00;
            operation_active = true;
            write_committed = false;
            operation_metadata.fill(0);
            std::copy_n(req.begin() + 10, 9, operation_metadata.begin());

            op_buffer.resize(READ_PAYLOAD_SIZE);
            build_read_buffer_payload(image, Signature{}, operation_metadata, write_mode, op_buffer);
        } else {
            nfc_status = 0x07;
            nfc_detail = 0x41;
            write_mode = false;
        }
        break;
    }

    case 0x15: { // fetch read buffer
        direction = 0x01;
        if (operation_active && op_buffer.size() == READ_PAYLOAD_SIZE) {
            std::copy_n(op_buffer.begin(), READ_PAYLOAD_SIZE, payload);
            payload_len = READ_PAYLOAD_SIZE;
        }
        break;
    }

    case 0x14: { // stage write chunk
        if (req.size() >= 4 && !tag_ejected && operation_active) {
            const uint16_t offset = static_cast<uint16_t>(req[0]) | (static_cast<uint16_t>(req[1]) << 8);
            const uint16_t declared = static_cast<uint16_t>(req[2]) | (static_cast<uint16_t>(req[3]) << 8);
            if (offset + declared <= WRITE_STAGING_SIZE && req.size() >= 4 + declared) {
                std::copy_n(req.begin() + 4, declared, write_staging.begin() + offset);
                std::fill_n(write_coverage.begin() + offset, declared, static_cast<uint8_t>(1));
            }
        }
        break;
    }

    case 0x08: { // commit write
        if (write_mode && operation_active) {
            const auto res = apply_write_staging(write_staging, write_coverage, image);
            if (res.ok) {
                nfc_status = 0x05;
                nfc_detail = 0x00;
                write_committed = true;
                operation_active = false;
                write_mode = false;
            } else {
                nfc_status = 0x07;
                nfc_detail = 0x41;
            }
        } else {
            nfc_status = 0x07;
            nfc_detail = 0x41;
        }
        break;
    }

    default:
        break;
    }

    return true;
}

// --- AmiiboV3Runtime State Machine Implementation ---

void AmiiboV3Runtime::init(std::span<const uint8_t> v3_image, const Signature& sig) {
    reset_transaction();
    signature = sig;
    signature_set = true;
    write_staging.fill(0);
    write_coverage.fill(0);
    op_buffer.clear();
}

void AmiiboV3Runtime::reset_transaction() {
    operation_active = false;
    device_cmd_staged = false;
    write_mode = false;
    extended_mode = false;
    extended_expected_size = 0;
    extended_phase = V3ExtendedPhase::IDLE;
    extended_deadline_ms = 0;
    nfc_status = 0x09;
    nfc_detail = 0x00;
    write_committed = false;
    tag_ejected = false;
    represent_cooldown_until_ms = 0;
}

bool AmiiboV3Runtime::step(uint64_t now_ms, uint8_t sub, std::span<const uint8_t> req,
                           uint8_t* payload, std::size_t& payload_len, uint8_t& direction,
                           std::vector<uint8_t>& image) {
    payload_len = 0;
    direction = 0x04; // Header-only ACK by default

    // Expire extended sequence if deadline passed
    if (extended_phase == V3ExtendedPhase::AWAIT_UPDATE && now_ms >= extended_deadline_ms) {
        extended_phase = V3ExtendedPhase::IDLE;
    }

    // Preserve NS_REG stored in flash; SRAM_RF_READY is raised on served copy
    const uint8_t stored_ns_reg = image.size() == V3_DUMP_SIZE ? image[V3_NS_REG_OFFSET] : 0;
    if (image.size() == V3_DUMP_SIZE) {
        image[V3_NS_REG_OFFSET] |= V3_SRAM_RF_READY;
    }

    switch (sub) {
    case 0x03: // enter scan
        if (tag_ejected && now_ms >= represent_cooldown_until_ms) {
            tag_ejected = false;
        }
        operation_active = false;
        if (write_mode || extended_mode) {
            write_mode = false;
            extended_mode = false;
            extended_expected_size = 0;
            write_staging.fill(0);
            write_coverage.fill(0);
        }
        if (!write_committed) {
            nfc_status = 0x09;
            nfc_detail = 0x00;
        }
        break;

    case 0x04: { // leave scan / stop
        const bool completed_write = write_committed;
        const bool continue_extended = completed_write
            && extended_phase == V3ExtendedPhase::AWAIT_UPDATE
            && now_ms < extended_deadline_ms;

        operation_active = false;
        device_cmd_staged = false;
        write_mode = false;
        extended_mode = false;
        extended_expected_size = 0;

        if (continue_extended) {
            // Keep tag presented! Do NOT eject after 355B clear
            write_committed = false;
            tag_ejected = false;
            nfc_status = 0x09;
            nfc_detail = 0x00;
        } else if (completed_write) {
            write_committed = false;
            tag_ejected = true;
            represent_cooldown_until_ms = now_ms + 3000;
            nfc_status = 0x07;
            nfc_detail = 0x41;
        } else if (!tag_ejected) {
            nfc_status = 0x09;
            nfc_detail = 0x00;
        }
        break;
    }

    case 0x05: { // status
        direction = 0x01;
        std::memset(payload, 0, STATUS_PAYLOAD_SIZE);
        if (!tag_ejected && image.size() == V3_DUMP_SIZE) {
            payload[0] = nfc_status;
            payload[1] = nfc_detail;
            payload[4] = 0x01;
            payload[5] = 0x01;
            payload[6] = 0x02;
            payload[7] = 0x07;
            std::copy_n(image.begin(), 7, payload + 8);

            // Genuine controller reports empty status body for 0x15, 0x16, 0x18!
            if (nfc_status == 0x15 || nfc_status == 0x16 || nfc_status == 0x18) {
                std::memset(payload, 0, STATUS_PAYLOAD_SIZE);
                payload[0] = nfc_status;
            }
        } else {
            payload[0] = 0x07;
            payload[1] = 0x41;
        }
        payload_len = STATUS_PAYLOAD_SIZE;
        break;
    }

    case 0x06: { // begin v3 read descriptor
        const std::uint8_t desc_blocks = req.size() >= 11 ? req[10] : 0;
        bool zero_uid = req.size() >= 9;
        if (zero_uid) {
            for (size_t i = 2; i < 9; ++i) if (req[i] != 0) { zero_uid = false; break; }
        }
        const bool selected_uid = req.size() >= 9 && std::equal(image.begin(), image.begin() + 7, req.begin() + 2);
        const bool valid = req.size() >= 13 && desc_blocks >= 1
            && (11 + static_cast<size_t>(desc_blocks) * 2 <= req.size())
            && (zero_uid || selected_uid) && !tag_ejected;

        if (valid) {
            build_v3_read_buffer(image, signature, req, op_buffer);
            operation_active = true;
            nfc_status = 0x04; // active
            nfc_detail = 0x00;
            write_committed = false;
            write_mode = false;
            extended_mode = false;
            extended_expected_size = 0;
        } else {
            operation_active = false;
            nfc_status = 0x07;
            nfc_detail = 0x41;
        }
        break;
    }

    case 0x15: { // fetch buffer chunk
        if (operation_active && !op_buffer.empty() && req.size() >= 2) {
            const uint16_t offset = static_cast<uint16_t>(req[0]) | (static_cast<uint16_t>(req[1]) << 8);
            if (build_buffer_chunk(op_buffer, offset, std::span<uint8_t>(payload, READ_CHUNK_PAYLOAD_SIZE), payload_len)) {
                direction = 0x01;
                if (nfc_status == 0x18) {
                    nfc_status = 0x09;
                }
            }
        }
        break;
    }

    case 0x1E: { // sector-aware read
        if (!tag_ejected && build_v3_sector_read_buffer(image, signature, req, op_buffer)) {
            operation_active = true;
            nfc_status = 0x15;
            nfc_detail = 0x00;
            write_mode = false;
            extended_mode = false;
            extended_expected_size = 0;
        } else {
            operation_active = false;
            nfc_status = 0x07;
            nfc_detail = 0x41;
        }
        break;
    }

    case 0x14: { // stage chunk / device command
        if (req.size() < 4 || tag_ejected) {
            nfc_status = 0x07;
            nfc_detail = 0x41;
            break;
        }
        const uint16_t offset = static_cast<uint16_t>(req[0]) | (static_cast<uint16_t>(req[1]) << 8);
        const uint16_t declared = static_cast<uint16_t>(req[2]) | (static_cast<uint16_t>(req[3]) << 8);
        const size_t available = req.size() - 4;
        const auto data = req.subspan(4, std::min<size_t>(declared, available));

        if (declared == 0 || declared > available) {
            nfc_status = 0x07;
            nfc_detail = 0x41;
            break;
        }

        if (offset == 0 && is_v3_device_command(data, image)) {
            device_cmd_staged = true;
            break;
        }

        if (!write_mode && !extended_mode && offset == 0 && operation_active && nfc_status == 0x04) {
            if (is_v3_write_start(data, image)) {
                write_mode = true;
                write_committed = false;
                write_staging.fill(0);
                write_coverage.fill(0);
            } else if ((extended_expected_size = v3_extended_expected_size(data, image)) != 0) {
                extended_mode = true;
                write_staging.fill(0);
                write_coverage.fill(0);
            }
        }

        if (operation_active && (write_mode || extended_mode) && nfc_status == 0x04) {
            const size_t max_size = extended_mode ? extended_expected_size : WRITE_STAGING_SIZE;
            if (offset + declared <= max_size) {
                std::copy_n(data.begin(), declared, write_staging.begin() + offset);
                std::fill_n(write_coverage.begin() + offset, declared, static_cast<uint8_t>(1));
            }
        } else {
            nfc_status = 0x07;
            nfc_detail = 0x41;
        }
        break;
    }

    case 0x21: { // execute staged device command
        if (device_cmd_staged && build_v3_device_result(image, op_buffer)) {
            operation_active = true;
            nfc_status = 0x18;
            device_cmd_staged = false;
        }
        break;
    }

    case 0x08: { // commit ordinary v3 write
        if (!tag_ejected && operation_active && write_mode && nfc_status == 0x04) {
            image[V3_NS_REG_OFFSET] = stored_ns_reg;
            const auto res = apply_v3_write_staging(write_staging, write_coverage, image);
            if (res.ok) {
                nfc_status = 0x05;
                nfc_detail = 0x00;
                operation_active = false;
                write_mode = false;
                extended_expected_size = 0;
                write_committed = true;
            } else {
                nfc_status = 0x07;
                nfc_detail = 0x41;
            }
        } else {
            nfc_status = 0x07;
            nfc_detail = 0x41;
        }
        break;
    }

    case 0x20: { // commit extended sector-aware operation
        if (!tag_ejected && operation_active && extended_mode && nfc_status == 0x04) {
            image[V3_NS_REG_OFFSET] = stored_ns_reg;
            const size_t comm_size = extended_expected_size;
            const auto res = apply_v3_extended_staging(write_staging, write_coverage, comm_size, image);
            if (res.ok) {
                nfc_status = 0x16;
                nfc_detail = 0x00;
                operation_active = false;
                extended_mode = false;
                extended_expected_size = 0;

                if (comm_size == V3_EXTENDED_CLEAR_SIZE) {
                    extended_phase = V3ExtendedPhase::AWAIT_UPDATE;
                    extended_deadline_ms = now_ms + 5000;
                } else if (comm_size == V3_EXTENDED_UPDATE_SIZE) {
                    extended_phase = V3ExtendedPhase::UPDATE_COMMITTED;
                    extended_deadline_ms = 0;
                }
                write_committed = false;
            } else {
                nfc_status = 0x07;
                nfc_detail = 0x41;
            }
        } else {
            nfc_status = 0x07;
            nfc_detail = 0x41;
        }
        break;
    }

    default:
        break;
    }

    // Restore original NS_REG in image before return
    if (image.size() == V3_DUMP_SIZE) {
        image[V3_NS_REG_OFFSET] = stored_ns_reg;
    }

    return true;
}

// --- S2NfcRuntime Class Implementation ---

bool S2NfcRuntime::set_tag_data(std::span<const uint8_t> data, bool has_real_signature, const Signature& sig) {
    const size_t len = data.size();
    if (len != TAGMO_DUMP_SIZE && len != RAW_DUMP_SIZE && len != EXTENDED_DUMP_SIZE && len != V3_DUMP_SIZE) {
        return false;
    }

    const bool is_v3 = (len == V3_DUMP_SIZE);
    std::string validation_error;

    if (is_v3) {
        tag_image_.assign(data.begin(), data.end());
        if (!validate_v3_dump(tag_image_, &validation_error)) {
            tag_type_ = TagType::NONE;
            tag_image_.clear();
            return false;
        }
        tag_type_ = TagType::V3;
        has_real_signature_ = has_real_signature;
        signature_ = sig;
        modified_ = false;
        v3_.init(tag_image_, signature_);
    } else {
        tag_image_.assign(RAW_DUMP_SIZE, 0);
        const size_t raw_bytes = std::min(len, RAW_DUMP_SIZE);
        std::copy_n(data.begin(), raw_bytes, tag_image_.begin());
        if (!validate_raw_dump(tag_image_, &validation_error)) {
            tag_type_ = TagType::NONE;
            tag_image_.clear();
            return false;
        }
        tag_type_ = TagType::NTAG215;
        has_real_signature_ = has_real_signature || (len == EXTENDED_DUMP_SIZE);
        if (len == EXTENDED_DUMP_SIZE) {
            std::copy_n(data.begin() + RAW_DUMP_SIZE, ORIGINALITY_SIGNATURE_SIZE, signature_.begin());
        } else {
            signature_ = sig;
        }
        modified_ = false;
        ntag215_.init(tag_image_, signature_);
    }

    return true;
}

void S2NfcRuntime::clear() {
    tag_type_ = TagType::NONE;
    tag_image_.clear();
    signature_.fill(0);
    has_real_signature_ = false;
    modified_ = false;
    ntag215_.reset_transaction();
    v3_.reset_transaction();
}

bool S2NfcRuntime::step(uint64_t now_ms, uint8_t sub, std::span<const uint8_t> req,
                        uint8_t* payload, std::size_t& payload_len, uint8_t& direction) {
    if (tag_type_ == TagType::NONE) {
        payload_len = 0;
        direction = 0x04;
        if (sub == 0x05) { // Status
            std::memset(payload, 0, STATUS_PAYLOAD_SIZE);
            payload[0] = 0x07;
            payload[1] = 0x41;
            payload_len = STATUS_PAYLOAD_SIZE;
            direction = 0x01;
        }
        return true;
    }

    if (tag_type_ == TagType::V3) {
        const bool ok = v3_.step(now_ms, sub, req, payload, payload_len, direction, tag_image_);
        if (v3_.write_committed) modified_ = true;
        return ok;
    } else {
        const bool ok = ntag215_.step(now_ms, sub, req, payload, payload_len, direction, tag_image_);
        if (ntag215_.write_committed) modified_ = true;
        return ok;
    }
}

bool S2NfcRuntime::is_placed(uint64_t now_ms) const {
    if (tag_type_ == TagType::NONE) return false;
    if (tag_type_ == TagType::V3) return !v3_.tag_ejected;
    return !ntag215_.tag_ejected;
}

uint8_t S2NfcRuntime::nfc_status() const {
    if (tag_type_ == TagType::V3) return v3_.nfc_status;
    if (tag_type_ == TagType::NTAG215) return ntag215_.nfc_status;
    return 0x07;
}

uint8_t S2NfcRuntime::nfc_detail() const {
    if (tag_type_ == TagType::V3) return v3_.nfc_detail;
    if (tag_type_ == TagType::NTAG215) return ntag215_.nfc_detail;
    return 0x41;
}

} // namespace ns::s2nfc
