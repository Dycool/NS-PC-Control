impl AmiiboV3Runtime {
    fn init(&mut self, signature: Signature) {
        *self = Self {
            signature,
            ..Self::default()
        };
    }

    fn reset_transaction(&mut self) {
        let signature = self.signature;
        *self = Self {
            signature,
            ..Self::default()
        };
    }

    fn step(&mut self, now_ms: u64, sub: u8, req: &[u8], image: &mut [u8]) -> NfcReply {
        let mut reply = NfcReply::ack();
        if self.extended_phase == V3ExtendedPhase::AwaitUpdate && now_ms >= self.extended_deadline_ms {
            self.extended_phase = V3ExtendedPhase::Idle;
        }
        let stored_ns_reg = if image.len() == V3_DUMP_SIZE {
            let value = image[V3_NS_REG_OFFSET];
            image[V3_NS_REG_OFFSET] |= V3_SRAM_RF_READY;
            value
        } else {
            0
        };

        match sub {
            0x03 => {
                if self.tag_ejected && now_ms >= self.represent_cooldown_until_ms {
                    self.tag_ejected = false;
                }
                self.operation_active = false;
                if self.write_mode || self.extended_mode {
                    self.write_mode = false;
                    self.extended_mode = false;
                    self.extended_expected_size = 0;
                    self.write_staging.fill(0);
                    self.write_coverage.fill(0);
                }
                if !self.write_committed {
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                }
            }
            0x04 => {
                let completed_write = self.write_committed;
                let continue_extended = completed_write
                    && self.extended_phase == V3ExtendedPhase::AwaitUpdate
                    && now_ms < self.extended_deadline_ms;
                self.operation_active = false;
                self.device_cmd_staged = false;
                self.write_mode = false;
                self.extended_mode = false;
                self.extended_expected_size = 0;
                if continue_extended {
                    self.write_committed = false;
                    self.tag_ejected = false;
                    self.nfc_status = 0x09;
                    self.nfc_detail = 0;
                } else if completed_write {
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
                if !self.tag_ejected && image.len() == V3_DUMP_SIZE {
                    payload[0] = self.nfc_status;
                    payload[1] = self.nfc_detail;
                    payload[4] = 0x01;
                    payload[5] = 0x01;
                    payload[6] = 0x02;
                    payload[7] = 0x07;
                    payload[8..15].copy_from_slice(&image[..7]);
                    if matches!(self.nfc_status, 0x15 | 0x16 | 0x18) {
                        payload.fill(0);
                        payload[0] = self.nfc_status;
                    }
                } else {
                    payload[0] = 0x07;
                    payload[1] = 0x41;
                }
                reply = NfcReply::input(payload);
            }
            0x06 => {
                let desc_blocks = req.get(10).copied().unwrap_or(0);
                let zero_uid = req.len() >= 9 && req[2..9].iter().all(|value| *value == 0);
                let selected_uid = req.len() >= 9 && req[2..9] == image[..7];
                let valid = req.len() >= 13
                    && desc_blocks >= 1
                    && 11 + usize::from(desc_blocks) * 2 <= req.len()
                    && (zero_uid || selected_uid)
                    && !self.tag_ejected;
                if valid {
                    match build_v3_read_buffer(image, self.signature.bytes(), req) {
                        Ok(buffer) => {
                            self.op_buffer = buffer;
                            self.operation_active = true;
                            self.nfc_status = 0x04;
                            self.nfc_detail = 0;
                            self.write_committed = false;
                            self.write_mode = false;
                            self.extended_mode = false;
                            self.extended_expected_size = 0;
                        }
                        Err(_) => {
                            self.operation_active = false;
                            self.nfc_status = 0x07;
                            self.nfc_detail = 0x41;
                        }
                    }
                } else {
                    self.operation_active = false;
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            0x15 => {
                if self.operation_active && !self.op_buffer.is_empty() && req.len() >= 2 {
                    let offset = u16::from_le_bytes([req[0], req[1]]);
                    reply = NfcReply::input(build_buffer_chunk(&self.op_buffer, offset));
                    if self.nfc_status == 0x18 {
                        self.nfc_status = 0x09;
                    }
                }
            }
            0x1e => match build_v3_sector_read_buffer(image, self.signature.bytes(), req) {
                Ok(buffer) if !self.tag_ejected => {
                    self.op_buffer = buffer;
                    self.operation_active = true;
                    self.nfc_status = 0x15;
                    self.nfc_detail = 0;
                    self.write_mode = false;
                    self.extended_mode = false;
                    self.extended_expected_size = 0;
                }
                _ => {
                    self.operation_active = false;
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            },
            0x14 => {
                if req.len() < 4 || self.tag_ejected {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                } else {
                    let offset = usize::from(u16::from_le_bytes([req[0], req[1]]));
                    let declared = usize::from(u16::from_le_bytes([req[2], req[3]]));
                    let available = req.len() - 4;
                    if declared == 0 || declared > available {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    } else {
                        let data = &req[4..4 + declared];
                        if offset == 0 && is_v3_device_command(data, image) {
                            self.device_cmd_staged = true;
                        } else {
                            if !self.write_mode
                                && !self.extended_mode
                                && offset == 0
                                && self.operation_active
                                && self.nfc_status == 0x04
                            {
                                if is_v3_write_start(data, image) {
                                    self.write_mode = true;
                                    self.write_committed = false;
                                    self.write_staging.fill(0);
                                    self.write_coverage.fill(0);
                                } else {
                                    let size = v3_extended_expected_size(data, image);
                                    if size != 0 {
                                        self.extended_expected_size = size;
                                        self.extended_mode = true;
                                        self.write_staging.fill(0);
                                        self.write_coverage.fill(0);
                                    }
                                }
                            }
                            if self.operation_active
                                && (self.write_mode || self.extended_mode)
                                && self.nfc_status == 0x04
                            {
                                let max_size = if self.extended_mode {
                                    self.extended_expected_size
                                } else {
                                    WRITE_STAGING_SIZE
                                };
                                if offset + declared <= max_size {
                                    self.write_staging[offset..offset + declared].copy_from_slice(data);
                                    self.write_coverage[offset..offset + declared].fill(1);
                                } else {
                                    self.nfc_status = 0x07;
                                    self.nfc_detail = 0x41;
                                }
                            } else if !self.device_cmd_staged {
                                self.nfc_status = 0x07;
                                self.nfc_detail = 0x41;
                            }
                        }
                    }
                }
            }
            0x21 => {
                if self.device_cmd_staged && let Ok(buffer) = build_v3_device_result(image) {
                    self.op_buffer = buffer;
                    self.operation_active = true;
                    self.nfc_status = 0x18;
                    self.device_cmd_staged = false;
                }
            }
            0x08 => {
                if !self.tag_ejected
                    && self.operation_active
                    && self.write_mode
                    && self.nfc_status == 0x04
                {
                    if image.len() == V3_DUMP_SIZE {
                        image[V3_NS_REG_OFFSET] = stored_ns_reg;
                    }
                    let result = apply_v3_write_staging(&self.write_staging, &self.write_coverage, image);
                    if result.ok() {
                        self.nfc_status = 0x05;
                        self.nfc_detail = 0;
                        self.operation_active = false;
                        self.write_mode = false;
                        self.extended_expected_size = 0;
                        self.write_committed = true;
                    } else {
                        self.nfc_status = 0x07;
                        self.nfc_detail = 0x41;
                    }
                } else {
                    self.nfc_status = 0x07;
                    self.nfc_detail = 0x41;
                }
            }
            0x20 => {
                if !self.tag_ejected
                    && self.operation_active
                    && self.extended_mode
                    && self.nfc_status == 0x04
                {
                    if image.len() == V3_DUMP_SIZE {
                        image[V3_NS_REG_OFFSET] = stored_ns_reg;
                    }
                    let committed_size = self.extended_expected_size;
                    let result = apply_v3_extended_staging(
                        &self.write_staging,
                        &self.write_coverage,
                        committed_size,
                        image,
                    );
                    if result.ok() {
                        self.nfc_status = 0x16;
                        self.nfc_detail = 0;
                        self.operation_active = false;
                        self.extended_mode = false;
                        self.extended_expected_size = 0;
                        if committed_size == V3_EXTENDED_CLEAR_SIZE {
                            self.extended_phase = V3ExtendedPhase::AwaitUpdate;
                            self.extended_deadline_ms = now_ms.saturating_add(5000);
                        } else if committed_size == V3_EXTENDED_UPDATE_SIZE {
                            self.extended_phase = V3ExtendedPhase::UpdateCommitted;
                            self.extended_deadline_ms = 0;
                        }
                        self.write_committed = false;
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
        if image.len() == V3_DUMP_SIZE {
            image[V3_NS_REG_OFFSET] = stored_ns_reg;
        }
        reply
    }
}
