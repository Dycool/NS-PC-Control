#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MouseSample { delta_x: i32, delta_y: i32, scroll_y: i32, left_down: bool, right_down: bool }

impl MouseSample {
    pub fn deltas(&self) -> [i32; 3] { [self.delta_x, self.delta_y, self.scroll_y] }
    pub fn buttons(&self) -> [bool; 2] { [self.left_down, self.right_down] }
}

#[derive(Default)]
pub struct MouseAccumulator { pending_x: i64, pending_y: i64, pending_scroll: i64, left_down: bool, right_down: bool }

impl MouseAccumulator {
    pub fn add_motion(&mut self, delta_x: i32, delta_y: i32) {
        self.pending_x = self.pending_x.saturating_add(i64::from(delta_x));
        self.pending_y = self.pending_y.saturating_add(i64::from(delta_y));
    }
    pub fn add_scroll(&mut self, scroll_y: i32) { self.pending_scroll = self.pending_scroll.saturating_add(i64::from(scroll_y)); }
    pub fn set_buttons(&mut self, left_down: bool, right_down: bool) { self.left_down = left_down; self.right_down = right_down; }
    pub fn consume(&mut self) -> MouseSample {
        let delta_x = take_i32(&mut self.pending_x);
        let delta_y = take_i32(&mut self.pending_y);
        let scroll_y = take_i32(&mut self.pending_scroll);
        MouseSample { delta_x, delta_y, scroll_y, left_down: self.left_down, right_down: self.right_down }
    }
}

fn take_i32(value: &mut i64) -> i32 {
    let chunk = (*value).clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32;
    *value -= i64::from(chunk);
    chunk
}
