#include "amiibo_library.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>

int main() {
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / ("ns-pc-control-amiibo-test-" + std::to_string(nonce));
    setenv("NS_PC_CONTROL_DATA_DIR", root.c_str(), 1);

    // A synthetic key is sufficient for structural tests. It is deliberately
    // not a Nintendo retail key and cannot produce a console-valid Amiibo.
    std::array<uint8_t, amiibo_library::RETAIL_KEY_SIZE> test_key{};
    test_key[31] = 0;
    test_key[111] = 0;

    std::vector<uint8_t> tag;
    auto generated = amiibo_library::generate_template(
        0x00000000u, 0x00340102u, test_key, tag);
    assert(generated);
    const std::vector<uint8_t> factory = tag;
    tag.clear();
    auto selected = amiibo_library::select(
        0x00000000u, 0x00340102u, 0, tag, factory);
    assert(selected);
    assert(tag.size() == ns::AMIIBO_RAW_DUMP_SIZE);
    assert(tag[0] == 0x04);

    const std::vector<uint8_t> original = tag;
    tag[100] ^= 0x5a;
    assert(amiibo_library::store_writeback(0, tag.data(), tag.size()));
    tag.clear();
    selected = amiibo_library::select(
        0x00000000u, 0x00340102u, 0, tag);
    assert(selected);
    assert(tag[100] == static_cast<uint8_t>(original[100] ^ 0x5a));

    tag.clear();
    generated = amiibo_library::generate_template(
        0x12345678u, 0x00000003u, test_key, tag);
    assert(generated);
    assert(tag.size() == ns::AMIIBO_V3_DUMP_SIZE);
    assert(tag[0x388] == 0x01);
    assert(tag[0x3b0] == 0x41);

    assert(amiibo_library::clear());
    tag.clear();
    selected = amiibo_library::select(
        0x00000000u, 0x00340102u, 0, tag);
    assert(!selected);
    assert(selected.code == ns::AMIIBO_LIBRARY_GENERATION_ERROR);
    return 0;
}
