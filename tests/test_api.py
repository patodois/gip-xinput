# SPDX-License-Identifier: MIT OR Apache-2.0
import subprocess
import test_transport as transport


class ApiTest(transport.FakeHostCase):
    # Use the same source fixtures, but test the actual DLL API code via a POSIX OS boundary.
    def test_public_api_and_enable_rumble_keystrokes(self):
        result = subprocess.run([str(transport.ROOT / "build/api-test"), "--self-test"],
                                text=True, capture_output=True, timeout=10)
        print(result.stdout)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SELFTEST PASS", result.stdout)
        self.log.seek(0)
        output = self.log.read()
        self.assertEqual(output.count(b"RUMBLE 10000 20000"), 1)

    def test_absent_host_returns_disconnected(self):
        self.stop()
        result = subprocess.run([str(transport.ROOT / "build/api-test"), "--disconnected"],
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DISCONNECTED PASS", result.stdout)


class DisconnectedApiTest(transport.FakeHostCase):
    fake_args = ["--disconnected"]

    def test_present_host_with_no_controller_returns_disconnected(self):
        result = subprocess.run([str(transport.ROOT / "build/api-test"), "--disconnected"],
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.log.seek(0)
        self.assertNotIn(b"RUMBLE", self.log.read())
