const MOTION_PERIOD_US: u64 = 3_800;
const INTEGRATION_STEP_US: u64 = 4_000;
const GYRO_FP_SHIFT: u32 = 6;
const GYRO_JITTER_LIMIT: i32 = 6 << GYRO_FP_SHIFT;
const GYRO_STILL_LIMIT: i32 = 40;
const GYRO_WARMUP_LIMIT: i32 = 40;
const GYRO_WARMUP_SAMPLES: u16 = 32;
const GYRO_COUNTS_PER_DPS: f32 = 16.384;
const DEG_TO_RAD: f32 = 0.017_453_292;
const INV_SQRT2: f32 = 0.707_106_77;
const COMPONENT_EPSILON: f32 = 0.000_01;
const ACCEL_PDU_PER_COUNT: i64 = 68_963;

pub const MOTION_CARRIER_SIZE: usize = 30;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MotionCarrierSample { accel: [i16; 3], gyro: [i16; 3] }

impl MotionCarrierSample {
    pub fn new(accel: [i16; 3], gyro: [i16; 3]) -> Self { Self { accel, gyro } }
    pub fn accel(&self) -> [i16; 3] { self.accel }
    pub fn gyro(&self) -> [i16; 3] { self.gyro }
}

#[derive(Clone, Debug)]
pub struct MotionCarrierState {
    quaternion: [f32; 4],
    gyro_lp: [i32; 3],
    gyro_bias: [i32; 3],
    gyro_prev: [i32; 3],
    gyro_jitter: [i32; 3],
    accel: [i16; 3],
    carrier: [u8; MOTION_CARRIER_SIZE],
    last_update_us: u64,
    imu_tick: u16,
    timing: u16,
    bias_warmup_samples: u16,
    omitted_component: u8,
    initialized: bool,
    bias_ready: bool,
    carrier_valid: bool,
}

impl Default for MotionCarrierState {
    fn default() -> Self {
        Self {
            quaternion: [0.0, 0.0, 0.0, 1.0], gyro_lp: [0; 3], gyro_bias: [0; 3], gyro_prev: [0; 3], gyro_jitter: [0; 3], accel: [0; 3], carrier: [0; MOTION_CARRIER_SIZE], last_update_us: 0, imu_tick: 0, timing: 0, bias_warmup_samples: 0, omitted_component: 0, initialized: false, bias_ready: false, carrier_valid: false,
        }
    }
}

impl MotionCarrierState {
    pub fn reset(&mut self) { *self = Self::default(); }
    pub fn carrier(&self) -> &[u8; MOTION_CARRIER_SIZE] { &self.carrier }
    pub fn quaternion(&self) -> [f32; 4] { self.quaternion }
    pub fn bias_ready(&self) -> bool { self.bias_ready }
    pub fn carrier_valid(&self) -> bool { self.carrier_valid }

    pub fn update(&mut self, samples: &[MotionCarrierSample; 3], now_us: u64) -> bool {
        let (accel, gyro) = prepare_sample(samples);
        if !self.initialized {
            self.initialized = true;
            self.last_update_us = now_us;
            self.accel = accel;
            for (axis, value) in gyro.into_iter().enumerate() {
                self.gyro_lp[axis] = i32::from(value) * (1 << GYRO_FP_SHIFT);
                self.gyro_prev[axis] = i32::from(value);
            }
            return self.build_carrier();
        }
        let mut elapsed_us = now_us.saturating_sub(self.last_update_us);
        if elapsed_us < MOTION_PERIOD_US { return false; }
        elapsed_us = elapsed_us.clamp(500, 16_000);
        self.last_update_us = now_us;

        let mut steady = true;
        for (axis, value) in gyro.into_iter().enumerate() {
            let current = i32::from(value);
            let delta = current.saturating_sub(self.gyro_prev[axis]).abs();
            self.gyro_prev[axis] = current;
            self.gyro_jitter[axis] += ((delta * (1 << GYRO_FP_SHIFT)) - self.gyro_jitter[axis]) >> 3;
            if self.gyro_jitter[axis] > GYRO_JITTER_LIMIT { steady = false; }
        }
        for (axis, value) in gyro.into_iter().enumerate() {
            let target = i32::from(value) * (1 << GYRO_FP_SHIFT);
            self.gyro_lp[axis] += (target - self.gyro_lp[axis]) >> 2;
            if fixed_to_counts(self.gyro_lp[axis] - self.gyro_bias[axis]).abs() > GYRO_STILL_LIMIT { steady = false; }
        }
        let mut corrected = [0_i32; 3];
        for (axis, value) in gyro.into_iter().enumerate() {
            if steady { self.gyro_bias[axis] += bias_step(self.gyro_lp[axis] - self.gyro_bias[axis]); }
            let source = if steady { self.gyro_lp[axis] } else { i32::from(value) * (1 << GYRO_FP_SHIFT) };
            corrected[axis] = fixed_to_counts(source - self.gyro_bias[axis]);
        }
        if !self.bias_ready {
            let warmup_still = gyro.iter().enumerate().all(|(axis, value)| i32::from(*value).abs() <= GYRO_WARMUP_LIMIT && self.gyro_jitter[axis] <= (GYRO_WARMUP_LIMIT << GYRO_FP_SHIFT));
            if warmup_still {
                self.bias_warmup_samples = self.bias_warmup_samples.saturating_add(1);
                for (axis, corrected_value) in corrected.iter_mut().enumerate() { self.gyro_bias[axis] = self.gyro_lp[axis]; *corrected_value = 0; }
                if self.bias_warmup_samples >= GYRO_WARMUP_SAMPLES { self.bias_ready = true; }
            } else {
                self.bias_warmup_samples = 0;
                self.gyro_bias = [0; 3];
                corrected = [0; 3];
            }
        }
        if self.bias_ready {
            let scale = DEG_TO_RAD / GYRO_COUNTS_PER_DPS;
            let omega = [corrected[0] as f32 * scale, corrected[1] as f32 * scale, corrected[2] as f32 * scale];
            let mut remaining_us = elapsed_us;
            while remaining_us != 0 {
                let step_us = remaining_us.min(INTEGRATION_STEP_US);
                integrate_body_rate(&mut self.quaternion, omega, step_us as f32 / 1_000_000.0);
                remaining_us -= step_us;
            }
        }
        let count = u16::try_from((elapsed_us + 625) / 1_250).unwrap_or(u16::MAX).clamp(1, 15);
        self.imu_tick = self.imu_tick.wrapping_add(count) & 0x0fff;
        self.timing = (count << 12) | self.imu_tick;
        self.accel = accel;
        self.build_carrier()
    }

    fn build_carrier(&mut self) -> bool {
        let mut carrier = [0_u8; MOTION_CARRIER_SIZE];
        carrier[0..2].copy_from_slice(&self.timing.to_le_bytes());
        carrier[2] = 0;
        carrier[3] = 0x0c;
        let Some(orientation) = self.encode_orientation() else { return false; };
        set_orientation(&mut carrier, orientation);
        for (axis, value) in self.accel.into_iter().enumerate() {
            let scaled = (i64::from(value) * ACCEL_PDU_PER_COUNT).clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32;
            let start = 16 + axis * 4;
            carrier[start..start + 4].copy_from_slice(&scaled.to_le_bytes());
        }
        carrier[28] = 0;
        carrier[29] = 2;
        self.carrier = carrier;
        self.carrier_valid = true;
        true
    }

    fn encode_orientation(&mut self) -> Option<[u32; 3]> {
        let wire = [self.quaternion[3], self.quaternion[0], self.quaternion[1], self.quaternion[2]];
        let mut omitted = usize::from(self.omitted_component & 3);
        let boundary_reached = wire.iter().enumerate().any(|(index, value)| index != omitted && value.abs() > INV_SQRT2);
        if boundary_reached {
            omitted = wire.iter().enumerate().max_by(|(_, left), (_, right)| left.abs().partial_cmp(&right.abs()).unwrap_or(std::cmp::Ordering::Equal)).map(|(index, _)| index).unwrap_or(0);
            self.omitted_component = omitted as u8;
        }
        let sign = if wire[omitted] < 0.0 { -1.0 } else { 1.0 };
        let values = [wire[(omitted + 1) & 3] * sign, wire[(omitted + 2) & 3] * sign, wire[(omitted + 3) & 3] * sign];
        if values.iter().any(|value| !value.is_finite() || *value < -INV_SQRT2 - COMPONENT_EPSILON || *value > INV_SQRT2 + COMPONENT_EPSILON) { return None; }
        Some([
            encode_component(values[0], 67_108_864.0, 0x03ff_ffff),
            encode_component(values[1], 33_554_432.0, 0x01ff_ffff),
            ((omitted as u32) << 24) | encode_component(values[2], 16_777_216.0, 0x00ff_ffff),
        ])
    }
}

fn fixed_to_counts(value: i32) -> i32 { value / (1 << GYRO_FP_SHIFT) }
fn bias_step(delta: i32) -> i32 {
    let step = delta / 256;
    if step == 0 && delta >= (1 << GYRO_FP_SHIFT) { 1 } else if step == 0 && delta <= -(1 << GYRO_FP_SHIFT) { -1 } else { step }
}
fn prepare_sample(samples: &[MotionCarrierSample; 3]) -> ([i16; 3], [i16; 3]) {
    let mut accel_mean = [0_i32; 3];
    for sample in samples {
        for (axis, value) in sample.accel().into_iter().enumerate() { accel_mean[axis] += i32::from(value); }
    }
    for value in &mut accel_mean { *value /= 3; }
    let newest_gyro = samples[2].gyro();
    ([clamp_i16(-accel_mean[1]), clamp_i16(accel_mean[0]), clamp_i16(accel_mean[2])], [clamp_i16(-i32::from(newest_gyro[1])), newest_gyro[0], newest_gyro[2]])
}
fn clamp_i16(value: i32) -> i16 { value.clamp(i32::from(i16::MIN), i32::from(i16::MAX)) as i16 }
fn normalize_quaternion(quaternion: &mut [f32; 4]) {
    let norm_sq = quaternion.iter().map(|value| value * value).sum::<f32>();
    if norm_sq <= 0.0 { return; }
    let mut inverse_norm = 1.5 - 0.5 * norm_sq;
    inverse_norm *= 1.5 - 0.5 * norm_sq * inverse_norm * inverse_norm;
    for value in quaternion { *value *= inverse_norm; }
}
fn integrate_body_rate(quaternion: &mut [f32; 4], omega: [f32; 3], dt_seconds: f32) {
    let [x, y, z, w] = *quaternion;
    let [ox, oy, oz] = omega;
    let half_dt = 0.5 * dt_seconds;
    quaternion[0] += half_dt * (w * ox + y * oz - z * oy);
    quaternion[1] += half_dt * (w * oy - x * oz + z * ox);
    quaternion[2] += half_dt * (w * oz + x * oy - y * ox);
    quaternion[3] += half_dt * (-x * ox - y * oy - z * oz);
    normalize_quaternion(quaternion);
}
fn encode_component(value: f32, scale: f32, maximum: u32) -> u32 {
    let scaled = (value * INV_SQRT2 + 0.5) * scale;
    if scaled <= 0.0 { 0 } else if scaled >= maximum as f32 { maximum } else { (scaled + 0.5) as u32 }
}
fn set_orientation(carrier: &mut [u8; MOTION_CARRIER_SIZE], orientation: [u32; 3]) {
    let g0 = orientation[0] & 0x03ff_ffff;
    let g1 = orientation[1] & 0x03ff_ffff;
    let g2 = orientation[2] & 0x03ff_ffff;
    carrier[5] = g0 as u8; carrier[6] = (g0 >> 8) as u8; carrier[7] = (g0 >> 16) as u8; carrier[8] = (carrier[8] & 0xfc) | ((g0 >> 24) as u8 & 0x03);
    carrier[9] = g1 as u8; carrier[10] = (g1 >> 8) as u8; carrier[11] = (g1 >> 16) as u8; carrier[12] = (carrier[12] & 0xfc) | ((g1 >> 24) as u8 & 0x03);
    carrier[13] = g2 as u8; carrier[14] = (g2 >> 8) as u8; carrier[15] = (g2 >> 16) as u8; carrier[4] = (carrier[4] & 0xfc) | ((g2 >> 24) as u8 & 0x03);
}

#[cfg(test)]
mod tests {
    use super::{MotionCarrierSample, MotionCarrierState};
    fn read_i32_le(bytes: &[u8]) -> i32 { i32::from_le_bytes(bytes.try_into().expect("four bytes")) }
    fn read_orientation(carrier: &[u8; 30]) -> [u32; 3] {
        [
            u32::from(carrier[5]) | (u32::from(carrier[6]) << 8) | (u32::from(carrier[7]) << 16) | (u32::from(carrier[8] & 0x03) << 24),
            u32::from(carrier[9]) | (u32::from(carrier[10]) << 8) | (u32::from(carrier[11]) << 16) | (u32::from(carrier[12] & 0x03) << 24),
            u32::from(carrier[13]) | (u32::from(carrier[14]) << 8) | (u32::from(carrier[15]) << 16) | (u32::from(carrier[4] & 0x03) << 24),
        ]
    }
    #[test]
    fn mirrors_hardware_validated_cpp_carrier_tests() {
        let mut state = MotionCarrierState::default();
        let mut samples = [MotionCarrierSample::new([0, 0, 4096], [0; 3]); 3];
        assert!(state.update(&samples, 1_000));
        assert!(state.carrier_valid());
        let carrier = *state.carrier();
        assert_eq!(&carrier[0..2], &[0, 0]);
        assert_eq!(&carrier[2..4], &[0, 0x0c]);
        assert_eq!(read_orientation(&carrier), [0x0200_0000, 0x0100_0000, 0x0080_0000]);
        assert_eq!(read_i32_le(&carrier[16..20]), 0);
        assert_eq!(read_i32_le(&carrier[20..24]), 0);
        assert_eq!(read_i32_le(&carrier[24..28]), 282_472_448);
        assert_eq!(&carrier[28..30], &[0, 2]);
        let held = carrier;
        assert!(!state.update(&samples, 2_000));
        assert_eq!(*state.carrier(), held);
        let mut now_us = 5_000;
        for _ in 0..40 { state.update(&samples, now_us); now_us += 4_000; }
        assert!(state.bias_ready());
        samples.fill(MotionCarrierSample::new([0, 0, 4096], [328, 0, 0]));
        let before_motion = *state.carrier();
        for _ in 0..125 { state.update(&samples, now_us); now_us += 4_000; }
        assert_ne!(*state.carrier(), before_motion);
        assert!(state.quaternion()[1].abs() > 0.05);
        state.reset();
        samples.fill(MotionCarrierSample::new([0, -4096, 0], [0; 3]));
        assert!(state.update(&samples, now_us));
        assert_eq!(read_i32_le(&state.carrier()[16..20]), 282_472_448);
    }
}
