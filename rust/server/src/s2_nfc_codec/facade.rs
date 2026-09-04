#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TagType {
    #[default]
    None,
    Ntag215,
    V3,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct S2NfcRuntime {
    tag_type: TagType,
    tag_image: Vec<u8>,
    signature: Signature,
    has_real_signature: bool,
    modified: bool,
    ntag215: Ntag215Runtime,
    v3: AmiiboV3Runtime,
}

impl S2NfcRuntime {
    pub fn set_tag_data(
        &mut self,
        data: &[u8],
        has_real_signature: bool,
        signature: Signature,
    ) -> Result<(), NfcError> {
        if !matches!(data.len(), TAGMO_DUMP_SIZE | RAW_DUMP_SIZE | EXTENDED_DUMP_SIZE | V3_DUMP_SIZE) {
            return Err(NfcError::new("unsupported Amiibo dump size"));
        }
        if data.len() == V3_DUMP_SIZE {
            validate_v3_dump(data)?;
            self.tag_image = data.to_vec();
            self.tag_type = TagType::V3;
            self.has_real_signature = has_real_signature;
            self.signature = signature;
            self.modified = false;
            self.v3.init(signature);
        } else {
            self.tag_image = vec![0u8; RAW_DUMP_SIZE];
            let raw_bytes = data.len().min(RAW_DUMP_SIZE);
            self.tag_image[..raw_bytes].copy_from_slice(&data[..raw_bytes]);
            validate_raw_dump(&self.tag_image)?;
            self.tag_type = TagType::Ntag215;
            self.has_real_signature = has_real_signature || data.len() == EXTENDED_DUMP_SIZE;
            self.signature = if data.len() == EXTENDED_DUMP_SIZE {
                Signature::from_bytes(
                    data[RAW_DUMP_SIZE..EXTENDED_DUMP_SIZE]
                        .try_into()
                        .expect("extended signature is exactly 32 bytes"),
                )
            } else {
                signature
            };
            self.modified = false;
            self.ntag215.init();
        }
        Ok(())
    }

    pub fn clear(&mut self) {
        self.tag_type = TagType::None;
        self.tag_image.clear();
        self.signature = Signature::from_bytes([0; ORIGINALITY_SIGNATURE_SIZE]);
        self.has_real_signature = false;
        self.modified = false;
        self.ntag215.reset_transaction();
        self.v3.reset_transaction();
    }

    #[must_use]
    pub fn step(&mut self, now_ms: u64, sub: u8, req: &[u8]) -> NfcReply {
        match self.tag_type {
            TagType::None => {
                if sub == 0x05 {
                    let mut payload = vec![0u8; STATUS_PAYLOAD_SIZE];
                    payload[0] = 0x07;
                    payload[1] = 0x41;
                    NfcReply::input(payload)
                } else {
                    NfcReply::ack()
                }
            }
            TagType::V3 => {
                let reply = self.v3.step(now_ms, sub, req, &mut self.tag_image);
                if self.v3.write_committed {
                    self.modified = true;
                }
                reply
            }
            TagType::Ntag215 => {
                let reply = self.ntag215.step(now_ms, sub, req, &mut self.tag_image);
                if self.ntag215.write_committed {
                    self.modified = true;
                }
                reply
            }
        }
    }

    #[must_use]
    pub fn is_placed(&self) -> bool {
        match self.tag_type {
            TagType::None => false,
            TagType::V3 => !self.v3.tag_ejected,
            TagType::Ntag215 => !self.ntag215.tag_ejected,
        }
    }

    #[must_use]
    pub const fn is_v3(&self) -> bool {
        matches!(self.tag_type, TagType::V3)
    }

    #[must_use]
    pub const fn is_modified(&self) -> bool {
        self.modified
    }

    pub fn clear_modified(&mut self) {
        self.modified = false;
    }

    #[must_use]
    pub const fn tag_type(&self) -> TagType {
        self.tag_type
    }

    #[must_use]
    pub fn image(&self) -> &[u8] {
        &self.tag_image
    }

    pub fn replace_image(&mut self, image: Vec<u8>) -> Result<(), NfcError> {
        match self.tag_type {
            TagType::V3 => validate_v3_dump(&image)?,
            TagType::Ntag215 => validate_raw_dump(&image)?,
            TagType::None => return Err(NfcError::new("cannot replace image without an active tag")),
        }
        self.tag_image = image;
        Ok(())
    }

    #[must_use]
    pub const fn signature(&self) -> &Signature {
        &self.signature
    }

    #[must_use]
    pub const fn has_real_signature(&self) -> bool {
        self.has_real_signature
    }

    #[must_use]
    pub const fn nfc_status(&self) -> u8 {
        match self.tag_type {
            TagType::V3 => self.v3.nfc_status,
            TagType::Ntag215 => self.ntag215.nfc_status,
            TagType::None => 0x07,
        }
    }

    #[must_use]
    pub const fn nfc_detail(&self) -> u8 {
        match self.tag_type {
            TagType::V3 => self.v3.nfc_detail,
            TagType::Ntag215 => self.ntag215.nfc_detail,
            TagType::None => 0x41,
        }
    }
}
