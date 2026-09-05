use ns_shared::protocol::S2_AUDIO_USB_FRAME_BYTES;

const PLAYBACK_MAX_MS: usize = 250;
const PLAYBACK_MIN_RATIO: f64 = 0.9975;
const PLAYBACK_MAX_RATIO: f64 = 1.0025;
const PLAYBACK_TRIM_COEFF: f64 = 0.0005;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaybackBacklogAction {
    Keep,
    Reset,
}

/// Mirror the C++ playback hard-cap check. This decision is made from the
/// already-queued bytes before the newest packet is inserted, so a reset drops
/// stale audio while allowing the caller to enqueue the fresh packet afterward.
#[must_use]
pub fn playback_backlog_action(pre_queue_bytes: i32) -> PlaybackBacklogAction {
    let queued = usize::try_from(pre_queue_bytes).unwrap_or(0);
    if queued > S2_AUDIO_USB_FRAME_BYTES * PLAYBACK_MAX_MS {
        PlaybackBacklogAction::Reset
    } else {
        PlaybackBacklogAction::Keep
    }
}

/// Return whether a paused playback stream has accumulated enough post-insert
/// audio to resume. The backend remains authoritative: callers only mark the
/// stream started after the resume operation itself succeeds.
#[must_use]
pub fn playback_should_resume(
    post_queue_bytes: i32,
    target_ms: f64,
    playback_started: bool,
) -> bool {
    !playback_started && playback_queued_ms(post_queue_bytes) >= target_ms
}

/// Compute the tiny resampling correction used by the C++ client once playback
/// is running. Callers apply this only while the backend stream is started.
#[must_use]
pub fn playback_frequency_ratio(post_queue_bytes: i32, target_ms: f64) -> f64 {
    (1.0 + (playback_queued_ms(post_queue_bytes) - target_ms) * PLAYBACK_TRIM_COEFF)
        .clamp(PLAYBACK_MIN_RATIO, PLAYBACK_MAX_RATIO)
}

fn playback_queued_ms(queued_bytes: i32) -> f64 {
    let queued = usize::try_from(queued_bytes).unwrap_or(0);
    queued as f64 / S2_AUDIO_USB_FRAME_BYTES as f64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hard_cap_is_strictly_greater_than_250_ms() {
        let cap = S2_AUDIO_USB_FRAME_BYTES * PLAYBACK_MAX_MS;
        assert_eq!(
            playback_backlog_action(-1),
            PlaybackBacklogAction::Keep
        );
        assert_eq!(
            playback_backlog_action(cap as i32),
            PlaybackBacklogAction::Keep
        );
        assert_eq!(
            playback_backlog_action((cap + 1) as i32),
            PlaybackBacklogAction::Reset
        );
    }

    #[test]
    fn preroll_resumes_at_the_adaptive_target_boundary() {
        let target_ms = 10.0;
        let target_bytes = S2_AUDIO_USB_FRAME_BYTES * 10;
        assert!(!playback_should_resume(
            (target_bytes - 1) as i32,
            target_ms,
            false
        ));
        assert!(playback_should_resume(
            target_bytes as i32,
            target_ms,
            false
        ));
        assert!(!playback_should_resume(
            target_bytes as i32,
            target_ms,
            true
        ));
    }

    #[test]
    fn frequency_trim_matches_cpp_limits_and_center() {
        let target_ms = 10.0;
        let target_bytes = S2_AUDIO_USB_FRAME_BYTES * 10;
        assert_eq!(
            playback_frequency_ratio(target_bytes as i32, target_ms),
            1.0
        );
        assert_eq!(
            playback_frequency_ratio(0, target_ms),
            PLAYBACK_MIN_RATIO
        );
        assert_eq!(
            playback_frequency_ratio(-1, target_ms),
            PLAYBACK_MIN_RATIO
        );
        assert_eq!(
            playback_frequency_ratio((S2_AUDIO_USB_FRAME_BYTES * 1000) as i32, target_ms),
            PLAYBACK_MAX_RATIO
        );
    }
}
