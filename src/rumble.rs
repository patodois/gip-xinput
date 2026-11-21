// SPDX-License-Identifier: MIT OR Apache-2.0
//! Which rumble the host wants on the pad, and when the USB worker has to send it.
//!
//! Kept free of USB and sockets so the rules can be tested without hardware.
use std::time::{Duration, Instant};

/// A client that has sent no request of any kind for this long is treated as gone (it exited,
/// crashed or stopped polling), and its rumble request is dropped. XInput keeps a vibration until
/// the game changes it, and games set it once and keep polling, so only silence ends it.
pub const CLIENT_GONE: Duration = Duration::from_millis(500);

/// Nonzero effects are sent with a 200 ms on-time (`mapping::rumble_payload`) and sent again this
/// often while they are still wanted, so a host that stops cannot leave the pad vibrating.
pub const REFRESH: Duration = Duration::from_millis(100);

const STOP: (u16, u16) = (0, 0);

/// The latest rumble request and the client that made it.
#[derive(Clone, Copy, Debug)]
pub struct Wanted {
    motors: (u16, u16),
    owner: u32,
    owner_seen: Instant,
}

impl Wanted {
    pub fn new(now: Instant) -> Self {
        Self {
            motors: STOP,
            owner: 0,
            owner_seen: now,
        }
    }

    /// Records a request of any kind from `client`; only the owner's requests keep its rumble.
    pub fn seen(&mut self, client: u32, now: Instant) {
        if client == self.owner {
            self.owner_seen = now;
        }
    }

    /// Replaces the request. The last caller wins, as with `XInputSetState`.
    pub fn set(&mut self, client: u32, motors: (u16, u16), now: Instant) {
        self.motors = motors;
        self.owner = client;
        self.owner_seen = now;
    }

    /// Forgets the request, for example when the controller is opened again.
    pub fn clear(&mut self) {
        self.motors = STOP;
    }

    /// The motors to drive now. Once the owner is gone the request is cleared, not only
    /// suspended, so it is never replayed to a later client.
    pub fn desired(&mut self, now: Instant) -> (u16, u16) {
        if now.saturating_duration_since(self.owner_seen) >= CLIENT_GONE {
            self.motors = STOP;
        }
        self.motors
    }
}

/// What was last sent to the pad.
#[derive(Clone, Copy, Debug)]
pub struct Sender {
    active: (u16, u16),
    sent_at: Instant,
}

impl Sender {
    pub fn new(now: Instant) -> Self {
        Self {
            active: STOP,
            sent_at: now,
        }
    }

    /// Whether `desired` has to be written now: it changed, or a nonzero effect needs its
    /// refresh. An unchanged request is not written again, however often a game repeats it.
    pub fn due(&self, desired: (u16, u16), now: Instant) -> bool {
        desired != self.active
            || (self.active != STOP && now.saturating_duration_since(self.sent_at) >= REFRESH)
    }

    pub fn sent(&mut self, motors: (u16, u16), now: Instant) {
        self.active = motors;
        self.sent_at = now;
    }

    /// The pad started again and its motors stopped; a wanted effect has to be sent again.
    pub fn restarted(&mut self) {
        self.active = STOP;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GAME: u32 = 100;
    const PROBE: u32 = 200;

    fn ms(n: u64) -> Duration {
        Duration::from_millis(n)
    }

    #[test]
    fn idle_stop_clears_the_request_instead_of_suspending_it() {
        let t = Instant::now();
        let mut w = Wanted::new(t);
        w.set(GAME, (1000, 2000), t);
        assert_eq!(w.desired(t + ms(499)), (1000, 2000));
        assert_eq!(w.desired(t + CLIENT_GONE), STOP);
        // A later request from anyone, of any kind, must not bring the old effect back.
        w.seen(GAME, t + ms(2000));
        w.seen(PROBE, t + ms(2000));
        assert_eq!(w.desired(t + ms(2001)), STOP);
    }

    #[test]
    fn polling_by_the_requesting_client_keeps_its_rumble() {
        let t = Instant::now();
        let mut w = Wanted::new(t);
        w.set(GAME, (1, 2), t);
        for step in 1..=10 {
            w.seen(GAME, t + ms(step * 400));
            assert_eq!(w.desired(t + ms(step * 400 + 1)), (1, 2));
        }
    }

    #[test]
    fn rumble_ends_when_the_requesting_client_goes_away_even_if_others_poll() {
        let t = Instant::now();
        let mut w = Wanted::new(t);
        w.set(GAME, (1, 2), t);
        for step in 1..=10 {
            w.seen(PROBE, t + ms(step * 100));
        }
        assert_eq!(w.desired(t + ms(1000)), STOP);
        w.seen(GAME, t + ms(1100));
        assert_eq!(w.desired(t + ms(1101)), STOP);
    }

    #[test]
    fn last_request_wins_and_moves_ownership() {
        let t = Instant::now();
        let mut w = Wanted::new(t);
        w.set(GAME, (1, 2), t);
        w.set(PROBE, (3, 4), t + ms(10));
        w.seen(GAME, t + ms(400));
        assert_eq!(w.desired(t + ms(600)), STOP);
    }

    #[test]
    fn clear_forgets_the_request() {
        let t = Instant::now();
        let mut w = Wanted::new(t);
        w.set(GAME, (1, 2), t);
        w.clear();
        assert_eq!(w.desired(t), STOP);
    }

    #[test]
    fn unchanged_requests_are_not_resent_until_the_refresh() {
        let t = Instant::now();
        let mut s = Sender::new(t);
        assert!(!s.due(STOP, t + ms(1000)), "silence is not re-sent");
        assert!(s.due((5, 6), t));
        s.sent((5, 6), t);
        assert!(!s.due((5, 6), t + ms(1)));
        assert!(!s.due((5, 6), t + ms(99)));
        assert!(s.due((5, 6), t + REFRESH));
        assert!(s.due((5, 7), t + ms(1)));
        assert!(s.due(STOP, t + ms(1)));
        s.sent(STOP, t + ms(2));
        assert!(!s.due(STOP, t + ms(500)));
    }

    #[test]
    fn a_restarted_pad_gets_the_wanted_effect_again() {
        let t = Instant::now();
        let mut s = Sender::new(t);
        s.sent((5, 6), t);
        s.restarted();
        assert!(s.due((5, 6), t + ms(1)));
        assert!(!s.due(STOP, t + ms(1)));
    }
}
