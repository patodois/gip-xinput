# Native dependency licenses

The only direct Rust dependency is `gip-pad` 0.4.0, taken from its public Git repository at commit `4817b68` until that version is on crates.io. Its native `aarch64-apple-darwin` dependency tree is below. All offer an allowed MIT, Apache-2.0 or BSD license. Lockfile entries for other platforms are not linked into the native host.

| Crate | Version | Declared license |
| --- | --- | --- |
| gip-pad | 0.4.0 | MIT OR Apache-2.0 |
| anyhow | 1.0.104 | MIT OR Apache-2.0 |
| core-foundation | 0.10.1 | MIT OR Apache-2.0 |
| core-foundation-sys | 0.8.7 | MIT OR Apache-2.0 |
| io-kit-sys | 0.5.0 | MIT OR Apache-2.0 |
| mach2 | 0.5.0 | BSD-2-Clause OR MIT OR Apache-2.0 |
| libc | 0.2.191 | MIT OR Apache-2.0 |
| nusb | 0.2.9 | Apache-2.0 OR MIT |
| futures-core | 0.3.34 | MIT OR Apache-2.0 |
| log | 0.4.35 | MIT OR Apache-2.0 |
| slab | 0.4.12 | MIT |

Corresponding upstream MIT license notices are retained under `licenses/dependencies/` for binary distribution. Include those notices and this project's licenses when redistributing the host. The C DLL uses Windows system APIs, including Winsock and the C runtime; no external wrapper or extra runtime DLL is bundled. Rust, MinGW-w64, GCC and Apple build tools retain their own licenses and runtime terms.
