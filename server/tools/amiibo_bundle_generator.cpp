#include "amiibo_library.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
void append_le32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: amiibo-bundle-generator "
                     "<amiibo_catalog.json> <build-key.bin> <output.bin>\n";
        return 2;
    }
    try {
        std::ifstream catalogue_file(argv[1]);
        if (!catalogue_file) throw std::runtime_error("cannot open catalogue");
        const nlohmann::json catalogue = nlohmann::json::parse(catalogue_file);
        const std::vector<uint8_t> key = read_binary(argv[2]);
        if (key.size() != amiibo_library::RETAIL_KEY_SIZE) {
            throw std::runtime_error("build key must be exactly 160 bytes");
        }

        struct Entry {
            uint32_t head;
            uint32_t tail;
            std::vector<uint8_t> tag;
        };
        std::vector<Entry> entries;
        std::set<uint64_t> seen;
        for (const auto& item : catalogue.at("amiibo")) {
            const uint32_t head =
                static_cast<uint32_t>(std::stoul(item.at("head").get<std::string>(),
                                                  nullptr, 16));
            const uint32_t tail =
                static_cast<uint32_t>(std::stoul(item.at("tail").get<std::string>(),
                                                  nullptr, 16));
            const uint64_t id = (static_cast<uint64_t>(head) << 32) | tail;
            if (!seen.insert(id).second) continue;
            Entry entry{head, tail, {}};
            const auto result = amiibo_library::generate_template(
                head, tail, key, entry.tag);
            if (!result) {
                throw std::runtime_error(
                    "failed to generate " + item.at("head").get<std::string>()
                    + item.at("tail").get<std::string>() + ": " + result.detail);
            }
            entries.push_back(std::move(entry));
        }
        if (entries.empty()) throw std::runtime_error("catalogue is empty");
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& left, const Entry& right) {
                      return std::tie(left.head, left.tail)
                          < std::tie(right.head, right.tail);
                  });

        constexpr uint32_t header_size = 12;
        constexpr uint32_t entry_size = 16;
        uint32_t data_offset =
            header_size + static_cast<uint32_t>(entries.size()) * entry_size;
        std::vector<uint8_t> bundle;
        bundle.reserve(data_offset + entries.size() * ns::AMIIBO_RAW_DUMP_SIZE);
        bundle.insert(bundle.end(), {'N', 'S', 'A', 'T', 1, 0, entry_size, 0});
        append_le32(bundle, static_cast<uint32_t>(entries.size()));
        for (const Entry& entry : entries) {
            append_le32(bundle, entry.head);
            append_le32(bundle, entry.tail);
            append_le32(bundle, data_offset);
            append_le32(bundle, static_cast<uint32_t>(entry.tag.size()));
            data_offset += static_cast<uint32_t>(entry.tag.size());
        }
        for (const Entry& entry : entries) {
            bundle.insert(bundle.end(), entry.tag.begin(), entry.tag.end());
        }

        const std::filesystem::path output(argv[3]);
        std::filesystem::create_directories(output.parent_path());
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bundle.data()),
                     static_cast<std::streamsize>(bundle.size()));
        if (!stream) throw std::runtime_error("cannot write output bundle");
        std::cout << "Generated " << entries.size() << " templates ("
                  << bundle.size() << " bytes)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "amiibo bundle generation failed: " << error.what() << '\n';
        return 1;
    }
}
