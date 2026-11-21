# SPDX-License-Identifier: MIT OR Apache-2.0
"""A late reply is not a disconnection: drives build/api-test --hold-over against a scripted host."""
import socket
import struct
import subprocess
import threading
import unittest

import test_transport as transport

REQUEST = struct.Struct("<4sBBHQHHI")
RESPONSE = struct.Struct("<4sBBHQIIHBBhhhhI")
BUTTON_A = 0x1000


class ScriptedHost:
    """Answers, ignores or refuses requests as the current step says."""

    def __init__(self):
        guard = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        guard.bind(transport.ADDRESS)
        guard.close()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(transport.ADDRESS)
        self.sock.settimeout(0.05)
        self.lock = threading.Lock()
        self.mode = "drop"
        self.log = []  # (step, operation, left, right) for every valid request
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def set_mode(self, mode):
        with self.lock:
            self.mode = mode
            if mode == "close":
                self.running = False
        if mode == "close":
            self.thread.join(timeout=5)
            self.sock.close()

    def serve(self):
        while True:
            with self.lock:
                if not self.running:
                    return
            try:
                data, peer = self.sock.recvfrom(64)
            except socket.timeout:
                continue
            if len(data) != REQUEST.size:
                continue
            magic, version, op, _, ident, left, right, _ = REQUEST.unpack(data)
            with self.lock:
                mode = self.mode
                self.log.append((mode, op, left, right))
            if mode == "drop":
                continue
            status, buttons = (0, BUTTON_A) if mode == "reply" else (1167, 0)
            self.sock.sendto(RESPONSE.pack(b"GXIP", 1, 0x80 | op, 0, ident, status, 42, buttons,
                                           0, 0, 0, 0, 0, 0, 0), peer)


class HoldOverTest(unittest.TestCase):
    def test_late_replies_hold_the_last_state_and_keystrokes(self):
        host = ScriptedHost()
        self.addCleanup(lambda: host.running and host.set_mode("close"))
        probe = subprocess.Popen([str(transport.ROOT / "build/api-test"), "--hold-over"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True)
        steps = []
        for line in probe.stdout:
            print(line, end="")
            if line.startswith("STEP "):
                mode = line.split()[1]
                steps.append(mode)
                host.set_mode(mode)
                probe.stdin.write("go\n")
                probe.stdin.flush()
        probe.stdin.close()
        stderr = probe.stderr.read()
        probe.wait(timeout=10)
        probe.stdout.close()
        probe.stderr.close()
        self.assertEqual(probe.returncode, 0, stderr)
        self.assertEqual(steps, ["reply", "drop", "reply", "drop", "reply", "unplugged", "reply", "unplugged",
                                 "drop", "reply", "close"])
        # The rumble that timed out during the first drop is the first request once replies resume.
        # It is sent three times in all: the timed-out attempt, the retry, and XInputEnable(TRUE).
        first_drop = next(i for i, entry in enumerate(host.log) if entry[0] == "drop")
        resumed = next(entry for entry in host.log[first_drop:] if entry[0] == "reply")
        self.assertEqual(resumed[1:], (2, 0x1111, 0x2222))
        self.assertEqual(sum(1 for entry in host.log if entry[1:] == (2, 0x1111, 0x2222)), 3)
        self.assertIn(("unplugged", 2, 0, 0), host.log)  # XInputEnable(FALSE) stopped the motors

if __name__ == "__main__":
    unittest.main()
