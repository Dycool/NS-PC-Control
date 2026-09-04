#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum StreamingCommandStatus {
    #[default]
    NotStreamingCommand,
    Valid,
    Truncated,
    UnsupportedReportId,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct StreamingCommandValidation { status: StreamingCommandStatus, report_id: u8 }

impl StreamingCommandValidation {
    pub fn status(&self) -> StreamingCommandStatus { self.status }
    pub fn report_id(&self) -> u8 { self.report_id }
}

pub fn validate_streaming_command(command: &[u8]) -> StreamingCommandValidation {
    if command.len() < 4 || command[0] != 0x03 || command[3] != 0x0a { return StreamingCommandValidation::default(); }
    if command.len() <= 8 { return StreamingCommandValidation { status: StreamingCommandStatus::Truncated, report_id: 0 }; }
    let report_id = command[8];
    let status = match report_id { 0x05 | 0x07 | 0x08 | 0x09 => StreamingCommandStatus::Valid, _ => StreamingCommandStatus::UnsupportedReportId };
    StreamingCommandValidation { status, report_id }
}

#[cfg(test)]
mod tests {
    use super::{validate_streaming_command, StreamingCommandStatus};
    fn command(report_id: u8) -> [u8; 9] { [0x03, 0, 0, 0x0a, 0, 0, 0, 0, report_id] }
    #[test]
    fn mirrors_cpp_validation_cases() {
        for report_id in [0x05, 0x07, 0x08, 0x09] {
            let result = validate_streaming_command(&command(report_id));
            assert_eq!(result.status(), StreamingCommandStatus::Valid);
            assert_eq!(result.report_id(), report_id);
        }
        assert_eq!(validate_streaming_command(&command(0x06)).status(), StreamingCommandStatus::UnsupportedReportId);
        assert_eq!(validate_streaming_command(&[0x03, 0, 0, 0x0a, 0, 0, 0, 0]).status(), StreamingCommandStatus::Truncated);
        assert_eq!(validate_streaming_command(&[0x22, 0, 0, 1]).status(), StreamingCommandStatus::NotStreamingCommand);
    }
}
