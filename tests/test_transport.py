# SPDX-License-Identifier: MIT OR Apache-2.0
import os
from pathlib import Path
import socket
import resource
import statistics
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Wall-clock assertions are skipped on shared CI machines (GX_SKIP_TIMING=1); make test runs them.
timing = unittest.skipIf(os.environ.get("GX_SKIP_TIMING"), "GX_SKIP_TIMING is set")
# Must match the port build/api-test was compiled for (Makefile TEST_PORT); never the real 26785.
ADDRESS = ("127.0.0.1", int(os.environ.get("GX_TEST_PORT", "26795")))


def request(op=1, ident=1, left=0, right=0):
    return struct.pack("<4sBBHQHHI", b"GXIP", 1, op, 0, ident, left, right, 0)


class FakeHostCase(unittest.TestCase):
    fake_args = []
    def setUp(self):
        # Refuse to interfere with another process on the test port, even when it is idle.
        guard = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        guard.bind(ADDRESS)
        guard.close()
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(ROOT / "target/debug/gip-xinput-fake-host"), "--port", str(ADDRESS[1])] + self.fake_args,
                                        stdout=self.log, stderr=self.log)
        self.addCleanup(self.stop)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect(ADDRESS)
        self.socket.settimeout(0.1)
        self.addCleanup(self.socket.close)
        for _ in range(30):
            if self.process.poll() is not None:
                self.fail("fake host exited before readiness")
            try:
                self.exchange(request())
                break
            except (socket.timeout, ConnectionRefusedError):
                time.sleep(0.01)
        else:
            self.fail("fake host did not become ready")

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()  # this exact child only
            self.process.wait(timeout=5)
        self.log.close()

    def exchange(self, data):
        self.socket.send(data)
        return self.socket.recv(128)


class TransportTest(FakeHostCase):
    def test_golden_response_and_rumble_round_trip(self):
        reply = self.exchange(request(ident=0x0807060504030201))
        expected = struct.pack("<4sBBHQIIHBBhhhhI", b"GXIP", 1, 0x81, 0,
                               0x0807060504030201, 0, 42, 0x1201, 128, 255,
                               32767, -32768, 1, -1, 0)
        self.assertEqual(reply, expected)
        self.assertEqual(self.exchange(request(ident=2))[20:24], struct.pack("<I", 42))
        reply = self.exchange(request(2, 3, 0x1234, 0xabcd))
        self.assertEqual(reply[5], 0x82)
        self.assertEqual(struct.unpack_from("<IH", reply, 20), (43, 0x2000))
        self.log.seek(0)
        self.assertIn(b"RUMBLE 4660 43981 => 7 67", self.log.read())

    def test_malformed_and_oversized_datagrams_are_ignored(self):
        for data in [b"", b"GXIP", request() + b"x", request(op=3), request(left=1)]:
            self.socket.send(data)
            with self.assertRaises(socket.timeout):
                self.socket.recv(128)
        self.assertEqual(len(self.exchange(request(ident=99))), 40)

    @timing
    def test_latency_below_five_ms(self):
        samples = []
        for ident in range(1000):
            start = time.perf_counter_ns()
            reply = self.exchange(request(ident=ident))
            samples.append((time.perf_counter_ns() - start) / 1e6)
            self.assertEqual(struct.unpack_from("<Q", reply, 8)[0], ident)
        samples.sort()
        print(f"\nNative UDP median={statistics.median(samples):.3f}ms p99={samples[989]:.3f}ms max={samples[-1]:.3f}ms")
        self.assertLess(samples[989], 5)

    @timing
    def test_idle_host_blocks_without_consuming_cpu(self):
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        time.sleep(0.2)
        self.stop()
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        used = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
        self.assertLess(used, 0.03)
