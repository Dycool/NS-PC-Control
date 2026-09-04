#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AxisSettings { deadzone: i16, invert: bool }

impl Default for AxisSettings { fn default() -> Self { Self { deadzone: 3_000, invert: false } } }

impl AxisSettings {
    pub fn new(deadzone: i16, invert: bool) -> Result<Self, String> {
        if !(0..=32_000).contains(&deadzone) { return Err("deadzone must be in 0..=32000".to_string()); }
        Ok(Self { deadzone, invert })
    }
    pub fn apply(&self, value: i16) -> u8 {
        let mut value = i32::from(value);
        if value.abs() <= i32::from(self.deadzone) { value = 0; }
        if self.invert { value = -value; }
        (((value + 32_768) * 255) / 65_535).clamp(0, 255) as u8
    }
    pub fn deadzone(&self) -> i16 { self.deadzone }
    pub fn inverted(&self) -> bool { self.invert }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct InputSettings { axes: [AxisSettings; 4], background_input: bool }

impl Default for InputSettings { fn default() -> Self { Self { axes: [AxisSettings::default(); 4], background_input: true } } }

impl InputSettings {
    pub fn axis(&self, index: usize) -> Option<AxisSettings> { self.axes.get(index).copied() }
    pub fn set_axis(&mut self, index: usize, settings: AxisSettings) -> Result<(), String> {
        let slot = self.axes.get_mut(index).ok_or_else(|| "axis index out of range".to_string())?;
        *slot = settings;
        Ok(())
    }
    pub fn background_input(&self) -> bool { self.background_input }
    pub fn set_background_input(&mut self, enabled: bool) { self.background_input = enabled; }
}
