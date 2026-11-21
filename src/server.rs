// SPDX-License-Identifier: MIT OR Apache-2.0
use crate::{
    mapping::Gamepad,
    protocol::{self, Operation, Request},
};
use std::io;
use std::net::UdpSocket;

pub trait Source {
    fn snapshot(&mut self) -> (u32, Option<Gamepad>);
    /// Called for every valid request, before it is answered. `client` identifies the calling
    /// process (`Request::client`).
    fn seen(&mut self, client: u32);
    /// Queue an output request without waiting for USB completion.
    fn rumble(&mut self, client: u32, left: u16, right: u16);
}

/// Receive errors that say nothing about the host's own socket: the loop goes on after them.
pub fn transient(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::Interrupted
            | io::ErrorKind::WouldBlock
            | io::ErrorKind::TimedOut
            | io::ErrorKind::ConnectionReset
            | io::ErrorKind::ConnectionRefused
    )
}

pub fn serve(socket: UdpSocket, source: &mut impl Source) -> io::Result<()> {
    let mut buffer = [0; protocol::REQUEST_SIZE + 1];
    loop {
        let (len, peer) = match socket.recv_from(&mut buffer) {
            Ok(received) => received,
            Err(e) if transient(&e) => {
                eprintln!("UDP receive: {e}; continuing");
                continue;
            }
            Err(e) => return Err(e),
        };
        if !peer.ip().is_loopback() {
            continue;
        }
        let Some(q) = Request::decode(&buffer[..len]) else {
            continue;
        };
        source.seen(q.client());
        let (_, pad) = source.snapshot();
        if pad.is_some() && q.operation == Operation::Rumble {
            source.rumble(q.client(), q.left, q.right);
        }
        let (sequence, pad) = source.snapshot();
        let reply = protocol::response(
            q,
            if pad.is_some() {
                0
            } else {
                protocol::DISCONNECTED
            },
            sequence,
            pad.unwrap_or_default(),
        );
        if let Err(e) = socket.send_to(&reply, peer) {
            eprintln!("UDP reply: {e}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::time::Duration;

    #[test]
    fn only_socket_independent_receive_errors_are_survived() {
        for kind in [
            io::ErrorKind::Interrupted,
            io::ErrorKind::WouldBlock,
            io::ErrorKind::TimedOut,
            io::ErrorKind::ConnectionReset,
            io::ErrorKind::ConnectionRefused,
        ] {
            assert!(transient(&io::Error::from(kind)), "{kind:?}");
        }
        for kind in [
            io::ErrorKind::InvalidInput,
            io::ErrorKind::PermissionDenied,
            io::ErrorKind::Other,
        ] {
            assert!(!transient(&io::Error::from(kind)), "{kind:?}");
        }
    }

    #[derive(Default)]
    struct Recorder {
        seen: Vec<u32>,
        rumble: Vec<(u32, u16, u16)>,
    }
    struct Shared(Arc<Mutex<Recorder>>);
    impl Source for Shared {
        fn snapshot(&mut self) -> (u32, Option<Gamepad>) {
            (7, Some(Gamepad::default()))
        }
        fn seen(&mut self, client: u32) {
            self.0.lock().unwrap().seen.push(client);
        }
        fn rumble(&mut self, client: u32, left: u16, right: u16) {
            self.0.lock().unwrap().rumble.push((client, left, right));
        }
    }

    #[test]
    fn every_request_reports_its_client_and_rumble_carries_it() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap();
        let record = Arc::new(Mutex::new(Recorder::default()));
        let mut source = Shared(record.clone());
        // The server loop never returns; the thread ends with the test process.
        std::thread::spawn(move || serve(socket, &mut source));
        let client = UdpSocket::bind("127.0.0.1:0").unwrap();
        client.connect(address).unwrap();
        client
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut reply = [0; 64];
        for q in [
            Request {
                operation: Operation::State,
                id: 0x0000_00aa_0000_0001,
                left: 0,
                right: 0,
            },
            Request {
                operation: Operation::Rumble,
                id: 0x0000_00bb_0000_0002,
                left: 3,
                right: 4,
            },
        ] {
            client.send(&q.encode()).unwrap();
            let n = client.recv(&mut reply).unwrap();
            assert_eq!(n, protocol::RESPONSE_SIZE);
            assert_eq!(reply[8..16], q.id.to_le_bytes());
        }
        let r = record.lock().unwrap();
        assert_eq!(r.seen, [0xaa, 0xbb]);
        assert_eq!(r.rumble, [(0xbb, 3, 4)]);
    }
}
