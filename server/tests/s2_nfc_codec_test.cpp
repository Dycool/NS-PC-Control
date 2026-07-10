#include "s2_nfc_codec.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>

using namespace ns::s2nfc;

namespace {

std::array<std::uint8_t, RAW_DUMP_SIZE> make_raw() {
    std::array<std::uint8_t, RAW_DUMP_SIZE> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    raw[0] = 0x04;
    raw[1] = 0x8A;
    raw[2] = 0x6D;
    raw[3] = static_cast<std::uint8_t>(0x88 ^ raw[0] ^ raw[1] ^ raw[2]);
    raw[4] = 0x2A;
    raw[5] = 0xB7;
    raw[6] = 0x5D;
    raw[7] = 0x80;
    raw[8] = static_cast<std::uint8_t>(raw[4] ^ raw[5] ^ raw[6] ^ raw[7]);
    raw[12] = 0xF1;
    raw[13] = 0x10;
    raw[14] = 0xFF;
    raw[15] = 0xEE;
    return raw;
}

void append_record(std::array<std::uint8_t, WRITE_STAGING_SIZE>& staging,
                   std::size_t& cursor, std::uint8_t page,
                   std::span<const std::uint8_t> data) {
    staging[cursor++] = page;
    staging[cursor++] = static_cast<std::uint8_t>(data.size());
    std::copy(data.begin(), data.end(), staging.begin() + static_cast<std::ptrdiff_t>(cursor));
    cursor += data.size();
}

} // namespace

int main() {
    auto raw = make_raw();
    std::string error;
    assert(validate_raw_dump(raw, &error));

    Signature signature{};
    for (std::size_t i = 0; i < signature.size(); ++i)
        signature[i] = static_cast<std::uint8_t>(0x80u + i);

    std::array<std::uint8_t, READ_PAYLOAD_SIZE> payload{};
    const std::array<std::uint8_t, 9> read_operation_metadata = {
        0x03, 0x00, 0x3B, 0x3C, 0x77, 0x78, 0x86, 0x00, 0x00,
    };
    const std::array<std::uint8_t, 9> write_operation_metadata = {
        0x01, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    assert(build_read_buffer_payload(raw, signature, read_operation_metadata, false, payload, &error));
    assert(payload[0] == 0x01 && payload[1] == 0x58 && payload[2] == 0x02 && payload[3] == 0x04);
    assert(payload[10] == 0x07);
    assert(std::equal(signature.begin(), signature.end(), payload.begin() + 22));
    assert(std::equal(raw.begin(), raw.end(), payload.begin() + READ_METADATA_SIZE));
    assert(std::all_of(payload.begin() + READ_METADATA_SIZE + RAW_DUMP_SIZE, payload.end(),
                       [](std::uint8_t v) { return v == 0; }));

    assert(build_read_buffer_payload(raw, signature, write_operation_metadata, true, payload, &error));
    assert(payload[1] == 0x40 && payload[2] == 0x00);
    assert(payload[54] == 0x01 && payload[55] == 0x03 && payload[56] == 0x03);
    assert(std::equal(raw.begin() + 12, raw.begin() + 16, payload.begin() + READ_METADATA_SIZE));
    assert(std::all_of(payload.begin() + READ_METADATA_SIZE + 4, payload.end(),
                       [](std::uint8_t v) { return v == 0; }));

    // Exact metadata vectors recovered from the supplied successful PC2 USB
    // capture. These contain no Amiibo application pages: only UID, READ_SIG,
    // controller metadata, and the page-3 capability-container bytes.
    const Signature captured_signature = {
        0x38, 0x6D, 0x9F, 0x0F, 0xD8, 0xC4, 0x95, 0x38,
        0xDC, 0xE2, 0x8E, 0x2D, 0xFD, 0xCA, 0x9A, 0x34,
        0x12, 0x5B, 0xA6, 0x37, 0xA7, 0x7F, 0xB0, 0x9D,
        0xD9, 0x73, 0x14, 0x5F, 0xF5, 0x1D, 0x80, 0x25,
    };
    const std::array<std::uint8_t, READ_METADATA_SIZE> expected_read_metadata = {
        0x01, 0x58, 0x02, 0x04, 0x00, 0x00, 0x00, 0x01,
        0x02, 0x00, 0x07, 0x04, 0x8A, 0x6D, 0x2A, 0xB7,
        0x5D, 0x80, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6D,
        0x9F, 0x0F, 0xD8, 0xC4, 0x95, 0x38, 0xDC, 0xE2,
        0x8E, 0x2D, 0xFD, 0xCA, 0x9A, 0x34, 0x12, 0x5B,
        0xA6, 0x37, 0xA7, 0x7F, 0xB0, 0x9D, 0xD9, 0x73,
        0x14, 0x5F, 0xF5, 0x1D, 0x80, 0x25, 0x03, 0x00,
        0x3B, 0x3C, 0x77, 0x78, 0x86, 0x00, 0x00,
    };
    assert(build_read_buffer_payload(raw, captured_signature, read_operation_metadata, false, payload, &error));
    assert(std::equal(expected_read_metadata.begin(), expected_read_metadata.end(), payload.begin()));

    const std::array<std::uint8_t, 67> expected_write_prefix = {
        0x01, 0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01,
        0x02, 0x00, 0x07, 0x04, 0x8A, 0x6D, 0x2A, 0xB7,
        0x5D, 0x80, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6D,
        0x9F, 0x0F, 0xD8, 0xC4, 0x95, 0x38, 0xDC, 0xE2,
        0x8E, 0x2D, 0xFD, 0xCA, 0x9A, 0x34, 0x12, 0x5B,
        0xA6, 0x37, 0xA7, 0x7F, 0xB0, 0x9D, 0xD9, 0x73,
        0x14, 0x5F, 0xF5, 0x1D, 0x80, 0x25, 0x01, 0x03,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF1,
        0x10, 0xFF, 0xEE,
    };
    assert(build_read_buffer_payload(raw, captured_signature, write_operation_metadata, true, payload, &error));
    assert(std::equal(expected_write_prefix.begin(), expected_write_prefix.end(), payload.begin()));

    std::array<std::uint8_t, WRITE_STAGING_SIZE> staging{};
    std::array<std::uint8_t, WRITE_STAGING_SIZE> coverage{};
    staging[0] = 0xD0;
    staging[1] = 0x07;
    const auto uid = uid_from_raw(raw);
    std::copy(uid.begin(), uid.end(), staging.begin() + 2);
    staging[9] = 0x01;
    staging[11] = 0x01;
    staging[12] = 0x04;
    staging[13] = 0xFF;
    staging[14] = 0xFF;
    staging[15] = 0xFF;
    staging[16] = 0xFF;
    staging[17] = 0xA5;
    staging[18] = 0xF9;
    staging[19] = 0xA6;
    staging[20] = 0x00;
    staging[21] = 3;

    std::array<std::uint8_t, 32> record1{};
    std::array<std::uint8_t, 240> record2{};
    std::array<std::uint8_t, 152> record3{};
    std::fill(record1.begin(), record1.end(), 0x11);
    std::fill(record2.begin(), record2.end(), 0x22);
    std::fill(record3.begin(), record3.end(), 0x33);
    std::size_t cursor = 22;
    append_record(staging, cursor, 0x05, record1);
    append_record(staging, cursor, 0x20, record2);
    append_record(staging, cursor, 0x5C, record3);
    assert(cursor == 452);
    coverage.fill(1);

    auto before = raw;
    const WriteApplyResult result = apply_write_staging(staging, coverage, raw);
    assert(result.ok);
    assert(result.record_count == 3);
    assert(result.data_bytes == 424);
    assert(std::equal(raw.begin(), raw.begin() + 16, before.begin()));
    assert(raw[16] == 0xA5 && raw[17] == 0xF9 && raw[18] == 0xA6 && raw[19] == 0x00);
    assert(std::all_of(raw.begin() + 20, raw.begin() + 52, [](std::uint8_t v) { return v == 0x11; }));
    assert(std::all_of(raw.begin() + 128, raw.begin() + 368, [](std::uint8_t v) { return v == 0x22; }));
    assert(std::all_of(raw.begin() + 368, raw.begin() + 520, [](std::uint8_t v) { return v == 0x33; }));
    assert(std::equal(raw.begin() + 520, raw.end(), before.begin() + 520));

    coverage[100] = 0;
    const WriteApplyResult incomplete = apply_write_staging(staging, coverage, raw);
    assert(!incomplete.ok);

    std::cout << "s2_nfc_codec_test: ok\n";
    return 0;
}
