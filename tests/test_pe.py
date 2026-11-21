# SPDX-License-Identifier: MIT OR Apache-2.0
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PeTest(unittest.TestCase):
    def test_dll_is_x64_and_exports_the_complete_public_13_surface(self):
        dump = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(ROOT / "build/xinput1_3.dll")], text=True)
        self.assertIn("file format pei-x86-64", dump)
        names = set(re.findall(r"\b(XInput\w+)\s*$", dump, re.MULTILINE))
        self.assertEqual(names, {"XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable",
                                 "XInputGetDSoundAudioDeviceGuids", "XInputGetBatteryInformation", "XInputGetKeystroke"})
        # Fallout 4 imports 2, 3 and 4 by ordinal, so ordinals are part of the ABI (README: Export ordinals).
        ordinals = {name: int(number) for number, name in
                    re.findall(r"\+base\[\s*(\d+)\]\s+[0-9a-f]+\s+(XInput\w+)\s*$", dump, re.MULTILINE)}
        self.assertEqual(ordinals, {"XInputGetState": 2, "XInputSetState": 3, "XInputGetCapabilities": 4,
                                    "XInputEnable": 5, "XInputGetDSoundAudioDeviceGuids": 6,
                                    "XInputGetBatteryInformation": 7, "XInputGetKeystroke": 8})
        imports = re.findall(r"DLL Name: (\S+)", dump)
        self.assertIn("WS2_32.dll", imports)
        self.assertFalse(any("libgcc" in name.lower() or "pthread" in name.lower() for name in imports))

    def test_probe_is_x64_and_loads_xinput_dynamically(self):
        dump = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(ROOT / "build/xinput-probe.exe")], text=True)
        self.assertIn("file format pei-x86-64", dump)
        self.assertNotIn("DLL NAME: XINPUT", dump.upper())
