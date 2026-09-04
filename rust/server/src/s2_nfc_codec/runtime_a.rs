#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NfcReply {
    direction: u8,
    payload: Vec<u8>,
}

impl NfcReply {
    fn ack() -> Self {
        Self {
            direction: 0x04,
            payload: Vec::new(),
        }
    }

    fn input(payload: Vec<u8>) -> Self {
        Self {
            direction: 0x01,
            payload,
        }
    }

    #[must_use]
    pub const fn direction(&self) -> u8 {
        self.direction
    }

    #[must_use]
    pub fn payload(&self) -> &[u8] {
        &self.payload
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum V3ExtendedPhase {
    #[default]
    Idle,
    AwaitUpdate,
    UpdateCommitted,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Ntag215Runtime {
    nfc_status: u8,
    nfc_detail: u8,
    operation_active: bool,
    write_mode: bool,
    write_staging: [u8; WRITE_STAGING_SIZE],
    write_coverage: [u8; WRITE_STAGING_SIZE],
    write_committed: bool,
    operation_metadata: [u8; 9],
    op_buffer: Vec<u8>,
    tag_ejected: bool,
    represent_cooldown_until_ms: u64,
}

impl Default for Ntag215Runtime {
    fn default() -> Self {
        Self {
            nfc_status: 0x09,
            nfc_detail: 0,
            operation_active: false,
            write_mode: false,
            write_staging: [0; WRITE_STAGING_SIZE],
            write_coverage: [0; WRITE_STAGING_SIZE],
            write_committed: false,
            operation_metadata: [0; 9],
            op_buffer: Vec::new(),
            tag_ejected: false,
            represent_cooldown_until_ms: 0,
        }
    }
}

impl Ntag215Runtime {
    fn init(&mut self) {
        *self = Self::default();
    }

    fn reset_transaction(&mut self) {
        self.operation_active = false;
        self.write_mode = false;
        self.nfc_status = 0x09;
        self.nfc_detail = 0;
        self.write_committed = false;
        self.tag_ejected = false;
        self.represent_cooldown_until_ms = 0;
    }

    fn step(&mut self, now_ms: u64, sub: u8, req: &[u8], image: &mut [u8]) -> NfcReply {
        let mut reply = NfcReply::ack();
        match sub {
            0x03 => {
                if self.tag_ejected && now_ms >= self.represent_cooldown_until_ms {
                    self.tag_ejected = false;
                }
                self.operation_active = false;
                if self.write_mode {
                    self.write_mode = false;
                    self.write_staging.fill(0);
                    self.write_coverage.fill(0);
                }
                if !self.write_committed {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x04 => {
                self.operation_active = false;
                self.write_mode = false;
                if self.write_committed {
                    self.write_committed = false;
                    self.tag_ejected = true;
                    self.represent_cooldown_until_ms = now_ms.saturating_add(3000);
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                } else if !self.tag_ejected {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x05 => {
                let mut payload = vec![0u8; STATUS_PAYLOAD_SIZE];
                if !self.tag_ejected && image.len() == RAW_DUMP_SIZE {
                    payload[0] = self.nfc_status;
                    payload[1] = self.nfc_detail;
                    payload[4] = 0x01;
                    payload[5] = 0x01;
                    payload[6] = 0x02;
                    payload[7] = 0x07;
                    payload[8..15].copy_from_slice(&uid_from_raw(image));
                } else {
                    payload[0] = 0x07;
                    payload[1] = 0x41;
                }
                reply = NfcReply::input(payload);
            }
            0x06 => {
                let valid = req.len() >= 19 && req[0] == 0xd0 && req[1] == 0x07 && !self.tag_ejected;
                if valid {
                    let uid = uid_from_raw(image);
                    let is_zero_uid = req[2..9].iter().all(|value| *value == 0);
                    self.write_mode = !is_zero_uid && req[2..9] == uid;
                    self.nfc_status = 0x04;
                    self.nfc_detail = 0;
                    self.operation_active = true;
                    self.write_committed = false;
                    self.operation_metadata.copy_from_slice(&req[10..19]);
                    self.op_buffer = build_read_buffer_payload(
                        image,
                        &[0; ORIGINALITY_SIGNATURE_SIZE],
                        &self.operation_metadata,
                        self.write_mode,
                    )
                    .unwrap_or_default();
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                    self.write_mode = false;
                }
            }
            0x15 => {
                if self.operation_active && self.op_buffer.len() == READ_PAYLOAD_SIZE {
                    reply = NfcReply::input(self.op_buffer.clone());
                } else {
                    reply.direction = 0x01;
                }
            }
            0x14 => {
                if req.len() >= 4 && !self.tag_ejected && self.operation_active {
                    let offset = usize::from(u16::from_le_bytes([req[0], req[1]]));
                    let declared = usize::from(u16::from_le_bytes([req[2], req[3]]));
                    if offset + declared <= WRITE_STAGING_SIZE && req.len() >= 4 + declared {
                        self.write_staging[offset..offset + declared]
                            .copy_from_slice(&req[4..4 + declared]);
                        self.write_coverage[offset..offset + declared].fill(1);
                    }
                }
            }
            0x08 => {
                if self.write_mode && self.operation_active {
                    let result = apply_write_staging(&self.write_staging, &self.write_coverage, image);
                    if result.ok() {
                        self.nfc_status = 0x05;
                        self.nfc_detail = 0;
                        self.write_committed = true;
                        self.operation_active = false;
                        self.write_mode = false;
                    } else {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    }
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            _ => {}
        }
        reply
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct AmiiboV3Runtime {
    nfc_status: u8,
    nfc_detail: u8,
    operation_active: bool,
    device_cmd_staged: bool,
    write_mode: bool,
    extended_mode: bool,
    extended_expected_size: usize,
    extended_phase: V3ExtendedPhase,
    extended_deadline_ms: u64,
    write_staging: [u8; WRITE_STAGING_SIZE],
    write_coverage: [u8; WRITE_STAGING_SIZE],
    write_committed: bool,
    op_buffer: Vec<u8>,
    tag_ejected: bool,
    represent_cooldown_until_ms: u64,
    signature: Signature,
}

impl Default for AmiiboV3Runtime {
    fn default() -> Self {
        Self {
            nfc_status: 0x09,
            nfc_detail: 0,
            operation_active: false,
            device_cmd_staged: false,
            write_mode: false,
            extended_mode: false,
            extended_expected_size: 0,
            extended_phase: V3ExtendedPhase::Idle,
            extended_deadline_ms: 0,
            write_staging: [0; WRITE_STAGING_SIZE],
            write_coverage: [0; WRITE_STAGING_SIZE],
            write_committed: false,
            op_buffer: Vec::new(),
            tag_ejected: false,
            represent_cooldown_until_ms: 0,
            signature: Signature::default(),
        }
    }
}

