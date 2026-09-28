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
    // Match the genuine PC2 NTAG215 status and offset-addressed 600-byte read.
    std::vector<std::uint8_t> ntag(ns::s2nfc::RAW_DUMP_SIZE);
    for (std::size_t i = 0; i < ntag.size(); ++i)
        ntag[i] = static_cast<std::uint8_t>(i);
    ntag[3] = static_cast<std::uint8_t>(0x88 ^ ntag[0] ^ ntag[1] ^ ntag[2]);
    ntag[8] = static_cast<std::uint8_t>(ntag[4] ^ ntag[5] ^ ntag[6] ^ ntag[7]);
    ns::s2nfc::Signature ntag_signature{};
    ntag_signature[0] = 0xA5;
    ns::s2nfc::S2NfcRuntime ntag_runtime;
    if (!ntag_runtime.set_tag_data(ntag, true, ntag_signature)) return 100;
    std::array<std::uint8_t, ns::s2nfc::READ_CHUNK_PAYLOAD_SIZE> ntag_reply{};
    std::size_t ntag_reply_size = 0;
    std::uint8_t ntag_direction = 0;
    if (!ntag_runtime.step(0, 0x05, {}, ntag_reply.data(), ntag_reply_size,
                           ntag_direction)
            || ntag_direction != 0x01
            || ntag_reply_size != ns::s2nfc::STATUS_PAYLOAD_SIZE
            || ntag_reply[8] != 0x07 || ntag_reply[9] != ntag[0]
            || ntag_reply[15] != ntag[7]) return 101;
    std::array<std::uint8_t, 19> ntag_request{};
    ntag_request[0] = 0xD0;
    ntag_request[1] = 0x07;
    ntag_request[10] = 0x01;
    if (!ntag_runtime.step(0, 0x06, ntag_request, ntag_reply.data(),
                           ntag_reply_size, ntag_direction)
            || ntag_direction != 0x04 || ntag_reply_size != 0) return 102;
    std::vector<std::uint8_t> reconstructed;
    for (std::uint16_t offset = 0; offset < ns::s2nfc::READ_PAYLOAD_SIZE;
         offset = static_cast<std::uint16_t>(reconstructed.size())) {
        const std::array<std::uint8_t, 2> request{
            static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(offset >> 8)};
        if (!ntag_runtime.step(0, 0x15, request, ntag_reply.data(),
                               ntag_reply_size, ntag_direction)
                || ntag_direction != 0x01 || ntag_reply_size > 73
                || ntag_reply_size < 4) return 103;
        const std::size_t chunk_size = ntag_reply[1] | (ntag_reply[2] << 8);
        if (chunk_size != ntag_reply_size - 3 || chunk_size > 70) return 104;
        reconstructed.insert(reconstructed.end(), ntag_reply.begin() + 3,
                             ntag_reply.begin() + ntag_reply_size);
    }
    if (reconstructed.size() != ns::s2nfc::READ_PAYLOAD_SIZE
            || reconstructed[0] != 0x04 || reconstructed[7] != 0x07
            || reconstructed[8] != ntag[0] || reconstructed[19] != 0xA5
            || !std::equal(ntag.begin(), ntag.end(), reconstructed.begin() + 60))
        return 105;
    if (!ntag_runtime.step(1, 0x04, {}, ntag_reply.data(), ntag_reply_size,
                           ntag_direction)
            || ntag_runtime.is_placed()
            || !ntag_runtime.step(4000, 0x03, {}, ntag_reply.data(),
                                  ntag_reply_size, ntag_direction)
            || ntag_runtime.is_placed()
            || !ntag_runtime.step(4000, 0x05, {}, ntag_reply.data(),
                                  ntag_reply_size, ntag_direction)
            || ntag_reply[0] != 0x07 || ntag_reply[1] != 0x41) return 107;
    // Selecting the same tag for a write lets the game's preliminary read
    // finish without removing the tag before its next scan.
    if (!ntag_runtime.set_tag_data(ntag)) return 111;
    ntag_runtime.set_defer_read_eject(true);
    const std::array<std::uint8_t, 2> ntag_last_chunk{0x30, 0x02};
    if (!ntag_runtime.step(0, 0x06, ntag_request, ntag_reply.data(),
                           ntag_reply_size, ntag_direction)
            || !ntag_runtime.step(0, 0x15, ntag_last_chunk, ntag_reply.data(),
                                  ntag_reply_size, ntag_direction)
            || !ntag_runtime.has_completed_read()
            || !ntag_runtime.step(1, 0x04, {}, ntag_reply.data(),
                                  ntag_reply_size, ntag_direction)
            || !ntag_runtime.is_placed()
            || !ntag_runtime.step(2, 0x03, {}, ntag_reply.data(),
                                  ntag_reply_size, ntag_direction)) return 112;
    auto write_request = ntag_request;
    const auto ntag_uid = ns::s2nfc::uid_from_raw(ntag);
    std::copy(ntag_uid.begin(), ntag_uid.end(), write_request.begin() + 2);
    if (!ntag_runtime.step(2, 0x06, write_request, ntag_reply.data(),
                           ntag_reply_size, ntag_direction)
            || !ntag_runtime.write_in_progress()) return 113;

    // Formatting uses a zero-UID read descriptor and only reveals its write
    // intent when the first 0x14 staging chunk arrives.
    ns::s2nfc::S2NfcRuntime format_runtime;
    if (!format_runtime.set_tag_data(ntag)
            || !format_runtime.step(0, 0x06, ntag_request, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)
            || format_runtime.write_in_progress()
            || !format_runtime.step(0, 0x15, ntag_last_chunk, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)) return 116;
    std::array<std::uint8_t, ns::s2nfc::WRITE_STAGING_SIZE> format_staging{};
    format_staging[0] = 0xD0;
    format_staging[1] = 0x07;
    std::copy(ntag_uid.begin(), ntag_uid.end(), format_staging.begin() + 2);
    format_staging[17] = 0xA5;
    format_staging[21] = 3;
    std::size_t record_offset = 22;
    for (const auto [page, length, value] : {
             std::array<std::uint8_t, 3>{5, 32, 0x11},
             std::array<std::uint8_t, 3>{32, 240, 0x22},
             std::array<std::uint8_t, 3>{92, 152, 0x33}}) {
        format_staging[record_offset++] = page;
        format_staging[record_offset++] = length;
        std::fill_n(format_staging.begin() + static_cast<std::ptrdiff_t>(record_offset),
                    length, value);
        record_offset += length;
    }
    if (record_offset != 452) return 117;
    for (std::size_t offset = 0; offset < format_staging.size(); offset += 76) {
        const std::size_t count = std::min<std::size_t>(76, format_staging.size() - offset);
        std::array<std::uint8_t, 80> frame{};
        frame[0] = static_cast<std::uint8_t>(offset);
        frame[1] = static_cast<std::uint8_t>(offset >> 8);
        frame[2] = static_cast<std::uint8_t>(count);
        std::copy_n(format_staging.begin() + static_cast<std::ptrdiff_t>(offset),
                    count, frame.begin() + 4);
        if (!format_runtime.step(0, 0x14,
                                 std::span<const std::uint8_t>(frame.data(), count + 4),
                                 ntag_reply.data(), ntag_reply_size, ntag_direction)
                || !format_runtime.write_in_progress()) return 118;
    }
    if (!format_runtime.step(0, 0x08, {}, ntag_reply.data(),
                             ntag_reply_size, ntag_direction)
            || !format_runtime.has_committed_write()
            || format_runtime.nfc_status() != 0x05
            || format_runtime.image()[20] != 0x11
            || format_runtime.image()[128] != 0x22
            || format_runtime.image()[368] != 0x33) return 119;
    format_runtime.set_write_persisted(true, 0);
    if (!format_runtime.step(1, 0x04, {}, ntag_reply.data(),
                             ntag_reply_size, ntag_direction)
            || format_runtime.is_placed()) return 120;

    auto image = make_v3();
    ns::s2nfc::S2NfcRuntime v3_runtime;
    if (!v3_runtime.set_tag_data(image)
            || !v3_runtime.step(0, 0x05, {}, ntag_reply.data(),
                                ntag_reply_size, ntag_direction)
            || ntag_direction != 0x01
            || ntag_reply_size != ns::s2nfc::STATUS_PAYLOAD_SIZE
            || ntag_reply[8] != 0x07 || ntag_reply[9] != image[0]
            || ntag_reply[15] != image[6]) return 106;
    const auto v3_read = initial_read_request();
    const std::array<std::uint8_t, 2> v3_last_chunk{0x76, 0x02};
    if (!v3_runtime.step(0, 0x06, v3_read, ntag_reply.data(),
                         ntag_reply_size, ntag_direction)
            || !v3_runtime.step(0, 0x15, v3_last_chunk, ntag_reply.data(),
                                ntag_reply_size, ntag_direction)
            || !v3_runtime.step(1, 0x04, {}, ntag_reply.data(),
                                ntag_reply_size, ntag_direction)
            || v3_runtime.is_placed()
            || !v3_runtime.step(4000, 0x03, {}, ntag_reply.data(),
                                ntag_reply_size, ntag_direction)
            || v3_runtime.is_placed()) return 108;
    if (!v3_runtime.set_tag_data(image)) return 114;
    v3_runtime.set_defer_read_eject(true);
    if (!v3_runtime.step(0, 0x06, v3_read, ntag_reply.data(),
                         ntag_reply_size, ntag_direction)
            || !v3_runtime.step(0, 0x15, v3_last_chunk, ntag_reply.data(),
                                ntag_reply_size, ntag_direction)
            || !v3_runtime.step(1, 0x04, {}, ntag_reply.data(),
                                ntag_reply_size, ntag_direction)
            || !v3_runtime.is_placed()) return 115;
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

    // PicoSwitch2 defaults to the compatibility view for the first 540-byte
    // descriptor, then serves raw v3 pages for the extended descriptor.
    auto initial_image = image;
    std::fill(initial_image.begin() + 0x80, initial_image.begin() + 0xC0, 0xA5);
    std::fill(initial_image.begin() + 0xC0, initial_image.begin() + 0x100, 0x5A);
    const std::array<std::uint8_t, 17> first_read{
        0xB8, 0x0B, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x03,
        0x00, 0x3B, 0x3C, 0x77, 0x78, 0x86,
    };
    if (!ns::s2nfc::build_v3_read_buffer(
            initial_image, signature, first_read, operation, &error)
            || operation.size() != 600 || operation[18] != 0x06
            || !std::equal(initial_image.begin() + 0xC0,
                           initial_image.begin() + 0x100,
                           operation.begin() + ns::s2nfc::V3_OPERATION_PREFIX_SIZE + 0x80))
        return 121;
    if (!ns::s2nfc::build_v3_read_buffer(
            image, signature, read_request, operation, &error)) return 122;

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

    // Replay the observed device command and both result chunks. Status must
    // remain 0x18 with an empty body when the console polls after the result.
    ns::s2nfc::S2NfcRuntime device_runtime;
    std::array<std::uint8_t, 80> device_stage{};
    device_stage[2] = 74;
    device_stage[4] = 0xD0;
    device_stage[5] = 0x07;
    std::copy_n(image.begin(), 7, device_stage.begin() + 6);
    device_stage[13] = 0x01;
    device_stage[14] = 0x01;
    device_stage[76] = 0xA4;
    device_stage[77] = 0x03;
    if (!device_runtime.set_tag_data(image)
            || !device_runtime.step(0, 0x14, device_stage, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)
            || !device_runtime.step(0, 0x21, {}, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)) return 123;
    for (const std::uint8_t offset : {0, 70}) {
        const std::array<std::uint8_t, 2> request{offset, 0};
        if (!device_runtime.step(0, 0x15, request, ntag_reply.data(),
                                 ntag_reply_size, ntag_direction)
                || ntag_direction != 0x01
                || !device_runtime.step(0, 0x05, {}, ntag_reply.data(),
                                         ntag_reply_size, ntag_direction)
                || ntag_reply_size != ns::s2nfc::STATUS_PAYLOAD_SIZE
                || ntag_reply[0] != 0x18
                || !std::all_of(ntag_reply.begin() + 1,
                                ntag_reply.begin() + ntag_reply_size,
                                [](std::uint8_t value) { return value == 0; })) return 124;
    }

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

    const auto stage_runtime = [&](ns::s2nfc::S2NfcRuntime& runtime,
                                   std::span<const std::uint8_t> bytes) {
        for (std::size_t offset = 0; offset < bytes.size(); offset += 76) {
            const auto count = std::min<std::size_t>(76, bytes.size() - offset);
            std::array<std::uint8_t, 80> frame{};
            frame[0] = static_cast<std::uint8_t>(offset);
            frame[1] = static_cast<std::uint8_t>(offset >> 8);
            frame[2] = static_cast<std::uint8_t>(count);
            std::copy_n(bytes.begin() + offset, count, frame.begin() + 4);
            if (!runtime.step(0, 0x14, std::span(frame).first(count + 4),
                              ntag_reply.data(), ntag_reply_size, ntag_direction)) return false;
        }
        return true;
    };
    // Console formatting begins this ordinary write directly from status
    // 0x18, after it fetches the 0x21 device result.
    if (!stage_runtime(device_runtime, staging)
            || !device_runtime.write_in_progress()
            || device_runtime.nfc_status() != 0x04
            || !device_runtime.step(0, 0x08, {}, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)
            || !device_runtime.has_committed_write()
            || device_runtime.nfc_status() != 0x05) return 130;
    const auto format_ordinary_staging = staging;
    device_runtime.set_write_persisted(true, 0);
    if (!device_runtime.step(1, 0x04, {}, ntag_reply.data(),
                             ntag_reply_size, ntag_direction)
            || device_runtime.is_placed()) return 131;
    ns::s2nfc::S2NfcRuntime write_runtime;
    if (!write_runtime.set_tag_data(image)
            || !write_runtime.step(0, 0x06, read_request, ntag_reply.data(),
                                   ntag_reply_size, ntag_direction)
            || !stage_runtime(write_runtime, staging)
            || !write_runtime.step(0, 0x08, {}, ntag_reply.data(),
                                   ntag_reply_size, ntag_direction)
            || !write_runtime.is_modified() || !write_runtime.has_committed_write()) return 125;
    write_runtime.clear_modified();
    write_runtime.set_write_persisted(true, 0);
    if (!write_runtime.step(1, 0x04, {}, ntag_reply.data(), ntag_reply_size, ntag_direction)
            || write_runtime.is_placed()) return 126;

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
    // The Switch sends this clear directly after the ordinary format commit,
    // without a new 0x06 read or 0x04 stop in between.
    ns::s2nfc::S2NfcRuntime format_sequence;
    if (!format_sequence.set_tag_data(image)
            || !format_sequence.step(0, 0x06, read_request, ntag_reply.data(),
                                     ntag_reply_size, ntag_direction)
            || !stage_runtime(format_sequence, format_ordinary_staging)
            || !format_sequence.step(0, 0x08, {}, ntag_reply.data(),
                                     ntag_reply_size, ntag_direction)
            || format_sequence.nfc_status() != 0x05
            || !stage_runtime(format_sequence,
                              std::span(staging).first(ns::s2nfc::V3_EXTENDED_CLEAR_SIZE))
            || !format_sequence.write_in_progress()
            || !format_sequence.step(1, 0x20, {}, ntag_reply.data(),
                                     ntag_reply_size, ntag_direction)
            || format_sequence.nfc_status() != 0x16
            || !format_sequence.is_modified()
            || !format_sequence.awaiting_v3_extended_update()) return 132;
    if (!format_sequence.step(2, 0x04, {}, ntag_reply.data(),
                             ntag_reply_size, ntag_direction)
            || !format_sequence.is_placed()
            || !format_sequence.step(3, 0x03, {}, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)
            || format_sequence.is_placed()
            || !format_sequence.step(4, 0x05, {}, ntag_reply.data(),
                                    ntag_reply_size, ntag_direction)
            || ntag_reply[0] != 0x07 || ntag_reply[1] != 0x41
            || format_sequence.awaiting_v3_extended_update()) return 133;
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
    const auto before_game_update = image;
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

    // Extended game-data commits must be persisted even before an ordinary
    // 0x08 commit; a subsequent status poll must not enqueue another save.
    ns::s2nfc::S2NfcRuntime game_runtime;
    if (!game_runtime.set_tag_data(before_game_update)
            || !game_runtime.step(0, 0x06, read_request, ntag_reply.data(),
                                  ntag_reply_size, ntag_direction)
            || !stage_runtime(game_runtime, std::span(staging).first(ns::s2nfc::V3_EXTENDED_UPDATE_SIZE))
            || !game_runtime.step(0, 0x20, {}, ntag_reply.data(), ntag_reply_size, ntag_direction)
            || !game_runtime.is_modified() || game_runtime.nfc_status() != 0x16) return 127;
    game_runtime.clear_modified();
    if (!game_runtime.step(1, 0x05, {}, ntag_reply.data(), ntag_reply_size, ntag_direction)
            || game_runtime.is_modified() || ntag_reply[0] != 0x16) return 128;
    ns::s2nfc::S2NfcRuntime reopened;
    if (!reopened.set_tag_data(game_runtime.image())
            || !reopened.step(2, 0x1E, sector_request, ntag_reply.data(), ntag_reply_size, ntag_direction)
            || reopened.nfc_status() != 0x15) return 129;

    return 0;
}
