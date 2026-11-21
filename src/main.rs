// SPDX-License-Identifier: MIT OR Apache-2.0
use gip_pad::{CMD_RUMBLE, Controller, Header};
use gip_xinput_host::{
    mapping::{self, Gamepad},
    protocol,
    rumble::{Sender, Wanted},
    server::{self, Source},
};
use std::{
    any::Any,
    net::UdpSocket,
    panic::{self, AssertUnwindSafe},
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

struct Shared {
    sequence: u32,
    pad: Option<Gamepad>,
    output: Wanted,
}

impl Shared {
    fn publish(&mut self, next: Option<Gamepad>) {
        if self.pad != next {
            self.sequence = self.sequence.wrapping_add(1);
            self.pad = next;
        }
    }
}

struct Live(Arc<Mutex<Shared>>);
impl Source for Live {
    fn snapshot(&mut self) -> (u32, Option<Gamepad>) {
        let s = self.0.lock().unwrap();
        (s.sequence, s.pad)
    }
    fn seen(&mut self, client: u32) {
        self.0.lock().unwrap().output.seen(client, Instant::now());
    }
    fn rumble(&mut self, client: u32, left: u16, right: u16) {
        self.0
            .lock()
            .unwrap()
            .output
            .set(client, (left, right), Instant::now());
    }
}

fn usb_worker(shared: Arc<Mutex<Shared>>) {
    let mut backoff = 1;
    loop {
        match Controller::open() {
            Ok(mut controller) => {
                eprintln!(
                    "Connected: {} ({:04x}:{:04x})",
                    controller.name, controller.vendor, controller.product
                );
                backoff = 1;
                {
                    let mut s = shared.lock().unwrap();
                    s.output.clear();
                    s.publish(Some(mapping::map(controller.state())));
                }
                let mut sender = Sender::new(Instant::now());
                loop {
                    let now = Instant::now();
                    let desired = shared.lock().unwrap().output.desired(now);
                    if sender.due(desired, now) {
                        // PadReader has no output API. Gip::send permits independent main motors;
                        // this is the payload used by gip-pad's own public Gip::rumble helper.
                        let payload = mapping::rumble_payload(desired.0, desired.1);
                        if let Err(e) = controller.gip.send(CMD_RUMBLE, 0, &payload) {
                            eprintln!("Rumble failed: {e:#}");
                            break;
                        }
                        sender.sent(desired, now);
                    }
                    match controller.poll(Duration::from_millis(2)) {
                        Ok(Some(packet)) => {
                            if Header::parse(&packet).is_some_and(|h| h.is_announce(&packet)) {
                                // gip-pad 0.4 treats a mid-session Hello as a restart: it has
                                // already sent the start packets again and released every input.
                                // The pad's motors stopped with it, so resend any wanted rumble.
                                eprintln!("Controller announced again; restarted it");
                                sender.restarted();
                            }
                            shared
                                .lock()
                                .unwrap()
                                .publish(Some(mapping::map(controller.state())));
                        }
                        Ok(None) => {}
                        Err(e) => {
                            eprintln!("USB read failed: {e:#}");
                            break;
                        }
                    }
                }
            }
            Err(e) => eprintln!("Controller unavailable: {e:#}; retry in {backoff}s"),
        }
        shared.lock().unwrap().publish(None);
        thread::sleep(Duration::from_secs(backoff));
        backoff = (backoff * 2).min(8);
    }
}

/// Runs `work` and returns its panic message if it panicked.
fn supervise(work: impl FnOnce()) -> Result<(), String> {
    panic::catch_unwind(AssertUnwindSafe(work)).map_err(|payload| panic_message(&*payload))
}

fn panic_message(payload: &(dyn Any + Send)) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic".to_string()
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args == ["--help"] {
        println!(
            "gip-xinput-host\nReads a GIP controller via gip-pad; serves only {}.\nRun only when the controller is free. Ctrl-C stops the host.",
            protocol::ADDRESS
        );
        return Ok(());
    }
    if !args.is_empty() {
        return Err("unknown arguments (use --help)".into());
    }
    let socket = UdpSocket::bind(protocol::ADDRESS)?; // bind before touching USB
    let shared = Arc::new(Mutex::new(Shared {
        sequence: SystemTime::now().duration_since(UNIX_EPOCH)?.subsec_nanos(),
        pad: None,
        output: Wanted::new(Instant::now()),
    }));
    let worker_state = shared.clone();
    thread::Builder::new()
        .name("gip-usb".into())
        .spawn(move || {
            // The worker only returns by panicking. Serving the last state after that would hold
            // the game's input forever, so the whole host stops and clients see it as absent.
            let reason = supervise(|| usb_worker(worker_state))
                .err()
                .unwrap_or_else(|| "returned".to_string());
            eprintln!("USB worker stopped ({reason}); exiting");
            std::process::exit(1);
        })?;
    eprintln!("Listening on {} (Ctrl-C to stop)", protocol::ADDRESS);
    server::serve(socket, &mut Live(shared))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sequence_changes_only_with_published_input_and_wraps() {
        let mut s = Shared {
            sequence: u32::MAX,
            pad: None,
            output: Wanted::new(Instant::now()),
        };
        s.publish(None);
        assert_eq!(s.sequence, u32::MAX);
        s.publish(Some(Gamepad::default()));
        assert_eq!(s.sequence, 0);
        s.publish(Some(Gamepad::default()));
        assert_eq!(s.sequence, 0);
        s.publish(None);
        assert_eq!(s.sequence, 1);
    }

    #[test]
    fn a_panicking_worker_is_reported_not_ignored() {
        assert_eq!(supervise(|| {}), Ok(()));
        assert_eq!(
            supervise(|| panic!("worker {}", 7)),
            Err("worker 7".to_string())
        );
        assert_eq!(supervise(|| panic!("static")), Err("static".to_string()));
    }
}
