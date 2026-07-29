#pragma once

#include "shared/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace amiibo_library {

inline constexpr std::size_t RETAIL_KEY_SIZE = 160;

struct OperationResult {
    uint8_t code = ns::AMIIBO_LIBRARY_INVALID_REQUEST;
    uint16_t tag_size = 0;
    std::string detail;

    explicit operator bool() const noexcept {
        return code == ns::AMIIBO_LIBRARY_OK;
    }
};

OperationResult select(uint32_t head, uint32_t tail, int console_port,
                       std::vector<uint8_t>& tag,
                       std::span<const uint8_t> fallback_template = {});
OperationResult generate_template(uint32_t head, uint32_t tail,
                                  std::span<const uint8_t> retail_key,
                                  std::vector<uint8_t>& tag);
OperationResult clear();

// Called when the console has changed the currently selected tag.
bool store_writeback(int console_port, const uint8_t* data, std::size_t len,
                     std::string* error = nullptr);

} // namespace amiibo_library
