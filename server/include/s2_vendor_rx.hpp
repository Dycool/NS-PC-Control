#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ns {

// Assemble complete Switch 2 vendor commands from arbitrary USB OUT fragments.
// The 0x14 NFC write request is 88 bytes and crosses a 64-byte packet boundary.
class S2VendorCommandAssembler {
public:
    static constexpr std::size_t MAX_COMMAND = 128;

    template <typename Dispatch>
    void feed(std::span<const std::uint8_t> fragment, std::uint64_t generation,
              std::chrono::steady_clock::time_point now, Dispatch&& dispatch) {
        if (generation_ != generation
                || ((length_ != 0 || discard_remaining_ != 0)
                    && now - last_fragment_ > std::chrono::seconds(1)))
            reset();
        generation_ = generation;
        last_fragment_ = now;

        std::size_t offset = 0;
        while (offset < fragment.size()) {
            if (discard_remaining_ != 0) {
                const std::size_t count = std::min(discard_remaining_, fragment.size() - offset);
                discard_remaining_ -= count;
                offset += count;
                continue;
            }

            const std::size_t target = expected_ ? expected_ : 8;
            const std::size_t count = std::min(target - length_, fragment.size() - offset);
            std::copy_n(fragment.begin() + static_cast<std::ptrdiff_t>(offset), count,
                        bytes_.begin() + static_cast<std::ptrdiff_t>(length_));
            length_ += count;
            offset += count;

            if (expected_ == 0 && length_ == 8) {
                const std::size_t payload =
                    (static_cast<std::size_t>(bytes_[4]) << 8) | bytes_[5];
                const std::size_t total = 8 + payload;
                if (bytes_[1] != 0x91 || total > MAX_COMMAND) {
                    if (bytes_[1] == 0x91 && total > MAX_COMMAND)
                        discard_remaining_ = payload;
                    length_ = expected_ = 0;
                    continue;
                }
                expected_ = total;
            }

            if (expected_ != 0 && length_ == expected_) {
                dispatch(std::span<const std::uint8_t>(bytes_.data(), length_));
                length_ = expected_ = 0;
            }
        }
    }

    void reset() { length_ = expected_ = discard_remaining_ = 0; }

private:
    std::array<std::uint8_t, MAX_COMMAND> bytes_{};
    std::size_t length_ = 0;
    std::size_t expected_ = 0;
    std::size_t discard_remaining_ = 0;
    std::uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point last_fragment_{};
};

} // namespace ns
