// SPDX-License-Identifier: MIT OR Apache-2.0
use gip_pad::Pad;
use gip_xinput_host::{
    mapping::{self, Gamepad},
    protocol,
    server::{self, Source},
};
use std::{
    io::{self, Write},
    net::UdpSocket,
};

struct Fake {
    disconnected: bool,
    sequence: u32,
    alternate: bool,
}
impl Source for Fake {
    fn snapshot(&mut self) -> (u32, Option<Gamepad>) {
        let pad = Pad {
            buttons: if self.alternate {
                1 << 5
            } else {
                (1 << 4) | (1 << 8) | (1 << 13)
            },
            left_trigger: 512,
            right_trigger: 1023,
            left_stick: (32767, -32768),
            right_stick: (1, -1),
            ..Pad::default()
        };
        (
            self.sequence,
            (!self.disconnected).then(|| mapping::map(pad)),
        )
    }
    fn seen(&mut self, _client: u32) {}
    fn rumble(&mut self, _client: u32, left: u16, right: u16) {
        println!(
            "RUMBLE {left} {right} => {} {}",
            mapping::motor(left),
            mapping::motor(right)
        );
        io::stdout().flush().unwrap();
        if (left, right) == (0x1234, 0xabcd) && !self.alternate {
            self.alternate = true;
            self.sequence += 1;
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    const USAGE: &str = "usage: gip-xinput-fake-host [--disconnected] [--port N]";
    let mut disconnected = false;
    let mut address = protocol::ADDRESS.to_string();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--disconnected" => disconnected = true,
            // Lets tests run while a real host holds the fixed port.
            "--port" => {
                let port: u16 = args.next().ok_or(USAGE)?.parse()?;
                address = format!("127.0.0.1:{port}");
            }
            _ => return Err(USAGE.into()),
        }
    }
    let socket = UdpSocket::bind(&address)?;
    println!("FAKE READY {address}");
    io::stdout().flush()?;
    server::serve(
        socket,
        &mut Fake {
            disconnected,
            sequence: 42,
            alternate: false,
        },
    )?;
    Ok(())
}
