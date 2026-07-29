#include "s2_native_command.hpp"

#include <array>
#include <cstdint>

namespace {

std::array<uint8_t, 9> streaming_command(uint8_t report_id) {
    return {0x03, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, report_id};
}

bool is_valid(uint8_t report_id) {
    const auto command = streaming_command(report_id);
    const auto result = validate_s2_streaming_command(command);
    return result.status == S2StreamingCommandStatus::Valid
        && result.report_id == report_id;
}

} // namespace

int main() {
    if (!is_valid(0x05) || !is_valid(0x07)
            || !is_valid(0x08) || !is_valid(0x09)) return 1;

    const std::array<uint8_t, 8> truncated{
        0x03, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00};
    if (validate_s2_streaming_command(truncated).status
            != S2StreamingCommandStatus::Truncated) return 2;

    const auto unsupported = streaming_command(0x06);
    if (validate_s2_streaming_command(unsupported).status
            != S2StreamingCommandStatus::UnsupportedReportId) return 3;

    const std::array<uint8_t, 4> optional{0x22, 0x00, 0x00, 0x01};
    if (validate_s2_streaming_command(optional).status
            != S2StreamingCommandStatus::NotStreamingCommand) return 4;
    return 0;
}
