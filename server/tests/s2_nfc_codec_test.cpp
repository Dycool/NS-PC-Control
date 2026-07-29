#include "s2_nfc_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

std::vector<std::uint8_t> make_v3() {
    std::vector<std::uint8_t> image(ns::s2nfc::V3_DUMP_SIZE);
    for (std::size_t i = 0; i < image.size(); ++i)
        image[i] = static_cast<std::uint8_t>(i);
    image[0] = 0x04;
    image[7] = 0x00;
    image[8] = 0x44;
    return image;
}

std::array<std::uint8_t, 19> initial_read_request() {
    return {
        0xB8, 0x0B,                         // timeout
        0, 0, 0, 0, 0, 0, 0,              // discovery UID
        0x01, 0x04,                         // tag type, range count
        0x00, 0x3B, 0x3C, 0x77,
        0x78, 0x91, 0xE2, 0xE6,
    };
}

std::uint16_t crc16_mcrf4xx(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFF;
    for (const auto value : bytes) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>(
                (crc >> 1u) ^ ((crc & 1u) ? 0x8408u : 0u));
    }
    return crc;
}

} // namespace

int main() {
    auto image = make_v3();
    std::string error;
    if (!ns::s2nfc::validate_v3_dump(image, &error)) return 1;
    const auto uid = ns::s2nfc::uid_from_dump(image);
    if (!std::equal(uid.begin(), uid.end(), image.begin())) return 2;

    ns::s2nfc::Signature signature{};
    const auto read_request = initial_read_request();
    std::vector<std::uint8_t> operation;
    if (!ns::s2nfc::build_v3_read_buffer(
            image, signature, read_request, operation, &error)) return 3;
    if (operation.size() != 664 || operation[18] != 0x06
            || !std::equal(image.begin(), image.begin() + 240,
                           operation.begin() + ns::s2nfc::V3_OPERATION_PREFIX_SIZE)) {
        return 4;
    }

    std::array<std::uint8_t, ns::s2nfc::READ_CHUNK_PAYLOAD_SIZE> chunk{};
    std::size_t chunk_size = 0;
    if (!ns::s2nfc::build_buffer_chunk(
            operation, 0, chunk, chunk_size, &error)
            || chunk_size != 73 || chunk[0] != 0
            || chunk[1] != 70 || chunk[2] != 0) return 5;
    if (!ns::s2nfc::build_buffer_chunk(
            operation, 630, chunk, chunk_size, &error)
            || chunk_size != 37 || chunk[0] != 1
            || chunk[1] != 34 || chunk[2] != 0) return 6;

    for (std::size_t i = 0; i < ns::s2nfc::V3_SRAM_SIZE - 2; ++i)
        image[ns::s2nfc::V3_SRAM_OFFSET + i] =
            static_cast<std::uint8_t>(i * 3u);
    const auto sram_data = std::span<const std::uint8_t>(image)
        .subspan(ns::s2nfc::V3_SRAM_OFFSET, ns::s2nfc::V3_SRAM_SIZE - 2);
    const std::uint16_t crc = crc16_mcrf4xx(sram_data);
    image[ns::s2nfc::V3_SRAM_OFFSET + 62] =
        static_cast<std::uint8_t>(crc >> 8u);
    image[ns::s2nfc::V3_SRAM_OFFSET + 63] =
        static_cast<std::uint8_t>(crc);
    if (!ns::s2nfc::v3_sram_response_valid(image)) return 7;
    if (!ns::s2nfc::build_v3_device_result(image, operation, &error)
            || operation.size() != ns::s2nfc::V3_DEVICE_RESULT_SIZE
            || operation[0] != 0x18 || operation[18] != 0x06
            || !std::equal(image.begin() + ns::s2nfc::V3_SRAM_OFFSET,
                           image.begin() + ns::s2nfc::V3_SRAM_OFFSET
                               + ns::s2nfc::V3_SRAM_SIZE,
                           operation.begin() + 19)) return 8;

    std::array<std::uint8_t, ns::s2nfc::WRITE_STAGING_SIZE> staging{};
    std::array<std::uint8_t, ns::s2nfc::WRITE_STAGING_SIZE> coverage{};
    coverage.fill(1);
    std::copy_n(image.begin(), 7, staging.begin() + 2);
    staging[9] = 0x01;
    staging[10] = 0x06;
    staging[17] = 0xAA;
    staging[18] = 0xBB;
    staging[19] = 0xCC;
    staging[20] = 0xDD;
    staging[21] = 1;
    staging[22] = 5;
    staging[23] = 4;
    staging[24] = 1;
    staging[25] = 2;
    staging[26] = 3;
    staging[27] = 4;
    const auto mutable_result =
        ns::s2nfc::apply_v3_write_staging(staging, coverage, image);
    if (!mutable_result.ok || mutable_result.record_count != 1
            || image[20] != 1 || image[23] != 4
            || image[16] != 0xAA || image[19] != 0xDD) return 9;

    staging.fill(0);
    coverage.fill(0);
    std::copy_n(image.begin(), 7, staging.begin() + 2);
    staging[9] = 0x01;
    staging[10] = 0x06;
    staging[22] = 2;
    std::size_t cursor = 23;
    staging[cursor++] = 0x00;
    staging[cursor++] = 0x92;
    staging[cursor++] = 0xF0;
    std::fill_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor), 0xF0,
                std::uint8_t{0x11});
    cursor += 0xF0;
    staging[cursor++] = 0x00;
    staging[cursor++] = 0xCE;
    staging[cursor++] = 0x50;
    std::fill_n(staging.begin() + static_cast<std::ptrdiff_t>(cursor), 0x50,
                std::uint8_t{0x22});
    std::fill_n(coverage.begin(), ns::s2nfc::V3_EXTENDED_CLEAR_SIZE,
                std::uint8_t{1});
    if (ns::s2nfc::v3_extended_expected_size(
            std::span<const std::uint8_t>(staging).first(70), image)
            != ns::s2nfc::V3_EXTENDED_CLEAR_SIZE) return 10;
    const auto extended_result = ns::s2nfc::apply_v3_extended_staging(
        staging, coverage, ns::s2nfc::V3_EXTENDED_CLEAR_SIZE, image);
    if (!extended_result.ok || extended_result.record_count != 2
            || image[0x92 * 4] != 0x11 || image[0xCE * 4] != 0x22) return 11;

    std::array<std::uint8_t, 23> sector_request{};
    std::copy_n(image.begin(), 7, sector_request.begin() + 2);
    sector_request[9] = 0x01;
    sector_request[10] = 2;
    sector_request[11] = 0;
    sector_request[12] = 0x92;
    sector_request[13] = 0x99;
    sector_request[14] = 1;
    sector_request[15] = 0;
    sector_request[16] = 0x18;
    if (!ns::s2nfc::build_v3_sector_read_buffer(
            image, signature, sector_request, operation, &error)
            || operation.size() != 196 || operation[0] != 0x15
            || operation[18] != 0x06
            || operation[96] != 0xA5 || operation[98] != 0x01) return 12;

    // The second Air Riders operation is allocation-relative. Exercise the
    // non-Kirby layout observed for King Dedede (B2 and sector-1 64/65) so the
    // implementation cannot accidentally regress to a fixed page whitelist.
    staging.fill(0);
    coverage.fill(0);
    std::copy_n(image.begin(), 7, staging.begin() + 2);
    staging[9] = 0x01;
    staging[10] = 0x06;
    staging[11] = 0x01;
    staging[12] = 0x01;
    staging[13] = 0x64;
    std::fill_n(staging.begin() + 14, 4, std::uint8_t{0xFF});
    staging[18] = 0xA5;
    staging[20] = 0x02;
    staging[22] = 3;
    staging[23] = 0x00;
    staging[24] = 0x04;
    staging[25] = 0x04;
    staging[26] = 0xA5;
    staging[28] = 0x07;
    staging[30] = 0x00;
    staging[31] = 0xB2;
    staging[32] = 0x20;
    std::fill_n(staging.begin() + 33, 0x20, std::uint8_t{0x33});
    staging[65] = 0x01;
    staging[66] = 0x65;
    staging[67] = 0x60;
    std::fill_n(staging.begin() + 68, 0x60, std::uint8_t{0x44});
    std::fill_n(coverage.begin(), ns::s2nfc::V3_EXTENDED_UPDATE_SIZE,
                std::uint8_t{1});
    if (ns::s2nfc::v3_extended_expected_size(
            std::span<const std::uint8_t>(staging).first(70), image)
            != ns::s2nfc::V3_EXTENDED_UPDATE_SIZE) return 13;
    const auto update_result = ns::s2nfc::apply_v3_extended_staging(
        staging, coverage, ns::s2nfc::V3_EXTENDED_UPDATE_SIZE, image);
    const std::size_t dedede_capability = 0x400 + 0x64 * 4;
    if (!update_result.ok || update_result.record_count != 3
            || image[0xB2 * 4] != 0x33 || image[0x400 + 0x65 * 4] != 0x44
            || image[dedede_capability] != 0xA5
            || image[dedede_capability + 2] != 0x02) return 14;

    sector_request[12] = 0xB2;
    sector_request[13] = 0xB9;
    sector_request[15] = 0x64;
    sector_request[16] = 0x7C;
    if (!ns::s2nfc::build_v3_sector_read_buffer(
            image, signature, sector_request, operation, &error)
            || operation.size() != 196
            || operation[96] != 0xA5 || operation[98] != 0x02) return 15;

    return 0;
}
