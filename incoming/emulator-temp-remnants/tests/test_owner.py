# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Run in an x64 VS developer shell: python test_owner.py <patched qemu tree>."""
import ctypes
from concurrent.futures import ThreadPoolExecutor
import _winapi
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SOURCE = Path(sys.argv.pop(1)).resolve() / "android-qemu2-glue"


class OwnerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        cls.exe = Path(cls.build.name) / "owner.exe"
        subprocess.run(["cl", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                        "/I" + str(SOURCE), str(HERE / "owner.cpp"),
                        str(SOURCE / "windows-temp-images.cpp"), "crypt32.lib", "advapi32.lib", "/Fe:" + str(cls.exe)],
                       cwd=cls.build.name, check=True)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.a = root / "一" / "same.avd"
        self.b = root / "二" / "same.avd"
        self.temp = root / "共用 TEMP"
        for path in (self.a, self.b, self.temp):
            path.mkdir(parents=True)

    def recover(self, avd):
        subprocess.run([self.exe, avd, self.temp, "recover"], check=True, timeout=15)

    def start(self, avd, tag, temp=None, mode="hold"):
        p = subprocess.Popen([self.exe, avd, temp or self.temp, tag, mode],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        def close():
            if p.poll() is None:
                p.kill()
            p.wait(timeout=15)
            p.stdin.close()
            p.stdout.close()
        self.addCleanup(close)
        self.assertEqual(p.stdout.readline().strip(), "ready")
        return p

    def stop(self, p, *, crash):
        if crash:
            p.kill()
        else:
            p.stdin.write("release\n")
            p.stdin.flush()
        rc = p.wait(timeout=15)
        if crash:
            self.assertNotEqual(rc, 0)
        else:
            self.assertEqual(rc, 0)

    def payloads(self, tag):
        return {p: p.read_bytes() for p in self.temp.glob("modavd-" + tag + "*.tmp*")
                if p.suffix != ".lease"}

    def test_process_exit_and_owner_boundaries(self):
        stray = {}
        for name in ("TMP1234.tmp", "TMP1234.tmp.qcow2", "modavd-unknown.tmp",
                     "modavd-unknown.tmp.lease"):
            p = self.temp / name
            p.write_bytes(os.urandom(24))
            stray[p] = p.read_bytes()
        live = self.start(self.a, "live")
        victim = self.start(self.a, "victim")
        other = self.start(self.b, "other")
        keep = self.payloads("live") | self.payloads("other") | stray
        self.assertEqual(len(self.payloads("victim")), 4)
        self.stop(victim, crash=True)
        self.stop(other, crash=True)
        self.recover(self.a)
        self.assertEqual(self.payloads("victim"), {})
        self.assertEqual({p: p.read_bytes() for p in keep}, keep)
        self.stop(live, crash=False)
        self.assertEqual(self.payloads("live"), {})
        self.recover(self.b)
        self.assertEqual(set(self.temp.iterdir()), set(stray))

    def test_simultaneous_start_and_recovery(self):
        live = self.start(self.a, "live")
        other = self.start(self.b, "foreign")
        stale = self.start(self.a, "stale")
        self.stop(other, crash=True)
        self.stop(stale, crash=True)
        # Each process recovers the same AVD, then tries to reserve the same
        # raw and sidecar names while the winner is still alive.
        processes = [subprocess.Popen([self.exe, self.a, self.temp, "race", "race"],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                     for _ in range(8)]
        try:
            for p in processes:
                self.assertEqual(p.stdout.readline().strip(), "waiting")
            for p in processes:
                p.stdin.write("go\n")
                p.stdin.flush()
            winners = [p for p in processes if p.stdout.readline().strip() == "ready"]
            self.assertEqual(len(winners), 1)
            self.assertTrue(all(p.wait(timeout=15) == 4 for p in processes if p not in winners))
            with ThreadPoolExecutor(max_workers=4) as workers:
                list(workers.map(self.recover, [self.a] * 8))
            self.assertFalse(self.payloads("stale"))
            for tag in ("foreign", "live", "race"):
                self.assertEqual(len(self.payloads(tag)), 4)
            self.stop(winners[0], crash=False)
            self.stop(live, crash=False)
            self.recover(self.b)
            self.assertFalse(list(self.temp.iterdir()))
            self.assertFalse(list(self.a.glob("*.lease")))
            self.assertFalse(list(self.b.glob("*.lease")))
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                p.wait(timeout=15)
                p.stdin.close()
                p.stdout.close()

    def test_directory_pins_allow_unrelated_renames(self):
        p = self.start(self.a, "directory", mode="pin-only")
        move = ctypes.WinDLL("kernel32", use_last_error=True).MoveFileExW
        move.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_ulong]
        move.restype = ctypes.c_int
        for directory in (self.a, self.temp):
            with self.subTest(directory=directory.name):
                # Same flags and names as main.cpp::convertExt4ToQcow2.
                destination = directory / "userdata-qemu.img"
                source = directory / "userdata-qemu.img.tmp.qcow2"
                destination.write_bytes(b"old raw")
                source.write_bytes(b"new qcow2")
                ok = move(str(source), str(destination), 0x1 | 0x8)
                self.assertTrue(ok, "MoveFileExW error " + str(ctypes.get_last_error()))
                self.assertEqual(destination.read_bytes(), b"new qcow2")
                self.assertFalse(source.exists())
                renamed = directory / "renamed-userdata.img"
                self.assertTrue(move(str(destination), str(renamed), 0),
                                "MoveFileExW error " + str(ctypes.get_last_error()))
                self.assertEqual(renamed.read_bytes(), b"new qcow2")
        self.stop(p, crash=False)

    @unittest.skipUnless(os.environ.get("MODAVD_QEMU_IMG"), "set MODAVD_QEMU_IMG for SDK conversion")
    def test_pinned_directory_allows_userdata_conversion(self):
        p = self.start(self.a, "conversion", mode="pin-only")
        sdk = os.environ["MODAVD_QEMU_IMG"]
        move = ctypes.WinDLL("kernel32", use_last_error=True).MoveFileExW
        move.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_ulong]
        move.restype = ctypes.c_int
        raw = self.a / "userdata-qemu.img"
        converted = self.a / "userdata-qemu.img.tmp.qcow2"
        subprocess.run([sdk, "create", "-q", "-f", "raw", raw.name, "1M"],
                       cwd=self.a, check=True, timeout=30)
        subprocess.run([sdk, "convert", "-T", "none", "-O", "qcow2", raw.name, converted.name],
                       cwd=self.a, check=True, timeout=30)
        self.assertTrue(move(str(converted), str(raw), 0x1 | 0x8),
                        "MoveFileExW error " + str(ctypes.get_last_error()))
        info = json.loads(subprocess.check_output([sdk, "info", "--output=json", raw.name], cwd=self.a))
        self.assertEqual(info["format"], "qcow2")
        self.assertEqual(info["virtual-size"], 1024 * 1024)
        self.assertFalse(converted.exists())
        self.stop(p, crash=False)

    def test_directory_pin_blocks_directory_rename(self):
        p = self.start(self.a, "pinned", mode="pin-only")
        for directory in (self.a, self.temp):
            with self.subTest(directory=directory.name):
                with self.assertRaises(OSError):
                    directory.rename(directory.with_name("moved"))
        self.stop(p, crash=False)

    def test_live_file_cannot_be_replaced(self):
        p = self.start(self.a, "pin")
        for file in self.payloads("pin"):
            with self.assertRaises(OSError):
                file.rename(self.temp / "renamed")
            replacement = self.temp / "replacement"
            replacement.write_bytes(b"keep")
            with self.assertRaises(OSError):
                os.replace(replacement, file)
            self.assertEqual(replacement.read_bytes(), b"keep")
            replacement.unlink()
        self.stop(p, crash=False)

    def test_junction_retarget_does_not_redirect_writes(self):
        alias = self.temp.parent / "temp-alias"
        alias2 = self.temp.parent / "other-alias"
        foreign = self.temp.parent / "foreign-temp"
        foreign.mkdir()
        _winapi.CreateJunction(str(self.temp), str(alias))
        _winapi.CreateJunction(str(foreign), str(alias2))
        p = self.start(self.a, "junction", temp=alias)
        alias.rename(self.temp.parent / "old-alias")
        alias2.rename(alias)
        sentinel = foreign / "modavd-junction.tmp"
        sentinel.write_bytes(b"keep")
        p.stdin.write("rewrite\n")
        p.stdin.flush()
        self.assertEqual(p.wait(timeout=15), 0)
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.assertFalse(list(self.temp.iterdir()))
        self.assertFalse(list(self.a.glob("*.lease")))

    def test_recovery_rejects_renamed_pair(self):
        p = self.start(self.a, "pair")
        self.stop(p, crash=True)
        image = self.temp / "modavd-pair.tmp"
        record = self.a / (image.name + ".lease")
        moved = self.temp / "modavd-another.tmp"
        image.rename(moved)
        record.rename(self.a / (moved.name + ".lease"))
        self.recover(self.a)
        self.assertEqual(moved.read_bytes(), b"created by pair")

    def test_modified_and_transplanted_records(self):
        a = self.start(self.a, "alpha")
        b = self.start(self.b, "beta")
        self.stop(a, crash=True)
        self.stop(b, crash=True)
        target = self.temp / "modavd-beta.tmp"
        record = self.b / (target.name + ".lease")
        copied = self.a / record.name
        shutil.copyfile(record, copied)
        # The shared TEMP directory is never an authority on who owns an image.
        shutil.copyfile(record, self.temp / record.name)
        self.recover(self.a)
        self.assertTrue(target.exists())
        self.assertTrue(copied.exists())
        self.assertTrue(record.exists())
        data = bytearray(record.read_bytes())
        data[-1] ^= 1
        record.write_bytes(data)
        self.recover(self.b)
        self.assertTrue(target.exists())
        self.assertEqual(record.read_bytes(), data)

    def test_moved_genuine_record_does_not_authorize_another_root(self):
        p = self.start(self.b, "move-record")
        self.stop(p, crash=True)
        file = self.temp / "modavd-move-record.tmp"
        source = self.b / (file.name + ".lease")
        moved = self.a / source.name
        source.rename(moved)
        self.recover(self.a)
        self.assertEqual(file.read_bytes(), b"created by move-record")
        self.assertTrue(moved.exists())
        moved.rename(source)
        self.recover(self.b)
        self.assertFalse(file.exists())

    def test_moved_image_in_different_temp(self):
        p = self.start(self.a, "move-image")
        self.stop(p, crash=True)
        preserved = self.temp.parent / "preserved"
        self.temp.rename(preserved)
        self.temp.mkdir()
        file = preserved / "modavd-move-image.tmp"
        # Even reusing the old TEMP name must not retire a record for a
        # different directory. Passing the moved directory changes its path.
        self.recover(self.a)
        subprocess.run([self.exe, self.a, preserved, "recover"], check=True, timeout=15)
        self.assertTrue(file.exists())
        self.assertTrue((self.a / (file.name + ".lease")).exists())

    def test_creation_permissions(self):
        p = self.start(self.a, "acl")
        self.stop(p, crash=True)
        env = os.environ.copy()
        env["TEST_IMAGE"] = str(self.temp / "modavd-acl.tmp")
        env["TEST_LEASE"] = str(self.a / "modavd-acl.tmp.lease")
        # Inspect the actual Windows ACL after both objects have been created.
        check = """
        $ErrorActionPreference = 'Stop'
        $user = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        foreach ($path in $env:TEST_IMAGE, $env:TEST_LEASE) {
            $acl = [IO.File]::GetAccessControl($path)
            if (!$acl.AreAccessRulesProtected) { exit 2 }
            $rules = $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])
            $principals = @($rules | ForEach-Object { $_.IdentityReference.Value })
            if ($user -notin $principals -or 'S-1-5-18' -notin $principals) { exit 3 }
            if (@($principals | Where-Object {$_ -ne $user -and $_ -ne 'S-1-5-18'}).Count) { exit 4 }
        }
        """
        subprocess.run(["powershell.exe", "-NoProfile", "-Command", check],
                       env=env, check=True, timeout=30)

    def test_replacement_and_held_file(self):
        p = self.start(self.a, "replace")
        self.stop(p, crash=True)
        original = self.temp / "modavd-replace.tmp"
        moved = self.temp / "moved-original"
        original.rename(moved)
        original.write_bytes(b"unrelated replacement")
        held = self.temp / "modavd-replace-drive.tmp.qcow2"
        with held.open("rb") as reader:
            self.recover(self.a)
            self.assertEqual(reader.read(), b"created by replace")
            self.assertTrue(held.exists())
        self.recover(self.a)
        self.assertFalse(held.exists())
        self.assertEqual(original.read_bytes(), b"unrelated replacement")
        self.assertEqual(moved.read_bytes(), b"created by replace")

    def test_extra_link_and_reparse_point(self):
        p = self.start(self.a, "links")
        self.stop(p, crash=True)
        file = self.temp / "modavd-links.tmp"
        alias = self.temp / "extra-link"
        os.link(file, alias)
        drive = self.temp / "modavd-links-drive.tmp.qcow2"
        drive.unlink()
        try:
            drive.symlink_to(alias)
        except OSError:
            drive.write_bytes(b"unrelated replacement")
        self.recover(self.a)
        self.assertEqual(file.read_bytes(), b"created by links")
        self.assertTrue(drive.exists())
        alias.unlink()
        self.recover(self.a)
        self.assertFalse(file.exists())
        self.assertTrue(os.path.lexists(drive))

    def test_missing_receipt_preserves_crash_file(self):
        receipt = self.a / "modavd-no-proof.tmp.lease"
        receipt.write_bytes(b"existing unrelated file")
        p = self.start(self.a, "no-proof")
        self.stop(p, crash=True)
        self.recover(self.a)
        self.assertEqual(receipt.read_bytes(), b"existing unrelated file")
        self.assertEqual((self.temp / "modavd-no-proof.tmp").read_bytes(), b"created by no-proof")

    def test_sidecar_collision(self):
        existing = self.temp / "modavd-collision.tmp.qcow2"
        existing.write_bytes(b"not ours")
        subprocess.run([self.exe, self.a, self.temp, "collision", "collision"], check=True)
        self.assertEqual(set(self.temp.iterdir()), {existing})
        self.assertEqual(existing.read_bytes(), b"not ours")

    @unittest.skipUnless(os.environ.get("MODAVD_QEMU_IMG"), "set MODAVD_QEMU_IMG for SDK I/O")
    def test_qemu_image_creation_and_mirroring(self):
        p = self.start(self.a, "sdk")
        base = self.temp / "modavd-sdk.tmp"
        with base.open("r+b") as disk:
            disk.truncate(66 << 20)
        cache = base.with_suffix(".tmp.qcow2")
        drive = self.temp / "modavd-sdk-drive.tmp.qcow2"

        def image(*args):
            return subprocess.check_output([os.environ["MODAVD_QEMU_IMG"], *args], cwd=self.temp)

        image("create", "-q", "-f", "qcow2", "-F", "raw", "-b", base.name, cache.name)
        image("create", "-q", "-f", "qcow2", "-F", "qcow2", "-b", cache.name, drive.name)
        self.assertEqual(json.loads(image("info", "--output=json", drive.name))["backing-filename"],
                         cache.name)
        # drive-share mirrors the cache when remounting a read-only snapshot on x86.
        source = self.temp / "copy-source.qcow2"
        shutil.copyfile(cache, source)
        copy_file = ctypes.WinDLL("kernel32", use_last_error=True).CopyFileW
        self.assertTrue(copy_file(str(source), str(drive), False), ctypes.get_last_error())
        image("check", "-q", drive.name)
        self.stop(p, crash=True)
        self.recover(self.a)
        self.assertFalse(cache.exists())
        self.assertFalse(drive.exists())
        self.assertTrue(source.exists())

    def test_long_temp_uses_backing_paths_that_fit_qemu(self):
        parent = self.temp.parent
        padding = 226 - len(str(parent).encode("utf-8")) - 1
        if not 1 <= padding <= 240:
            self.skipTest("needs a lab root below the emulator path limit")
        long_temp = parent / ("t" * padding)
        long_temp.mkdir()
        tag = "1b0d628c-f25b-4c4e-9951-45b716e71379"
        sdk = os.environ.get("MODAVD_QEMU_IMG")

        def allocate():
            proc = subprocess.Popen([self.exe, self.a, long_temp, tag, "paths"],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    text=True, encoding="utf-8")
            def close():
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=15)
                proc.stdin.close()
                proc.stdout.close()
            self.addCleanup(close)
            cache = Path(proc.stdout.readline().strip())
            drive = Path(proc.stdout.readline().strip())
            self.assertEqual(proc.stdout.readline().strip(), "ready")
            self.assertLess(len(str(cache.with_suffix(".tmp.qcow2")).encode()), 260)
            self.assertLess(len(str(drive).encode()), 260)
            self.assertEqual(len(cache.stem), 7)
            return proc, cache, drive

        p, raw, drive = allocate()
        side = raw.with_suffix(".tmp.qcow2")
        if sdk and str(long_temp).isascii():
            with raw.open("r+b") as disk:
                disk.truncate(66 << 20)
            subprocess.run([sdk, "create", "-q", "-f", "qcow2", "-F", "raw",
                            "-b", raw.name, side], check=True)
            subprocess.run([sdk, "create", "-q", "-f", "qcow2", "-F", "qcow2",
                            "-b", side, drive], check=True)
            chain = json.loads(subprocess.check_output(
                [sdk, "info", "--output=json", "--backing-chain", drive]))
            self.assertEqual([layer["format"] for layer in chain], ["qcow2", "qcow2", "raw"])
            self.assertEqual(chain[0]["virtual-size"], 66 << 20)
        self.stop(p, crash=True)
        subprocess.run([self.exe, self.a, long_temp, "recover"], check=True, timeout=15)
        self.assertFalse(list(long_temp.iterdir()))
        side.write_bytes(b"existing sidecar")
        p, alternate, _ = allocate()
        self.assertNotEqual(alternate, raw)
        self.stop(p, crash=False)
        self.assertEqual(side.read_bytes(), b"existing sidecar")
        self.assertFalse(list(self.a.glob("*.lease")))

    def test_copied_receipt(self):
        p = self.start(self.a, "copy")
        self.stop(p, crash=True)
        file = self.temp / "modavd-copy.tmp"
        receipt = self.a / (file.name + ".lease")
        receipt.rename(self.temp / "original-receipt")
        shutil.copyfile(self.temp / "original-receipt", receipt)
        self.recover(self.a)
        self.assertTrue(file.exists())
        self.assertTrue(receipt.exists())


if __name__ == "__main__":
    unittest.main()
