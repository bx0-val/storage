"""Integration checks for storage data, filesystem edge cases, and terminal use."""

import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest


STORAGE = Path(os.environ.get("STORAGE_BIN", str(Path(__file__).resolve().parent / "build/storage")))


def run(*args, **kwargs):
    return subprocess.run([str(STORAGE), *map(str, args)],
                          capture_output=True, text=True, timeout=15, **kwargs)


class Terminal:
    def __init__(self, *args):
        self.master, self.slave = pty.openpty()
        self.before = termios.tcgetattr(self.slave)
        self.resize(100, 32)
        environment = {**os.environ, "TERM": "xterm-256color"}
        environment.pop("NO_COLOR", None)
        self.child = subprocess.Popen([str(STORAGE), *map(str, args)],
                                      stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                      env=environment, start_new_session=True)
        self.output = b""

    def resize(self, width, height):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        if hasattr(self, "child"):
            self.child.send_signal(signal.SIGWINCH)

    def wait_for(self, needle, timeout=8):
        deadline = time.monotonic() + timeout
        needle = needle.encode()
        while needle not in self.output and time.monotonic() < deadline:
            if select.select([self.master], [], [], 0.1)[0]:
                self.output += os.read(self.master, 65536)
            if self.child.poll() is not None:
                break
        if needle not in self.output:
            raise AssertionError(f"Did not render {needle!r}: {self.output[-5000:]!r}")

    def send(self, keys):
        self.output = b""
        os.write(self.master, keys.encode())

    def assert_restored(self, testcase, code=0):
        testcase.assertEqual(self.child.wait(timeout=3), code)
        testcase.assertEqual(termios.tcgetattr(self.slave), self.before)

    def close(self):
        if self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=3)
        os.close(self.master)
        os.close(self.slave)


class StorageTest(unittest.TestCase):
    def test_filesystem_numbers_and_virtual_filter(self):
        result = run("--json", check=True)
        data = json.loads(result.stdout)
        root = next(item for item in data["filesystems"] if item["mount"] == "/")
        live = os.statvfs("/")
        self.assertEqual(root["total"], live.f_blocks * live.f_frsize)
        self.assertEqual(root["inodes"], live.f_files)
        self.assertAlmostEqual(root["percent"], 100 * root["used"] / (root["used"] + root["available"]))
        self.assertFalse(data["warnings"])
        all_data = json.loads(run("--all", "--json", check=True).stdout)
        self.assertGreaterEqual(len(all_data["filesystems"]), len(data["filesystems"]))
        self.assertFalse(any(item["type"] == "tmpfs" for item in data["filesystems"]))

    def test_folder_accounting_and_unusual_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "nested").mkdir()
            payload = directory / "nested/payload"
            payload.write_bytes(b"x" * 8192)
            os.link(payload, directory / "hardlink")
            unusual = "hidden\tname\nwith spaces"
            (directory / unusual).write_bytes(b"x" * 4096)
            (directory / ".hidden").write_text("included")
            (directory / "link").symlink_to("/etc")
            with (directory / "sparse").open("wb") as handle:
                handle.truncate(1024**3)
            report = json.loads(run("--json", directory, check=True).stdout)["directory"]
            expected = int(subprocess.check_output(["du", "-sx", "-B1", "--", str(directory)]).split()[0])
            self.assertEqual(report["total"], expected)
            self.assertFalse(report["warning"])
            entries = {item["name"]: item for item in report["entries"]}
            self.assertIn(unusual, entries)
            self.assertIn(".hidden", entries)
            self.assertTrue(entries["nested"]["directory"])
            self.assertFalse(entries["link"]["directory"])
            self.assertLess(entries["sparse"]["size"], 1024**3)
            sizes = [item["size"] for item in report["entries"]]
            self.assertEqual(sizes, sorted(sizes, reverse=True))

    @unittest.skipIf(os.geteuid() == 0, "root can bypass the permission fixture")
    def test_partial_scan_is_reported(self):
        with tempfile.TemporaryDirectory() as temporary:
            locked = Path(temporary) / "locked"
            locked.mkdir()
            (locked / "file").write_text("cannot read")
            locked.chmod(0)
            try:
                report = json.loads(run("--json", temporary, check=True).stdout)["directory"]
                self.assertIn("Partial scan:", report["warning"])
            finally:
                locked.chmod(0o700)

    def test_pipes_and_invalid_arguments(self):
        text = run(check=True).stdout
        self.assertIn("FILESYSTEMS", text)
        self.assertIn("DISKS & PARTITIONS", text)
        self.assertNotIn("\x1b", text)
        for args in (("--interval", "nan"), ("--interval", "0"), ("--interval", "inf"),
                     ("/does-not-exist-storage-test",), ("/etc/passwd",)):
            result = run(*args)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("Traceback", result.stderr)

    def test_no_external_tools_are_required(self):
        with tempfile.TemporaryDirectory() as empty_path:
            result = run("--json", env={**os.environ, "PATH": empty_path}, check=True)
            self.assertTrue(json.loads(result.stdout)["filesystems"])

    def test_terminal_views_navigation_resize_and_quit(self):
        with tempfile.TemporaryDirectory(prefix="storage-tui-") as temporary:
            directory = Path(temporary)
            (directory / "child").mkdir()
            (directory / "child/payload").write_bytes(b"x" * 8192)
            terminal = Terminal(directory)
            try:
                terminal.wait_for("Cached:")
                terminal.send("\n")
                terminal.wait_for("payload")
                terminal.send("h")
                terminal.wait_for("child/")
                terminal.send("1")
                terminal.wait_for("MOUNTED FILESYSTEMS")
                terminal.send("2")
                terminal.wait_for("DISKS & PARTITIONS")
                terminal.send("?")
                terminal.wait_for("Switch views")
                terminal.send("?")
                terminal.output = b""
                terminal.resize(45, 12)
                terminal.wait_for("Resize to at least")
                terminal.resize(80, 24)
                terminal.wait_for("DISKS & PARTITIONS")
                terminal.send("q")
                terminal.assert_restored(self)
            finally:
                terminal.close()

    def test_interrupt_restores_terminal(self):
        terminal = Terminal()
        try:
            terminal.wait_for("MOUNTED FILESYSTEMS")
            terminal.child.send_signal(signal.SIGINT)
            terminal.assert_restored(self, 130)
        finally:
            terminal.close()

    def test_quit_cancels_parallel_scan(self):
        terminal = Terminal(Path.home())
        try:
            terminal.wait_for("Scanning:")
            self.assertIsNone(terminal.child.poll())
            started = time.monotonic()
            terminal.send("q")
            terminal.assert_restored(self)
            self.assertLess(time.monotonic() - started, 1)
        finally:
            terminal.close()

    def test_parallel_accounting_utf8_and_empty_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root / "empty").mkdir()
            (root / "世界_💾").write_bytes(b"x"*8192)
            (root / "\x1b[31mescape").write_text("safe display")
            for i in range(60):
                folder=root / f"folder-{i}"
                folder.mkdir()
                for j in range(8):
                    (folder / str(j)).write_bytes(b"x"*4096)
                os.link(root / "世界_💾", folder / "shared")
            expected=int(subprocess.check_output(["du","-sx","-B1",str(root)]).split()[0])
            for jobs in (1,4,8):
                report=json.loads(run("--json","--jobs",jobs,root,check=True).stdout)["directory"]
                self.assertEqual(report["total"],expected)
                self.assertEqual(report["path"],str(root))
                self.assertTrue(report["complete"])
                self.assertFalse(report["warning"])
                self.assertIn("世界_💾",{e["name"] for e in report["entries"]})
            text=run("--plain",root,check=True).stdout
            self.assertNotIn("\x1b",text)

    def test_cached_navigation_does_not_rescan(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/"child").mkdir()
            (root/"child/original").write_text("cached")
            terminal=Terminal(root)
            try:
                terminal.wait_for("Cached:")
                (root/"child/new-after-scan").write_text("requires refresh")
                terminal.send("\n")
                terminal.wait_for("original")
                self.assertNotIn(b"new-after-scan",terminal.output)
                terminal.send("r")
                terminal.wait_for("new-after-scan")
                terminal.send("q")
                terminal.assert_restored(self)
            finally:
                terminal.close()

    def test_sigterm_restores_terminal(self):
        terminal=Terminal()
        try:
            terminal.wait_for("MOUNTED FILESYSTEMS")
            terminal.child.terminate()
            terminal.assert_restored(self,143)
        finally:
            terminal.close()

if __name__ == "__main__":
    unittest.main(verbosity=2)
