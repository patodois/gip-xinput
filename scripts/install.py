#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Install the bridge DLL into one game directory of a Wine prefix, with a reversible journal."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KEY = r"HKCU\Software\Wine\DllOverrides"
FULL_KEY = r"HKEY_CURRENT_USER\Software\Wine\DllOverrides"
VALUE = "xinput1_3"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def environment(prefix, wine):
    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDEBUG"] = "-all"
    env["PATH"] = str(wine.parent) + os.pathsep + env.get("PATH", "")
    env["WINESERVER"] = str(wine.parent / "wineserver")
    return env


def reg(wine, prefix, *args, check=True):
    # Argument arrays preserve paths/spaces without shell expansion.
    result = subprocess.run([str(wine), "reg", *args], env=environment(prefix, wine),
                            capture_output=True, text=True, errors="replace", timeout=60)
    if check and result.returncode:
        raise RuntimeError(f"wine reg {args[0]} failed: {result.stdout} {result.stderr}")
    return result


def wine_path(path):
    return "Z:" + str(Path(path).resolve()).replace("/", "\\")


def parse_export(data):
    """Return (exact value assignment or None, key is completely empty)."""
    text = data.decode("utf-16") if data.startswith((b"\xff\xfe", b"\xfe\xff")) else data.decode("utf-8-sig")
    lines = text.replace("\r\n", "\n").splitlines()
    sections = []
    assignments = []
    current = None
    i = 0
    found = None
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            sections.append(current)
        elif line.startswith(('"', '@')):
            whole = line
            while whole.rstrip().endswith("\\"):
                if i == len(lines):
                    raise RuntimeError("truncated registry export")
                whole += "\r\n" + lines[i]
                i += 1
            assignments.append(whole)
            if current and current.casefold() == FULL_KEY.casefold():
                match = re.match(r'^"([^"\\]+)"=', whole)
                if match and match[1].casefold() == VALUE:
                    if found is not None:
                        raise RuntimeError("duplicate override in export")
                    found = whole
    if not sections or sections[0].casefold() != FULL_KEY.casefold():
        raise RuntimeError("unexpected registry export header")
    return found, len(sections) == 1 and not assignments


def assignment_data(line):
    return line.split("=", 1)[1] if line is not None else None


def snapshot(wine, prefix, destination):
    parent = reg(wine, prefix, "query", r"HKCU\Software\Wine")
    query = reg(wine, prefix, "query", KEY, check=False)
    if query.returncode == 1:
        if FULL_KEY.casefold() in parent.stdout.casefold():
            raise RuntimeError("DllOverrides exists but cannot be read; refusing to change it")
        return False, None, True
    if query.returncode:
        raise RuntimeError("cannot query DllOverrides")
    reg(wine, prefix, "export", KEY, wine_path(destination), "/y")
    if not destination.is_file():
        raise RuntimeError("wine did not create the registry snapshot")
    value, empty = parse_export(destination.read_bytes())
    return True, value, empty


def write_manifest(state, info):
    temp = state / "manifest.next.json"
    with temp.open("w") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    temp.replace(state / "manifest.json")


def literal_remover(target):
    if not target.is_absolute() or target.name.casefold() != "xinput1_3.dll":
        raise RuntimeError("invalid DLL removal path")
    return "#!/bin/sh\nset -eu\n/bin/rm -f -- " + shlex.quote(str(target)) + "\n"


def validate_file(path):
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise RuntimeError(f"refusing symlink or special file: {path}")


def validate_dll(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise RuntimeError("DLL is not a PE binary; run make windows")
    offset = struct.unpack_from("<I", data, 60)[0]
    if (offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0"
            or struct.unpack_from("<H", data, offset + 4)[0] != 0x8664
            or not struct.unpack_from("<H", data, offset + 22)[0] & 0x2000
            or struct.unpack_from("<H", data, offset + 24)[0] != 0x20b):
        raise RuntimeError("DLL must be a PE32+ x86_64 DLL; run make windows")


def uninstall(state):
    state = state.resolve(strict=True)
    info = json.loads((state / "manifest.json").read_text())
    if info["phase"] == "restored":
        print("Already restored; journal and backups retained.")
        return
    wine, prefix, target = (Path(info[k]) for k in ("wine", "prefix", "target"))
    game = Path(info["game"])
    if target.parent != game or target.parent.resolve() != game or target.name.casefold() != "xinput1_3.dll":
        raise RuntimeError("journal target does not match game directory")
    validate_file(target)
    current_hash = digest(target) if target.exists() else None
    allowed = {info["installed_sha256"], info["original_sha256"], None}
    if current_hash not in allowed and info["phase"] != "prepared":
        raise RuntimeError("DLL changed since installation; preserving it and the journal")
    if info["original_sha256"]:
        backup = state / "original.dll"
        validate_file(backup)
        if digest(backup) != info["original_sha256"]:
            raise RuntimeError("original DLL backup is corrupt")
    remover = state / "remove-installed-dll.sh"
    if remover.read_text() != literal_remover(target):
        raise RuntimeError("literal removal script does not match journal")
    _, current_value, _ = snapshot(wine, prefix, state / "uninstall-current.reg")
    prior_value = info["original_override"]
    if assignment_data(current_value) not in {assignment_data(prior_value), '"native,builtin"', None}:
        raise RuntimeError("override changed since installation; preserving it and the journal")
    if current_value is None and prior_value is not None and info["phase"] == "installed":
        raise RuntimeError("override changed since installation; preserving it and the journal")
    # Registry first: if Wine is unavailable, the DLL and backup remain recoverable.
    info["phase"] = "restoring"
    write_manifest(state, info)
    if current_value is not None:
        reg(wine, prefix, "delete", KEY, "/v", VALUE, "/f")
    if prior_value is not None:
        restore = state / "restore-value.reg"
        text = "Windows Registry Editor Version 5.00\r\n\r\n[" + FULL_KEY + "]\r\n" + prior_value + "\r\n"
        restore.write_bytes(text.encode("utf-16"))
        reg(wine, prefix, "import", wine_path(restore))
    elif not info["original_key_existed"]:
        exists, _, empty = snapshot(wine, prefix, state / "after-value-removal.reg")
        if exists and empty:
            reg(wine, prefix, "delete", KEY, "/f")
    if info["original_sha256"]:
        shutil.copy2(state / "original.dll", target)
        os.utime(target, ns=(info["original_atime_ns"], info["original_mtime_ns"]))
    elif target.exists():
        # This script contains exactly one absolute literal rm operand, generated at install.
        subprocess.run(["/bin/sh", str(remover)], check=True)
    info["phase"] = "restored"
    write_manifest(state, info)
    print("Restored the prior DLL and override. Backups retained at", state)


def validate_setup(game, wine, prefix, exe):
    if not wine.is_file() or not os.access(wine, os.X_OK):
        raise RuntimeError(f"--wine is not an executable file: {wine}")
    if not (prefix / "drive_c").is_dir() or not (prefix / "user.reg").is_file():
        raise RuntimeError(f"--prefix is not a Wine prefix (no drive_c or user.reg): {prefix}")
    # The DLL override is written to this prefix, so the game must be one of its programs.
    if not game.is_dir() or not game.is_relative_to(prefix / "drive_c"):
        raise RuntimeError(f"--game-dir must be a directory inside {prefix / 'drive_c'}")
    if exe is not None and not (game / exe).is_file():
        raise RuntimeError(f"game directory does not contain {exe}")


# Files install() writes into a new journal before its manifest exists.
PREPARED_FILES = ("original-key.reg", "original.dll", "remove-installed-dll.sh", "manifest.next.json")


def discard_unfinished_journal(state):
    """Remove what install() wrote before its first manifest; nothing outside the journal changed yet."""
    for name in PREPARED_FILES:
        (state / name).unlink(missing_ok=True)
    try:
        state.rmdir()
    except OSError:
        print("Left", state, "because it holds files this installer did not write", file=sys.stderr)


def install(game, dll, state, wine, prefix, exe=None):
    game, dll = game.resolve(strict=True), dll.resolve(strict=True)
    wine, prefix = wine.resolve(strict=True), prefix.resolve(strict=True)
    validate_setup(game, wine, prefix, exe)
    validate_dll(dll)
    candidates = [p for p in game.iterdir() if p.name.casefold() == "xinput1_3.dll"]
    if len(candidates) > 1:
        raise RuntimeError("multiple case variants of xinput1_3.dll; resolve them before installing")
    target = candidates[0] if candidates else game / "xinput1_3.dll"
    validate_file(target)
    state = state.absolute()
    if state.is_symlink() or state.exists():
        raise RuntimeError("state directory already exists; uninstall it or choose a fresh --state")
    state.mkdir(mode=0o700, parents=True)
    state = state.resolve(strict=True)
    try:
        key_existed, value, _ = snapshot(wine, prefix, state / "original-key.reg")
        original_stat = target.stat() if target.exists() else None
        original = digest(target) if original_stat else None
        if original:
            shutil.copy2(target, state / "original.dll")
        remover = state / "remove-installed-dll.sh"
        remover.write_text(literal_remover(target))
        info = dict(phase="prepared", wine=str(wine), prefix=str(prefix), game=str(game), target=str(target),
                    original_key_existed=key_existed, original_override=value,
                    original_atime_ns=original_stat.st_atime_ns if original_stat else None,
                    original_mtime_ns=original_stat.st_mtime_ns if original_stat else None,
                    original_sha256=original, installed_sha256=digest(dll))
        write_manifest(state, info)  # all backups saved before either external mutation
    except BaseException:
        # Without a manifest the journal could neither be uninstalled nor reused; nothing outside
        # it has changed yet, so remove it and let the same --state be used again.
        discard_unfinished_journal(state)
        raise
    try:
        shutil.copy2(dll, target)
        reg(wine, prefix, "add", KEY, "/v", VALUE, "/t", "REG_SZ", "/d", "native,builtin", "/f")
        info["phase"] = "installed"
        write_manifest(state, info)
    except Exception:
        print("Installation failed; attempting restoration from", state, file=sys.stderr)
        uninstall(state)
        raise
    print("Installed", target)
    print("Journal:", state)
    print("Uninstall:", shlex.join([str(ROOT / "scripts/uninstall.sh"), "--state", str(state)]))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="The registry override goes into --prefix, and --game-dir must be inside that prefix's "
               "drive_c. Use the Wine build that normally runs that prefix. See README.md for an example.")
    parser.add_argument("action", choices=["install", "uninstall"])
    parser.add_argument("--state", type=Path, default=ROOT / ".install-state",
                        help="journal directory (default: .install-state in this repository)")
    parser.add_argument("--wine", type=Path, help="the wine executable that runs the prefix (install)")
    parser.add_argument("--prefix", type=Path, help="the Wine prefix (bottle) the game runs in (install)")
    parser.add_argument("--game-dir", type=Path, help="the directory holding the game's executable (install)")
    parser.add_argument("--exe", help="optional: refuse unless the game directory holds this file")
    parser.add_argument("--dll", type=Path, default=ROOT / "build/xinput1_3.dll")
    args = parser.parse_args()
    if args.action == "install":
        missing = [name for name in ("wine", "prefix", "game_dir") if getattr(args, name) is None]
        if missing:
            parser.error("install needs " + ", ".join("--" + name.replace("_", "-") for name in missing))
        install(args.game_dir, args.dll, args.state, args.wine, args.prefix, args.exe)
    else:
        uninstall(args.state)

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        sys.exit(str(error))
