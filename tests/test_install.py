# SPDX-License-Identifier: MIT OR Apache-2.0
import importlib.util
import json
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("installer", Path(__file__).resolve().parents[1] / "scripts/install.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class Registry:
    def __init__(self, exists, value, extra=False):
        self.exists, self.value, self.extra = exists, value, extra
        self.fail_add = False
        self.fail_query = False
        self.prefixes = set()

    def reg(self, wine, prefix, *args, check=True):
        self.prefixes.add(prefix)
        action = args[0]
        status = 0
        if action == "query" and self.fail_query:
            raise subprocess.TimeoutExpired([str(wine), "reg", *args], 60)
        if action == "query":
            status = 0 if args[1] != installer.KEY or self.exists else 1
        elif action == "export":
            path = Path(args[2][2:].replace("\\", "/"))
            content = "Windows Registry Editor Version 5.00\r\n\r\n[" + installer.FULL_KEY + "]\r\n"
            if self.value is not None:
                content += self.value + "\r\n"
            if self.extra:
                content += '"other"="builtin"\r\n'
            path.write_bytes(content.encode("utf-16"))
        elif action == "add":
            if self.fail_add:
                raise RuntimeError("injected reg add failure")
            self.exists, self.value = True, '"xinput1_3"="native,builtin"'
        elif action == "delete":
            if "/v" in args:
                self.value = None
            else:
                self.exists = False
        elif action == "import":
            path = Path(args[1][2:].replace("\\", "/"))
            self.value, _ = installer.parse_export(path.read_bytes())
            self.exists = True
        else:
            raise AssertionError(args)
        if check and status:
            raise RuntimeError("query failed")
        return subprocess.CompletedProcess(args, status, "", "")


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gip install space ' ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "prefix"
        (self.prefix / "drive_c").mkdir(parents=True)
        (self.prefix / "user.reg").write_text("WINE REGISTRY Version 2\n")
        self.game = self.prefix / "drive_c/Games/Example Game"
        self.game.mkdir(parents=True)
        (self.game / "Game.exe").write_bytes(b"test")
        self.dll = self.root / "bridge.dll"
        pe = bytearray(128)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 60, 64)
        pe[64:68] = b"PE\0\0"
        struct.pack_into("<H", pe, 68, 0x8664)
        struct.pack_into("<H", pe, 86, 0x2000)
        struct.pack_into("<H", pe, 88, 0x20b)
        self.dll.write_bytes(pe)
        self.wine = self.root / "wine"
        self.wine.write_text("mock")
        self.wine.chmod(0o755)
        self.state = self.root / "journal"

    def install(self, registry, game=None, exe=None):
        with patch.object(installer, "reg", registry.reg):
            installer.install(game or self.game, self.dll, self.state, self.wine, self.prefix, exe)

    def uninstall(self, registry):
        with patch.object(installer, "reg", registry.reg):
            installer.uninstall(self.state)

    def test_absent_key_and_dll_restored_and_literal_path_is_quoted(self):
        registry = Registry(False, None)
        self.install(registry)
        script = (self.state / "remove-installed-dll.sh").read_text()
        self.assertNotIn("$", script)
        self.assertIn("/bin/rm -f -- /", script.replace("'", ""))
        self.uninstall(registry)
        self.assertFalse((self.game / "xinput1_3.dll").exists())
        self.assertFalse(registry.exists)
        self.uninstall(registry)  # idempotent, retained audit record

    def test_existing_dll_mode_timestamp_case_and_typed_value_restored(self):
        target = self.game / "XINPUT1_3.dll"
        target.write_bytes(b"original")
        target.chmod(0o640)
        import os
        os.utime(target, ns=(1_500_000_000_000_000_000, 1_500_000_000_000_000_000))
        before = target.stat()
        value = '"XInput1_3"=hex(2):6e,00,61,00,74,00,69,00,76,00,65,00,00,00'
        registry = Registry(True, value, extra=True)
        self.install(registry)
        self.assertEqual(target.read_bytes(), self.dll.read_bytes())
        self.uninstall(registry)
        self.assertEqual(target.read_bytes(), b"original")
        self.assertEqual(target.stat().st_mode, before.st_mode)
        self.assertEqual(target.stat().st_mtime_ns, before.st_mtime_ns)
        self.assertEqual(registry.value, value)
        self.assertTrue(registry.extra)

    def test_unrelated_value_added_after_install_is_preserved(self):
        registry = Registry(False, None)
        self.install(registry)
        registry.extra = True
        self.uninstall(registry)
        self.assertTrue(registry.exists and registry.extra)
        self.assertIsNone(registry.value)

    def test_add_failure_rolls_back_both_changes(self):
        registry = Registry(False, None)
        registry.fail_add = True
        with self.assertRaisesRegex(RuntimeError, "injected"):
            self.install(registry)
        self.assertFalse((self.game / "xinput1_3.dll").exists())
        self.assertFalse(registry.exists)
        self.assertEqual(json.loads((self.state / "manifest.json").read_text())["phase"], "restored")

    def test_changed_dll_or_override_is_not_overwritten(self):
        registry = Registry(False, None)
        self.install(registry)
        target = self.game / "xinput1_3.dll"
        target.write_bytes(b"user edit")
        with self.assertRaisesRegex(RuntimeError, "DLL changed"):
            self.uninstall(registry)
        self.assertEqual(target.read_bytes(), b"user edit")
        target.write_bytes(self.dll.read_bytes())
        registry.value = '"xinput1_3"="user edit"'
        with self.assertRaisesRegex(RuntimeError, "override changed"):
            self.uninstall(registry)
        self.assertTrue(target.exists())

    def test_symlinks_are_refused_before_registry_mutation(self):
        (self.game / "xinput1_3.dll").symlink_to(self.dll)
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            self.install(Registry(False, None))
        self.assertFalse(self.state.exists())

    def test_multiline_registry_value_is_preserved(self):
        value = '"xinput1_3"=hex:01,02,\\\r\n  03,04'
        data = ("Windows Registry Editor Version 5.00\r\n\r\n[" + installer.FULL_KEY + "]\r\n" + value + "\r\n").encode("utf-16")
        self.assertEqual(installer.parse_export(data), (value, False))

    def test_wrong_architecture_is_refused_before_changing_anything(self):
        pe = bytearray(self.dll.read_bytes())
        struct.pack_into("<H", pe, 68, 0x14c)
        self.dll.write_bytes(pe)
        with self.assertRaisesRegex(RuntimeError, "x86_64"):
            self.install(Registry(False, None))
        self.assertFalse(self.state.exists())

    def test_registry_edits_go_to_the_given_prefix(self):
        registry = Registry(False, None)
        self.install(registry, exe="Game.exe")
        self.uninstall(registry)
        self.assertEqual(registry.prefixes, {self.prefix.resolve()})
        manifest = json.loads((self.state / "manifest.json").read_text())
        self.assertEqual(manifest["prefix"], str(self.prefix.resolve()))
        self.assertEqual(manifest["wine"], str(self.wine.resolve()))

    def test_game_outside_the_prefix_is_refused_before_changing_anything(self):
        elsewhere = self.root / "other prefix/drive_c/Game"
        elsewhere.mkdir(parents=True)
        registry = Registry(False, None)
        with self.assertRaisesRegex(RuntimeError, "inside"):
            self.install(registry, game=elsewhere)
        self.assertFalse(self.state.exists())
        self.assertEqual(registry.prefixes, set())
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_missing_executable_and_non_prefix_are_refused(self):
        with self.assertRaisesRegex(RuntimeError, "does not contain Other.exe"):
            self.install(Registry(False, None), exe="Other.exe")
        (self.prefix / "user.reg").unlink()
        with self.assertRaisesRegex(RuntimeError, "not a Wine prefix"):
            self.install(Registry(False, None))
        self.assertFalse(self.state.exists())

    def test_failed_first_snapshot_leaves_no_journal_behind(self):
        target = self.game / "xinput1_3.dll"
        target.write_bytes(b"original")
        registry = Registry(False, None)
        registry.fail_query = True
        with self.assertRaises(subprocess.TimeoutExpired):
            self.install(registry)
        self.assertFalse(self.state.exists())
        self.assertEqual(target.read_bytes(), b"original")
        registry.fail_query = False
        self.install(registry)  # the same --state works again
        self.uninstall(registry)
        self.assertEqual(target.read_bytes(), b"original")
