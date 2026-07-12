#pragma once

#include <cstdint>
#include <span>
#include <string_view>

struct EmbeddedRawGadgetModule {
    std::string_view kernel_release;
    std::span<const uint8_t> image;
};

std::span<const EmbeddedRawGadgetModule> embedded_raw_gadget_modules();
const EmbeddedRawGadgetModule* find_embedded_raw_gadget_module(std::string_view kernel_release);

