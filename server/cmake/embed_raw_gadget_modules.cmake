if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

set(modules)
if(DEFINED INPUT_DIR AND IS_DIRECTORY "${INPUT_DIR}")
    file(GLOB modules LIST_DIRECTORIES false "${INPUT_DIR}/raw_gadget-*.ko")
endif()
list(SORT modules)

file(WRITE "${OUTPUT_FILE}" [=[
#include "raw_gadget_embedded.hpp"

#include <array>

namespace {
]=])

set(entries "")
set(index 0)
foreach(module IN LISTS modules)
    get_filename_component(filename "${module}" NAME)
    string(REGEX REPLACE "^raw_gadget-(.*)\\.ko$" "\\1" kernel_release "${filename}")
    if(kernel_release STREQUAL filename)
        message(FATAL_ERROR "Unexpected Raw Gadget module filename: ${filename}")
    endif()

    file(READ "${module}" module_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," module_bytes "${module_hex}")
    file(APPEND "${OUTPUT_FILE}"
        "alignas(16) constexpr uint8_t module_${index}[] = {${module_bytes}};\n")
    string(APPEND entries
        "    EmbeddedRawGadgetModule{\"${kernel_release}\", std::span<const uint8_t>(module_${index})},\n")
    math(EXPR index "${index} + 1")
endforeach()

file(APPEND "${OUTPUT_FILE}" "\nconstexpr std::array<EmbeddedRawGadgetModule, ${index}> modules = {{\n${entries}}};\n")
file(APPEND "${OUTPUT_FILE}" [=[
} // namespace

std::span<const EmbeddedRawGadgetModule> embedded_raw_gadget_modules() {
    return modules;
}

const EmbeddedRawGadgetModule* find_embedded_raw_gadget_module(std::string_view kernel_release) {
    for (const auto& module : modules) {
        if (module.kernel_release == kernel_release) return &module;
    }
    return nullptr;
}
]=])

