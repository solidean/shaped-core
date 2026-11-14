#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/lib/toolchain/monado.py — what a test run adds to select the simulated headset.

The failure this exists for is quiet: an environment that names the simulator for the wrong run sends an application to a fake headset, or a person's chosen runtime gets overridden.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/monado-self-test.py [-v]
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.toolchain import monado as mo  # noqa: E402


class EnvFor(unittest.TestCase):
    def test_nothing_installed_adds_nothing(self) -> None:
        self.assertEqual(mo.env_for(None, {}), {})

    def test_an_install_selects_the_simulated_headset(self) -> None:
        env = mo.env_for(Path("m.json"), {})
        self.assertEqual(env["XR_RUNTIME_JSON"], "m.json")
        self.assertEqual(env["XRT_COMPOSITOR_NULL"], "1")
        self.assertEqual(env["SIMULATED_ENABLE"], "1")
        self.assertEqual(env["SIMULATED_LEFT"], "simple")
        self.assertEqual(env["SIMULATED_RIGHT"], "simple")

    def test_a_chosen_runtime_wins(self) -> None:
        self.assertEqual(mo.env_for(Path("m.json"), {"XR_RUNTIME_JSON": "link.json"}), {})


class IsComplete(unittest.TestCase):
    def test_a_manifest_without_its_library_is_not_an_install(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            install = Path(tmp)
            mo.manifest_of(install).parent.mkdir(parents=True)
            mo.manifest_of(install).write_text("{}", encoding="utf-8")
            self.assertFalse(mo.is_complete(install))

            library = install / "build" / "src" / "xrt" / "targets" / "openxr" / "openxr_monado.dll"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"")
            self.assertTrue(mo.is_complete(install))


class Pin(unittest.TestCase):
    def test_both_commits_are_pinned_in_full(self) -> None:
        pinned = mo.pin()
        self.assertEqual(len(pinned["monado"]), 40)
        self.assertEqual(len(pinned["vcpkg"]), 40)


if __name__ == "__main__":
    unittest.main()
