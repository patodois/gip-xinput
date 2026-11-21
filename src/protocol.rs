// SPDX-License-Identifier: MIT OR Apache-2.0
use crate::mapping::Gamepad;

pub const ADDRESS: &str = "127.0.0.1:26785";
pub const REQUEST_SIZE: usize = 24;
pub const RESPONSE_SIZE: usize = 40;
pub const DISCONNECTED: u32 = 1167;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Operation {
    State = 1,
    Rumble = 2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Request {
    pub operation: Operation,
    pub id: u64,
    pub left: u16,
    pub right: u16,
}

impl Request {
    pub fn decode(b: &[u8]) -> Option<Self> {
        if b.len() != REQUEST_SIZE
            || &b[..4] != b"GXIP"
            || b[4] != 1
            || b[6..8] != [0; 2]
            || b[20..24] != [0; 4]
        {
            return None;
        }
        let operation = match b[5] {
            1 => Operation::State,
            2 => Operation::Rumble,
            _ => return None,
        };
        let left = u16::from_le_bytes(b[16..18].try_into().ok()?);
        let right = u16::from_le_bytes(b[18..20].try_into().ok()?);
        if operation == Operation::State && (left != 0 || right != 0) {
            return None;
        }
        Some(Self {
            operation,
            id: u64::from_le_bytes(b[8..16].try_into().ok()?),
            left,
            right,
        })
    }

    /// The calling process, from the upper half of the request ID (see docs/protocol.md).
    pub fn client(self) -> u32 {
        (self.id >> 32) as u32
    }

    pub fn encode(self) -> [u8; REQUEST_SIZE] {
        let mut b = [0; REQUEST_SIZE];
        b[..4].copy_from_slice(b"GXIP");
        b[4] = 1;
        b[5] = self.operation as u8;
        b[8..16].copy_from_slice(&self.id.to_le_bytes());
        b[16..18].copy_from_slice(&self.left.to_le_bytes());
        b[18..20].copy_from_slice(&self.right.to_le_bytes());
        b
    }
}

pub fn response(request: Request, status: u32, sequence: u32, pad: Gamepad) -> [u8; RESPONSE_SIZE] {
    let mut b = [0; RESPONSE_SIZE];
    b[..4].copy_from_slice(b"GXIP");
    b[4] = 1;
    b[5] = 0x80 | request.operation as u8;
    b[8..16].copy_from_slice(&request.id.to_le_bytes());
    b[16..20].copy_from_slice(&status.to_le_bytes());
    b[20..24].copy_from_slice(&sequence.to_le_bytes());
    let pad = if status == 0 { pad } else { Gamepad::default() };
    b[24..26].copy_from_slice(&pad.buttons.to_le_bytes());
    b[26] = pad.lt;
    b[27] = pad.rt;
    for (i, v) in [pad.lx, pad.ly, pad.rx, pad.ry].into_iter().enumerate() {
        b[28 + i * 2..30 + i * 2].copy_from_slice(&v.to_le_bytes());
    }
    b
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn request_golden_bytes_and_roundtrip() {
        let q = Request {
            operation: Operation::Rumble,
            id: 0x0807060504030201,
            left: 0x1234,
            right: 0xabcd,
        };
        let bytes = *b"GXIP\x01\x02\0\0\x01\x02\x03\x04\x05\x06\x07\x08\x34\x12\xcd\xab\0\0\0\0";
        assert_eq!(q.encode(), bytes);
        assert_eq!(Request::decode(&bytes), Some(q));
        assert_eq!(q.client(), 0x08070605);
    }
    #[test]
    fn rejects_bad_lengths_magic_version_opcode_reserved_and_state_motors() {
        let q = Request {
            operation: Operation::State,
            id: 1,
            left: 0,
            right: 0,
        }
        .encode();
        for length in 0..q.len() {
            assert!(Request::decode(&q[..length]).is_none());
        }
        let mut long = q.to_vec();
        long.push(0);
        assert!(Request::decode(&long).is_none());
        for i in [0, 4, 5, 6, 7, 16, 18, 20, 21, 22, 23] {
            let mut bad = q;
            bad[i] = 255;
            assert!(Request::decode(&bad).is_none(), "byte {i}");
        }
    }
    #[test]
    fn response_golden_bytes_and_disconnection_clear_input() {
        let q = Request {
            operation: Operation::State,
            id: 1,
            left: 0,
            right: 0,
        };
        let p = Gamepad {
            buttons: 0x1201,
            lt: 128,
            rt: 255,
            lx: 32767,
            ly: -32768,
            rx: 1,
            ry: -1,
        };
        let b = response(q, 0, 42, p);
        assert_eq!(
            &b[16..],
            &[
                0, 0, 0, 0, 42, 0, 0, 0, 1, 18, 128, 255, 255, 127, 0, 128, 1, 0, 255, 255, 0, 0,
                0, 0
            ]
        );
        assert_eq!(&response(q, DISCONNECTED, 43, p)[24..], &[0; 16]);
    }
}
