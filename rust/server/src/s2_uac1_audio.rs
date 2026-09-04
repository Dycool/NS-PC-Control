use ns_shared::protocol::S2_AUDIO_USB_FRAME_BYTES;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct AudioControl {
    muted: bool,
    volume_1_256_db: i16,
}

impl AudioControl {
    #[must_use]
    pub const fn new(muted: bool, volume_1_256_db: i16) -> Self {
        Self {
            muted,
            volume_1_256_db,
        }
    }

    #[must_use]
    pub const fn muted(self) -> bool {
        self.muted
    }

    #[must_use]
    pub const fn volume_1_256_db(self) -> i16 {
        self.volume_1_256_db
    }
}

#[derive(Default)]
pub struct AudioFrameQueue {
    frames: VecDeque<[u8; S2_AUDIO_USB_FRAME_BYTES]>,
    capacity: usize,
}

impl AudioFrameQueue {
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            frames: VecDeque::with_capacity(capacity.max(1)),
            capacity: capacity.max(1),
        }
    }

    pub fn push(&mut self, frame: [u8; S2_AUDIO_USB_FRAME_BYTES]) {
        if self.capacity == 0 {
            self.capacity = 1;
        }
        while self.frames.len() >= self.capacity {
            self.frames.pop_front();
        }
        self.frames.push_back(frame);
    }

    pub fn pop(&mut self) -> Option<[u8; S2_AUDIO_USB_FRAME_BYTES]> {
        self.frames.pop_front()
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.frames.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }
}

pub struct Uac1AudioState {
    ready: AtomicBool,
}

impl Default for Uac1AudioState {
    fn default() -> Self {
        Self {
            ready: AtomicBool::new(false),
        }
    }
}

impl Uac1AudioState {
    pub fn start(&self) {
        self.ready.store(true, Ordering::Release);
    }

    pub fn stop(&self) {
        self.ready.store(false, Ordering::Release);
    }

    #[must_use]
    pub fn ready(&self) -> bool {
        self.ready.load(Ordering::Acquire)
    }

    pub fn process_console_frame(
        &self,
        frame: &mut [u8; S2_AUDIO_USB_FRAME_BYTES],
        control: AudioControl,
    ) -> bool {
        if !self.ready() {
            frame.fill(0);
            return false;
        }
        apply_gain_mute(frame, control);
        true
    }

    pub fn prepare_microphone_audio(
        &self,
        data: &[u8],
        control: AudioControl,
    ) -> Option<Vec<u8>> {
        if !self.ready()
            || data.is_empty()
            || !data.len().is_multiple_of(S2_AUDIO_USB_FRAME_BYTES)
        {
            return None;
        }
        let mut scratch = data.to_vec();
        apply_gain_mute(&mut scratch, control);
        Some(scratch)
    }
}

pub fn apply_gain_mute(pcm: &mut [u8], control: AudioControl) {
    if control.muted() {
        pcm.fill(0);
        return;
    }
    if control.volume_1_256_db() == 0 {
        return;
    }
    let db = f32::from(control.volume_1_256_db()) / 256.0;
    let gain = 10.0_f32.powf(db / 20.0);
    let (samples, _) = pcm.as_chunks_mut::<2>();
    for sample_bytes in samples {
        let sample = i16::from_le_bytes(*sample_bytes);
        let scaled = (f32::from(sample) * gain).clamp(-32768.0, 32767.0);
        *sample_bytes = (scaled as i16).to_le_bytes();
    }
}

#[cfg(test)]
mod tests {
    use super::{apply_gain_mute, AudioControl, Uac1AudioState};
    use ns_shared::protocol::S2_AUDIO_USB_FRAME_BYTES;

    #[test]
    fn mute_and_volume_match_cpp_semantics() {
        let mut muted = [0xff_u8; 8];
        apply_gain_mute(&mut muted, AudioControl::new(true, 0));
        assert_eq!(muted, [0; 8]);

        let mut pcm = [0u8; 4];
        pcm[..2].copy_from_slice(&10_000_i16.to_le_bytes());
        pcm[2..].copy_from_slice(&(-10_000_i16).to_le_bytes());
        apply_gain_mute(&mut pcm, AudioControl::new(false, 6 * 256));
        let positive = i16::from_le_bytes([pcm[0], pcm[1]]);
        let negative = i16::from_le_bytes([pcm[2], pcm[3]]);
        assert!((19_900..=20_000).contains(&positive));
        assert!((-20_000..=-19_900).contains(&negative));
    }

    #[test]
    fn microphone_requires_complete_usb_frames_and_ready_state() {
        let state = Uac1AudioState::default();
        let frame = vec![0u8; S2_AUDIO_USB_FRAME_BYTES];
        assert!(state
            .prepare_microphone_audio(&frame, AudioControl::default())
            .is_none());
        state.start();
        assert!(state
            .prepare_microphone_audio(&frame, AudioControl::default())
            .is_some());
        assert!(state
            .prepare_microphone_audio(&frame[..frame.len() - 1], AudioControl::default())
            .is_none());
        state.stop();
    }
}
