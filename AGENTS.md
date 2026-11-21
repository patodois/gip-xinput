# Contributing to gip-xinput

gip-xinput bridges a wired GIP (Xbox One / Series protocol) controller on macOS to XInput 1.3 in a 64-bit Windows game running under Wine. A native Rust host (`src/`) reads the pad through [`gip-pad`](https://github.com/patodois/gip-pad) and answers on UDP `127.0.0.1:26785`; a small C DLL (`windows/`) is the game's `xinput1_3.dll` and asks the host for state. `scripts/install.py` installs the DLL into one game directory of a Wine prefix, reversibly. Read README.md and docs/protocol.md before changing the transport or the API behaviour.

This file is for anyone changing the code, human or AI agent.

## Clean room

- Implement XInput behaviour only from Microsoft's public XInput documentation (Microsoft Learn: the function reference, the documented structs and constants) and from `gip-pad`'s public API.
- Do not read, search or reproduce other XInput or GIP implementations: Wine's `dlls/xinput*`, x360ce, ScpToolkit, DS4Windows, ViGEm, and the Linux, SDL or other GIP drivers. Behaviour that the public documentation does not specify (for example the undocumented ordinals 100 and above) stays out, or comes from a measurement that is written down.
- When a constant or behaviour comes from a measurement rather than from documentation, record what was measured and how (see README.md, "Export ordinals").

## Build and check

```sh
make all                        # release host, DLL and probe (needs x86_64-w64-mingw32-gcc)
make test                       # Rust, C and Python tests; no hardware, no game, no real prefix
cargo fmt --check
cargo clippy --all-targets --locked -- -D warnings
make test-wine WINE=/path/to/wine   # optional: the probe under a given Wine build, in a fresh prefix under build/
```

- The native tests use UDP port 26795 (`TEST_PORT`), not the real 26785, so they run beside a real host. `GX_SKIP_TIMING=1` skips the wall-clock assertions on shared machines.
- `make test-wine` uses the real port 26785; stop a running host first.
- CI (`.github/workflows/ci.yml`) runs fmt, clippy and `cargo test` on macOS and Linux, and the native C and Python tests on macOS. It does not build the Windows DLL.

## Safety rules for changes and tests

- Running `gip-xinput-host` opens and resets the first GIP controller it finds. Tests must never construct a real `gip_pad::Controller` or `PadReader`; use the fake host (`gip-xinput-fake-host`) or a scripted socket.
- Tests must not touch a real Wine prefix or game directory. The installer tests use a fake registry and a temporary prefix.
- In the installer, a shell `rm` may only receive one absolute literal path, never a variable or a pattern. The generated `remove-installed-dll.sh` deletes exactly the installed DLL. Paths with spaces stay quoted.
- Tests stop only the child processes they started, by handle; never by name or pattern.

## Invariants

- The host binds `127.0.0.1` only and validates the full packet: length, magic, version, operation, reserved fields; the DLL also checks the correlation ID.
- The DLL reports `ERROR_DEVICE_NOT_CONNECTED` at once when the host says so or is absent (port refused). A reply that is merely late is held over with the last good state for at most `GX_HOLD_MS` (`windows/protocol.h`); never longer.
- Unchanged state keeps the same packet number, including while disabled. No dead zone is applied to `XINPUT_GAMEPAD`.
- Rumble: independent motors; `XInputEnable(FALSE)` stops it and `XInputEnable(TRUE)` replays it; the host clears a request once the process that made it has been silent for 500 ms (`src/rumble.rs`) and never replays it to another client.
- No I/O, thread or wait in `DllMain` or under the Windows loader lock; the DLL has no `DllMain`.
- USB I/O happens outside the host's state mutex.

## Dependencies and licence

- Keep dependencies to what is necessary, under MIT, Apache-2.0 or BSD terms, and list them with their licence texts in THIRD_PARTY.md and `licenses/dependencies/`.
- `gip-pad` comes from its public Git repository at a fixed commit until the version used is on crates.io.
- The project is licensed `MIT OR Apache-2.0`; keep both licence files, the SPDX header on every source file, and the AI-use note in README.md.
- Never commit build output, journals (`.install-state*/`), game files, registry exports or personal paths.
