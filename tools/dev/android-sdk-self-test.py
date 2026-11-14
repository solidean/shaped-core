#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for `dev.py install android-sdk`: the pin, what counts as installed, and unpacking.

The archives are large and licensed, so nothing here downloads one.
Unpacking is the part that fails quietly, since a Linux NDK with its symlinks flattened still looks installed.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/android-sdk-self-test.py [-v]
"""

from __future__ import annotations

import os
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.cmd.install_android_sdk import extract  # noqa: E402
from tools.dev.lib.toolchain import android_sdk as sdk  # noqa: E402


def _entry(name: str, mode: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name)
    info.external_attr = mode << 16
    return info


class Pin(unittest.TestCase):
    def test_every_host_gets_each_folder_once(self) -> None:
        for host in ("windows", "linux", "macosx"):
            folders = [a.folder for a in sdk.for_host(sdk.pin(), host)]
            self.assertEqual(len(folders), 4)
            self.assertEqual(len(set(folders)), 4)

    def test_a_platform_is_the_same_archive_everywhere(self) -> None:
        platforms = [a for a in sdk.for_host(sdk.pin(), "windows") if a.folder.startswith("platforms/")]
        self.assertEqual(platforms[0].host, "any")

    def test_a_checksum_is_a_sha1(self) -> None:
        for a in sdk.pin():
            self.assertEqual(len(a.sha1), 40)


class IsInstalled(unittest.TestCase):
    def test_every_folder_must_exist(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archives = sdk.parse_pin("a any a.zip 0\nb/c any b.zip 0\n")
            self.assertFalse(sdk.is_installed(root, archives))
            (root / "a").mkdir()
            self.assertFalse(sdk.is_installed(root, archives))
            (root / "b" / "c").mkdir(parents=True)
            self.assertTrue(sdk.is_installed(root, archives))

    def test_nothing_pinned_is_not_installed(self) -> None:
        self.assertFalse(sdk.is_installed(Path("."), []))


class Extract(unittest.TestCase):
    def _archive(self, tmp: Path, entries: dict[str, tuple[int, bytes]]) -> Path:
        path = tmp / "a.zip"
        with zipfile.ZipFile(path, "w") as z:
            for name, (mode, data) in entries.items():
                z.writestr(_entry(name, mode), data)
        return path

    def test_it_returns_the_one_folder(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            into = Path(tmp) / "out"
            into.mkdir()
            top = extract(self._archive(Path(tmp), {"tool/bin/a": (stat.S_IFREG | 0o644, b"a")}), into)
            self.assertEqual(top, into / "tool")
            self.assertEqual((top / "bin" / "a").read_bytes(), b"a")

    def test_two_top_folders_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            into = Path(tmp) / "out"
            into.mkdir()
            archive = self._archive(Path(tmp), {"a/x": (0, b""), "b/y": (0, b"")})
            with self.assertRaises(ValueError):
                extract(archive, into)

    @unittest.skipIf(os.name == "nt", "Windows has neither executable bits nor unprivileged symlinks")
    def test_executable_bits_and_symlinks_survive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            into = Path(tmp) / "out"
            into.mkdir()
            top = extract(self._archive(Path(tmp), {"t/clang-21": (stat.S_IFREG | 0o755, b"x"),
                                                    "t/clang": (stat.S_IFLNK | 0o777, b"clang-21")}), into)
            self.assertTrue(os.access(top / "clang-21", os.X_OK))
            self.assertTrue((top / "clang").is_symlink())


if __name__ == "__main__":
    unittest.main()
