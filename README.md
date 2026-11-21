# gip-xinput

A local bridge from a wired GIP controller on macOS to **XInput 1.3 in a 64-bit Windows game running under Wine**. It is intended for the PDP Afterglow Wave (`0e6f:0338`), the original Fallout 4, and Highball's Wine 10 engine on Apple Silicon / Rosetta. Fallout 4 loads `XINPUT1_3.dll` and imports its functions by ordinal.

The native Rust host reads USB through [`gip-pad`](https://github.com/patodois/gip-pad) 0.4.0. A small MinGW C DLL supplies the game's XInput calls. There is one controller, at index 0. This does not create a macOS HID device or a system-wide Windows virtual controller. Headset audio is not bridged.

## Build

Prerequisites: Apple Silicon macOS, Rust with the native `aarch64-apple-darwin` target, Command Line Tools, Python 3.9+, `make`, and Homebrew's `x86_64-w64-mingw32-gcc`. Running Windows executables also requires Rosetta and an x86_64 Wine build (it was written for the Highball app's Wine 10 engine).

```sh
cd gip-xinput
make all
```

Outputs:

- `target/aarch64-apple-darwin/release/gip-xinput-host` — the real USB host; **running it opens/resets the controller**.
- `target/aarch64-apple-darwin/release/gip-xinput-fake-host` — hardware-free test fixture.
- `build/xinput1_3.dll` — PE32+ x86_64 DLL.
- `build/xinput-probe.exe` — PE32+ x86_64 diagnostic program.

The host's only direct dependency is `gip-pad`; the DLL links Windows system APIs. `Cargo.lock` fixes dependency versions. Native dependencies offer MIT, Apache-2.0 or BSD licenses; see [THIRD_PARTY.md](THIRD_PARTY.md). Build tools have their own licenses.

If your usual Cargo cache is read-only, point `CARGO_HOME` at a writable directory for the build.

## Installation and removal

Review `scripts/install.py`, `scripts/install.sh`, and `scripts/uninstall.sh` first. Close the game before installing or removing. Building and the normal tests do not change any game or Wine prefix.

The installer takes three required arguments:

- `--wine`: the `wine` executable of the Wine build that normally runs the prefix. The registry is edited with it, so use the same build the game runs with; the installer cannot check that.
- `--prefix`: the Wine prefix (bottle) the game runs in. It must contain `drive_c` and `user.reg`. The DLL override is written into this prefix's registry.
- `--game-dir`: the directory that holds the game's executable. It must be inside the prefix's `drive_c`, so the DLL and the override always land in the same prefix.

`--exe NAME` optionally refuses to install unless the game directory holds `NAME`. `--state` chooses the journal directory (default `.install-state` in the repository); `--dll` the DLL to install (default `build/xinput1_3.dll`).

Example, for the setup this bridge was written for: Fallout 4 from Steam in the Highball app's `Games` bottle, with its `x64-sikarugir10.0_6-r28` engine. Substitute your own Wine build, prefix and game:

```sh
HB="$HOME/Library/Application Support/Highball"
./scripts/install.sh \
  --wine "$HB/engines/x64-sikarugir10.0_6-r28/engine/bin/wine" \
  --prefix "$HB/bottles/Games" \
  --game-dir "$HB/bottles/Games/drive_c/Program Files (x86)/Steam/steamapps/common/Fallout 4" \
  --exe Fallout4.exe \
  --state "$PWD/.install-state"
```

In effect the installer copies the DLL into the game directory and runs, with `WINEPREFIX` set to `--prefix`:

```sh
wine reg add 'HKCU\Software\Wine\DllOverrides' /v xinput1_3 /t REG_SZ /d native,builtin /f
```

That command is shown for review; **use the installer**, which first saves any existing DLL, its metadata, the registry key's existence, and the exact override value, including its type/data. It also preserves an existing DLL's filename casing. It refuses symlinks, ambiguous case variants and reused journal directories. If saving that state fails before anything outside the journal has changed, it removes the files it wrote and the journal directory, so the same `--state` can be used again. An installation failure after that attempts restoration from the journal.

Remove using the same journal; the Wine build, prefix and game directory are read from it:

```sh
./scripts/uninstall.sh --state "$PWD/.install-state"
```

Removal restores the prior DLL and the prior override, including absence. If the override key was originally absent, it removes that key only if it is now empty. Other values/subkeys are preserved. It refuses to overwrite a DLL or override changed by someone else after installation. Backups and the journal remain for inspection; use a new `--state` directory for another installation after removal.

When the DLL was absent before installation, the installer generates `remove-installed-dll.sh` with **one absolute literal DLL path** in its `rm` command. No variable, glob, recursive removal or game-directory deletion is used. The uninstaller validates that command against the journal before running it. When a DLL existed before installation, it restores the backup without `rm`.

The registry override applies to every program in the prefix: another program there that ships its own `xinput1_3.dll` next to its executable will load that copy instead of Wine's. Only the given game directory receives this DLL. Avoid running installation/removal alongside other registry edits.

## Play

When the controller is free, start the real host in a terminal:

```sh
./target/aarch64-apple-darwin/release/gip-xinput-host
```

Then launch the game as usual. Keep the host running; Ctrl-C stops it. `--help` is safe and does not open USB. Only run one host, and release the controller from any other USB reader first. The host follows `gip-pad`'s first available GIP-controller selection.

`gip-pad`'s `PadReader` exposes `spawn()` and `latest()`, but has no rumble/output method. The host therefore uses the crate's public `Controller::open()`, `poll()`, `state()` and `Gip::send()` in a single USB thread. It uses the crate's own rumble payload layout with independent main motors. It retries failed connections with sleeps of 1, 2, 4, then 8 seconds. Each opening uses `Controller::open()`'s USB reset/startup behavior; unlike `PadReader`, this worker does not retain the crate's private per-connection reset budget across failed openings. If the pad announces itself again in the middle of a session, `gip-pad` 0.4.0's `poll()` sends the start packets again and reports every input as released; the host publishes that released state (so the state sequence changes) and sends any active rumble again.

## Transport and latency

Only UDP **IPv4 loopback `127.0.0.1:26785`** is used. The DLL binds an ephemeral loopback socket for each request, connects it to the host, and asks for the latest state or queues rumble. The host blocks in `recv_from` while idle; its USB thread blocks in `poll` for up to 2 ms so output can also be serviced promptly. There is no busy wait or DLL background thread. A process lock serializes XInput calls; sockets close before each call returns, making DLL unload straightforward.

The DLL waits with `select`, using a **2 ms response budget**. A host that reports the controller unplugged, or a host that is absent (the closed port is refused), yields `ERROR_DEVICE_NOT_CONNECTED` (1167) at once. A reply that does not arrive in time (or only malformed ones do) is not treated as an unplug: for up to **100 ms** after the last good reply (`GX_HOLD_MS` in `windows/protocol.h`, where the reasoning is written down) the DLL reports that last state again, with the same packet number, and keeps its keystroke queue and held keys. Only after 100 ms without any good reply does it report 1167. A rumble or `XInputEnable` request that timed out is sent again with the next query. `XInputEnable` has no return value. Nonzero controller indices are disconnected; `XUSER_INDEX_ANY` is supported for keystrokes.

The target is less than 5 ms bridge overhead under normal scheduling. Native loopback and native API tests assert p99 below 5 ms over 1,000 samples and print median/p99/max. The Wine test repeats the API measurement through Wine. macOS/Wine scheduling, first-use system initialization and USB transfer time are not hard real-time guarantees; hardware and Wine latency have not been measured. A 4 ms USB report interval is additional to transport latency.

Each request carries a 64-bit correlation ID. The response echoes it and carries a 32-bit state sequence. The USB worker increments that sequence only when the mapped state or connection changes. The DLL derives `dwPacketNumber` from this sequence and also counts effective changes caused by `XInputEnable`; unchanged public state while disabled keeps the packet number stable. Counter wrap is intentional. See [docs/protocol.md](docs/protocol.md) for the complete wire format.

Rumble is queued without waiting for USB completion, preserving input-response latency. Main motor speeds are independently rounded from `0..65535` to `0..100`. The rumble payload reuses `gip-pad`'s layout, which is not measured yet ([gip-pad measurements, section 6](https://github.com/patodois/gip-pad/blob/main/docs/measurements.md)): that the left byte drives the low-frequency motor and the right byte the high-frequency one, that the on-time unit is 10 ms, and that the pad stops by itself after the on-time are assumptions. Nonzero effects are sent with a (assumed) 200 ms on-time and refreshed every 100 ms while the process that asked for them keeps polling. When that process has sent nothing for 500 ms (it exited, crashed or stopped polling), the host clears the request and stops the motors; another process polling does not keep it alive, and the cleared request is not replayed to the next game. A game that stops polling for more than 500 ms therefore loses its rumble until it sets it again. Repeating the same motor values does not cause extra USB writes. If the on-time assumption holds, a host crash cannot leave an indefinitely repeating effect. USB I/O errors are reported by the host and make subsequent queries disconnected. If the USB thread itself stops (a panic), the host exits instead of serving the last input forever. Transient socket receive errors are logged and the host keeps serving.

Loopback restricts access to this machine; it is not authentication against other local processes. The port is fixed and must be free. There is no network discovery or external interface binding.

## Public API coverage

The implementation follows Microsoft's [XInput reference](https://learn.microsoft.com/en-us/windows/win32/api/xinput/) and [XInput versions](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-versions). All seven documented 1.3 functions are exported, by name and at ordinals 2 to 8 (see [Export ordinals](#export-ordinals)):

| Function | Behavior |
| --- | --- |
| [XInputGetState](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetstate) | Current input, state sequence, neutral input while disabled. |
| [XInputSetState](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputsetstate) | Independent low/high frequency main motors; remembers requests while disabled. |
| [XInputGetCapabilities](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetcapabilities) | Wired gamepad, 14 documented buttons, analog controls and rumble. No audio/wireless flags; 1.3 capability flags are zero. |
| [XInputEnable](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputenable) | Disabling stops rumble and reports neutral input; enabling replays the last requested rumble. Per-process state. |
| [XInputGetDSoundAudioDeviceGuids](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetdsoundaudiodeviceguids) | Success and two null GUIDs when connected: there is no bridged headset. |
| [XInputGetBatteryInformation](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetbatteryinformation) | Wired gamepad with level `BATTERY_LEVEL_FULL`, so callers that ignore the type see no empty battery; disconnected headset. Microsoft documents the level as meaningful only for wireless devices. |
| [XInputGetKeystroke](https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetkeystroke) | Queued key-down, key-up and repeat events; `ERROR_EMPTY` when no event is pending. |

Keystrokes are sampled on successful input/capability/battery/audio/keystroke calls. The bounded 128-event queue drops its oldest event on overflow; transitions between calls can be missed. Digital stick navigation uses an axis threshold of 16,384 with eight directions; trigger keys use the public threshold 30. Repeat starts after 500 ms and then every 50 ms. These are this bridge's navigation choices; **raw `XINPUT_GAMEPAD` has no deadzone or trigger threshold**. Disconnection and focus changes clear old queued events; a held-over late reply (see above) keeps them.

The [public `XINPUT_GAMEPAD` contract](https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad) defines no Guide or Share button. Those inputs are omitted. Menu maps to Start, View to Back, LS/RS to thumb clicks, and all other standard buttons map by name. Sticks are copied exactly, with up positive. Triggers are rounded from `0..1023` to `0..255`.

`XInputGetAudioDeviceIds` belongs to XInput 1.4 and is not exported here.

Only the seven documented functions are exported. Nothing is exported at ordinal 100 or above: the consulted public Microsoft reference does not document those ordinals or their contracts, so none is invented or taken from another implementation. The Guide button is therefore never reported, and a program that requires one of those ordinals cannot use this DLL. Only a 64-bit (x86_64) DLL is built, and the installer refuses anything else; 32-bit games are not supported.

### Export ordinals

`windows/xinput1_3.def` fixes the ordinals: `XInputGetState` 2, `XInputSetState` 3, `XInputGetCapabilities` 4, `XInputEnable` 5, `XInputGetDSoundAudioDeviceGuids` 6, `XInputGetBatteryInformation` 7, `XInputGetKeystroke` 8. `tests/test_pe.py` checks them in the built DLL.

- Ordinals 2, 3 and 4: Fallout 4 imports `xinput1_3.dll` by ordinal 2, 3 and 4. This is recorded in the message of commit `0f49ca0`: with the linker-assigned ordinals the game's calls reached the wrong functions and it closed. That record is the project's own observation; the game's import table is not reproduced here.
- Ordinals 5 to 8: where these came from is unrecorded. Microsoft's public XInput documentation does not list ordinals, and no measurement in this repository shows them. What would establish them: the export table of a Windows `xinput1_3.dll` (for example `x86_64-w64-mingw32-objdump -p xinput1_3.dll`), recorded here with that file's version and SHA-256, or the import table of a program that imports these functions by ordinal.

## Tests without hardware or games

```sh
make test
make test-wine WINE=/path/to/wine
```

`make test` checks Rust button/axis/trigger/rumble mapping, the rumble rules and protocol fixtures, C protocol decoding, the actual C API logic over real native loopback UDP with a test-only POSIX OS adapter, PE architecture/exports and ordinals, and installation rollback/restoration against a fake registry and a temporary prefix. Native C tests run AddressSanitizer and UndefinedBehaviorSanitizer. The fake-host tests cover malformed packets, independent rumble, absent host, stable/changing packet numbers, enable/disable, keystrokes/repeats, loopback latency and idle CPU. `tests/test_holdover.py` drives the C API against a scripted host that answers, stops answering, reports the pad unplugged or closes its port, and checks the hold-over, the keystroke state and the retried rumble. No test opens a real `Controller` or `PadReader`.

The native tests use UDP port 26795 (`make test TEST_PORT=...` to change it), not the real host's 26785, so they can run while a real host is running. `GX_SKIP_TIMING=1` skips the wall-clock latency and idle-CPU assertions, for shared CI machines; `make test` runs them. `test_pe.py` needs `x86_64-w64-mingw32-objdump`.

`make test-wine WINE=...` uses the given Wine build, creates a fresh `build/wine-test-*/prefix`, and runs only `xinput-probe.exe` plus the fake host. It does not touch any other prefix, install the DLL into a game, or start a game. It verifies no-host and disconnected-host responses, all seven APIs, rumble replay, latency, and host disappearance/restart in a still-running probe. The probe loads the adjacent DLL explicitly and prints its loaded path. Logs and the disposable prefix remain under `build/`. It uses the real port 26785, so stop any real host first, and do not run it alongside another Wine test.

What has been run: this repository records no complete `make test-wine` run and no test with a physical controller. The only recorded run of the DLL in a game is the one described under [Export ordinals](#export-ordinals), which predates the hold-over, rumble and battery changes. The native API test validates the same C logic, but does not establish Wine compatibility or physical controller behavior.

For a manual state display in the test prefix printed by that script (substitute its absolute path and your Wine build):

```sh
WINEPREFIX="/absolute/path/to/build/wine-test-XXXX/prefix" \
WINEDLLOVERRIDES="xinput1_3=n" \
  /path/to/wine build/xinput-probe.exe 300
```

The probe supports `--self-test` with the fake fixture, `--disconnected`, `--latency`, `--hold-over` (driven by `tests/test_holdover.py`), or a sample count (16 ms between printed samples). The fake fixture switches its buttons to B on rumble `(0x1234, 0xabcd)` so the self-test can exercise real state transitions.

## Sources, AI and license

Implementation sources were Microsoft's public XInput documentation linked above and the public `gip-pad` source. No Wine XInput implementation, x360ce, ScpToolkit, DS4Windows, ViGEm or other wrapper implementation was used. The dependency retains its own provenance and licenses. This project is not affiliated with Microsoft, PDP, Bethesda, Wine or Highball.

This implementation, tests and documentation were written with AI assistance (OpenAI Codex and Anthropic Claude). Running the host against a controller and validating it in a game are up to the user.

Licensed under **MIT OR Apache-2.0**, at your option. See [LICENSE-MIT](LICENSE-MIT) and [LICENSE-APACHE](LICENSE-APACHE).
