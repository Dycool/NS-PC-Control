#include "s2_vendor_rx.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

int main() {
    using namespace std::chrono;
    ns::S2VendorCommandAssembler assembler;
    const auto now = steady_clock::now();
    std::vector<std::vector<std::uint8_t>> received;
    const auto dispatch = [&](std::span<const std::uint8_t> command) {
        received.emplace_back(command.begin(), command.end());
    };

    std::array<std::uint8_t, 88> write{};
    write[0] = 0x01;
    write[1] = 0x91;
    write[3] = 0x14;
    write[5] = 80;
    for (std::size_t i = 8; i < write.size(); ++i)
        write[i] = static_cast<std::uint8_t>(i);
    assembler.feed(std::span(write).first(64), 1, now, dispatch);
    if (!received.empty()) return 1;
    assembler.feed(std::span(write).subspan(64), 1, now + milliseconds(1), dispatch);
    if (received.size() != 1 || received[0].size() != write.size()
            || !std::equal(write.begin(), write.end(), received[0].begin()))
        return 2;

    const std::array<std::uint8_t, 8> version{1, 0x91, 0, 0x0c, 0, 0, 0, 0};
    std::array<std::uint8_t, 16> combined{};
    std::copy(version.begin(), version.end(), combined.begin());
    std::copy(version.begin(), version.end(), combined.begin() + 8);
    assembler.feed(combined, 1, now + milliseconds(2), dispatch);
    if (received.size() != 3 || received[1] != received[2]
            || !std::equal(version.begin(), version.end(), received[1].begin())) return 3;

    assembler.feed(std::span(write).first(64), 1, now + milliseconds(3), dispatch);
    assembler.feed(version, 2, now + milliseconds(4), dispatch);
    if (received.size() != 4 || received.back().size() != version.size()) return 4;

    assembler.feed(std::span(write).first(64), 2, now + milliseconds(5), dispatch);
    assembler.feed(version, 2, now + seconds(2), dispatch);
    if (received.size() != 5 || received.back().size() != version.size()) return 5;

    std::vector<std::uint8_t> oversized(8 + 129 + version.size(), 0);
    oversized[0] = 1;
    oversized[1] = 0x91;
    oversized[3] = 0x14;
    oversized[5] = 129;
    std::copy(version.begin(), version.end(), oversized.end() - version.size());
    assembler.feed(oversized, 2, now + seconds(2) + milliseconds(1), dispatch);
    if (received.size() != 6 || received.back().size() != version.size()) return 6;

    return 0;
}
