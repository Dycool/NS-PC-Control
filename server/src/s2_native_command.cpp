#include "s2_native_command.hpp"

S2StreamingCommandValidation validate_s2_streaming_command(
    std::span<const uint8_t> command) {
    if (command.size() < 4 || command[0] != 0x03 || command[3] != 0x0A) {
        return {};
    }
    if (command.size() <= 8) {
        return {S2StreamingCommandStatus::Truncated, 0};
    }

    const uint8_t report_id = command[8];
    switch (report_id) {
        case 0x05:
        case 0x07:
        case 0x08:
        case 0x09:
            return {S2StreamingCommandStatus::Valid, report_id};
        default:
            return {S2StreamingCommandStatus::UnsupportedReportId, report_id};
    }
}

