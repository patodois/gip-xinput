// SPDX-License-Identifier: MIT OR Apache-2.0
use gip_pad::Pad;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Gamepad {
    pub buttons: u16,
    pub lt: u8,
    pub rt: u8,
    pub lx: i16,
    pub ly: i16,
    pub rx: i16,
    pub ry: i16,
}

// GIP report order -> Microsoft's public XINPUT_GAMEPAD button masks.
pub const BUTTON_MAP: [(u16, u16); 14] = [
    (1 << 2, 0x0010),
    (1 << 3, 0x0020),
    (1 << 4, 0x1000),
    (1 << 5, 0x2000),
    (1 << 6, 0x4000),
    (1 << 7, 0x8000),
    (1 << 8, 0x0001),
    (1 << 9, 0x0002),
    (1 << 10, 0x0004),
    (1 << 11, 0x0008),
    (1 << 12, 0x0100),
    (1 << 13, 0x0200),
    (1 << 14, 0x0040),
    (1 << 15, 0x0080),
];

pub fn trigger(value: u16) -> u8 {
    ((u32::from(value.min(1023)) * 255 + 511) / 1023) as u8
}

pub fn motor(value: u16) -> u8 {
    ((u32::from(value) * 100 + 32767) / 65535) as u8
}

pub fn rumble_payload(left: u16, right: u16) -> [u8; 9] {
    // Same layout as gip-pad's Gip::rumble, with independent main motors. gip-pad lists this
    // layout as not measured yet (its docs/measurements.md, section 6): the motor order, the
    // 10 ms on-time unit and so the 200 ms effect length are assumptions.
    [0, 0x0f, 0, 0, motor(left), motor(right), 20, 0, 0]
}

pub fn map(pad: Pad) -> Gamepad {
    Gamepad {
        buttons: BUTTON_MAP.iter().fold(0, |out, &(gip, xi)| {
            out | if pad.buttons & gip != 0 { xi } else { 0 }
        }),
        lt: trigger(pad.left_trigger),
        rt: trigger(pad.right_trigger),
        lx: pad.left_stick.0,
        ly: pad.left_stick.1,
        rx: pad.right_stick.0,
        ry: pad.right_stick.1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn every_button_has_its_own_documented_mask() {
        let expected = [
            0x10, 0x20, 0x1000, 0x2000, 0x4000, 0x8000, 1, 2, 4, 8, 0x100, 0x200, 0x40, 0x80,
        ];
        for (i, mask) in expected.into_iter().enumerate() {
            assert_eq!(
                map(Pad {
                    buttons: 1 << (i + 2),
                    ..Pad::default()
                })
                .buttons,
                mask
            );
        }
        assert_eq!(
            map(Pad {
                buttons: 0xffff,
                ..Pad::default()
            })
            .buttons,
            0xf3ff
        );
    }
    #[test]
    fn reserved_guide_and_share_are_not_public_xinput_buttons() {
        assert_eq!(
            map(Pad {
                buttons: 3,
                guide: true,
                share: true,
                ..Pad::default()
            }),
            Gamepad::default()
        );
    }
    #[test]
    fn axes_are_exact_and_have_no_deadzone_or_y_inversion() {
        for v in [i16::MIN, -1, 0, 1, i16::MAX] {
            let g = map(Pad {
                left_stick: (v, v),
                right_stick: (v, v),
                ..Pad::default()
            });
            assert_eq!((g.lx, g.ly, g.rx, g.ry), (v, v, v, v));
        }
    }
    #[test]
    fn analog_scaling_is_monotone_rounded_and_saturating() {
        assert_eq!(
            [
                trigger(0),
                trigger(1),
                trigger(511),
                trigger(512),
                trigger(1023),
                trigger(65535)
            ],
            [0, 0, 127, 128, 255, 255]
        );
        for v in 1..=1023 {
            assert!(trigger(v) >= trigger(v - 1));
        }
        assert_eq!([motor(0), motor(32768), motor(65535)], [0, 50, 100]);
    }

    #[test]
    fn rumble_motors_are_independent_and_time_limited() {
        assert_eq!(rumble_payload(65535, 0), [0, 15, 0, 0, 100, 0, 20, 0, 0]);
        assert_eq!(rumble_payload(0, 65535), [0, 15, 0, 0, 0, 100, 20, 0, 0]);
        assert_eq!(rumble_payload(0, 0), [0, 15, 0, 0, 0, 0, 20, 0, 0]);
    }
}
