use ns_shared::protocol::S2_AUDIO_USB_FRAME_BYTES;
use std::collections::VecDeque;

#[derive(Default)]
pub struct AudioFrameQueue { frames: VecDeque<[u8; S2_AUDIO_USB_FRAME_BYTES]>, capacity: usize }
impl AudioFrameQueue {
    pub fn with_capacity(capacity: usize) -> Self { Self { frames: VecDeque::with_capacity(capacity), capacity: capacity.max(1) } }
    pub fn push(&mut self, frame: [u8; S2_AUDIO_USB_FRAME_BYTES]) {
        if self.capacity == 0 { self.capacity = 1; }
        while self.frames.len() >= self.capacity { self.frames.pop_front(); }
        self.frames.push_back(frame);
    }
    pub fn pop(&mut self) -> Option<[u8; S2_AUDIO_USB_FRAME_BYTES]> { self.frames.pop_front() }
    pub fn len(&self) -> usize { self.frames.len() }
    pub fn is_empty(&self) -> bool { self.frames.is_empty() }
}
