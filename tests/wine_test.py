#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Hardware-free integration: the probe and DLL under a given Wine build, in a fresh isolated prefix."""
import argparse
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine", type=Path, required=True, help="the wine executable to test with")
    args = parser.parse_args()
    wine = args.wine.resolve(strict=True)
    fake_binary = ROOT / "target/aarch64-apple-darwin/release/gip-xinput-fake-host"
    probe = ROOT / "build/xinput-probe.exe"
    for file in (fake_binary, probe, ROOT / "build/xinput1_3.dll"):
        if not file.is_file():
            raise RuntimeError(f"missing {file}; run make all")
    guard = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    guard.bind(("127.0.0.1", 26785))
    guard.close()
    scratch = Path(tempfile.mkdtemp(prefix="wine-test-", dir=ROOT / "build"))
    env = os.environ.copy()
    env.update(WINEPREFIX=str(scratch / "prefix"), WINEARCH="win64", WINEDEBUG="-all",
               WINESERVER=str(wine.parent / "wineserver"),
               WINEDLLOVERRIDES="xinput1_3=n;winemenubuilder.exe=d;mscoree=d;mshtml=d")
    env["PATH"] = str(wine.parent) + os.pathsep + env.get("PATH", "")
    print("Wine:", wine, "\nIsolated prefix:", env["WINEPREFIX"], flush=True)
    children = []

    def start_fake(disconnected=False):
        log_path = scratch / f"fake-{len(children)}.log"
        log = log_path.open("w+")
        process = subprocess.Popen([str(fake_binary)] + (["--disconnected"] if disconnected else []), stdout=log, stderr=log)
        children.append((process, log))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("fake host exited: " + log_path.read_text())
            if "FAKE READY" in log_path.read_text():
                return process, log_path
            time.sleep(0.02)
        raise RuntimeError("fake host readiness timed out")

    def stop(process):
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=5)

    def run_probe(mode, marker):
        result = subprocess.run([str(wine), str(probe), mode], cwd=probe.parent,
                                env=env, capture_output=True, text=True, errors="replace", timeout=60)
        output = result.stdout + result.stderr
        (scratch / (mode.lstrip("-") + ".log")).write_text(output)
        print(output, end="", flush=True)
        if result.returncode or marker not in output:
            raise RuntimeError(f"probe {mode} failed (exit {result.returncode}); logs: {scratch}")

    try:
        run_probe("--disconnected", "DISCONNECTED PASS")
        fake, log_path = start_fake()
        run_probe("--self-test", "SELFTEST PASS")
        log = log_path.read_text()
        # Disabled SetState is recorded locally; the request appears exactly once on enable.
        if log.count("RUMBLE 10000 20000") != 1 or "RUMBLE 4660 43981" not in log or "RUMBLE 0 0" not in log:
            raise RuntimeError("rumble / enable replay mismatch: " + log)
        stop(fake)
        fake, _ = start_fake(True)
        run_probe("--disconnected", "DISCONNECTED PASS")
        stop(fake)
        fake, _ = start_fake()
        reconnect = subprocess.Popen([str(wine), str(probe), "--reconnect"], cwd=probe.parent, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        children.append((reconnect, None))
        # Wait for markers using the OS pipe; no polling or sleeps while awaiting the probe.
        deadline = time.monotonic() + 20
        lines = []
        while time.monotonic() < deadline:
            ready, _, _ = select.select([reconnect.stdout], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                raise RuntimeError("reconnect probe timed out")
            line = reconnect.stdout.readline().decode("utf-8", errors="replace")
            if not line:
                break
            lines.append(line)
            print(line, end="", flush=True)
            if "RECONNECT READY" in line:
                stop(fake)
            if "RECONNECT ABSENT" in line:
                fake, _ = start_fake()
            if "RECONNECT PASS" in line:
                break
        reconnect.wait(timeout=5)
        (scratch / "reconnect.log").write_text("".join(lines))
        if reconnect.returncode or not any("RECONNECT PASS" in s for s in lines):
            raise RuntimeError("reconnect failed")
        print("WINE INTEGRATION PASS; logs retained at", scratch)
    finally:
        for process, log in children:
            stop(process)
            if log:
                log.close()
            if process.stdout:
                process.stdout.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
