# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Run in an x64 VS developer shell: python test_owner.py <patched qemu tree>."""
import ctypes
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
                        str(SOURCE / "windows-temp-images.cpp"), "/Fe:" + str(cls.exe)],
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

    def start(self, avd, tag):
        p = subprocess.Popen([self.exe, avd, self.temp, tag, "hold"],
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
        receipt = self.temp / "modavd-no-proof.tmp.lease"
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

    def test_copied_receipt(self):
        p = self.start(self.a, "copy")
        self.stop(p, crash=True)
        file = self.temp / "modavd-copy.tmp"
        receipt = file.with_suffix(".tmp.lease")
        receipt.rename(self.temp / "original-receipt")
        shutil.copyfile(self.temp / "original-receipt", receipt)
        self.recover(self.a)
        self.assertTrue(file.exists())
        self.assertTrue(receipt.exists())


if __name__ == "__main__":
    unittest.main()
