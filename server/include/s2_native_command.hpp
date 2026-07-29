#pragma once

#include <cstdint>
#include <span>

enum class S2StreamingCommandStatus : uint8_t {
    NotStreamingCommand,
    Valid,
    Truncated,
    UnsupportedReportId,
};

struct S2StreamingCommandValidation {
    S2StreamingCommandStatus status = S2StreamingCommandStatus::NotStreamingCommand;
    uint8_t report_id = 0;
};

S2StreamingCommandValidation validate_s2_streaming_command(
    std::span<const uint8_t> command);

