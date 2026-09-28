#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Self-test for deps_manifest's host resolution: which upstream is available on which machine.

This is the one manifest question a wrong answer does not report.
An upstream declared available where it publishes nothing is fetched anyway, and the archive — built for another
instruction set — installs cleanly, so CMake reads `.install/` as "the dependency is here" and the first sign of
trouble is the linker refusing it several minutes later.

Availability is asserted against the REAL manifests rather than fixtures, so these cases pin what `extern/` actually
declares today and fail when someone adds a platform without adding its asset.

Run directly, or through `uv run dev.py check` as part of `dev-selftest`.
"""

from __future__ import annotations

import sys
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

import yaml

EXTERN = Path(__file__).resolve().parent
sys.path.insert(0, str(EXTERN))
import deps_manifest  # noqa: E402

HOSTS = [(os_key, arch) for os_key in ("windows", "linux", "macos") for arch in ("x64", "arm64")]

failures: list[str] = []


def check(what: str, got: object, want: object) -> None:
    if got != want:
        failures.append(f"{what}: got {got!r}, want {want!r}")


def availability(directory: Path) -> dict[tuple[str, str], bool]:
    """Whether the upstream in `directory` is available on each host, with the host functions stubbed per case."""
    real_os = deps_manifest.host_os_key
    real_arch = deps_manifest.host_arch_key
    try:
        out: dict[tuple[str, str], bool] = {}
        for os_key, arch in HOSTS:
            deps_manifest.host_os_key = lambda key=os_key: key
            deps_manifest.host_arch_key = lambda key=arch: key
            out[(os_key, arch)] = deps_manifest.one(directory).is_available
        return out
    finally:
        deps_manifest.host_os_key = real_os
        deps_manifest.host_arch_key = real_arch


def test_arch_key_covers_both_spellings() -> None:
    # Linux says aarch64 where Windows and macOS say arm64, and all three mean the same key.
    check("host_arch_key spellings", sorted(set(deps_manifest.HOST_ARCH_KEYS)), ["arm64", "x64"])
    check("host_keys shape", len(deps_manifest.host_keys()), 2)
    check("host_keys is os-first", deps_manifest.host_keys()[0], deps_manifest.host_os_key())


@contextmanager
def on_machine(machine: str, os_key: str = "linux") -> Iterator[None]:
    """Run the body as though `platform.machine()` said `machine` on an `os_key` host."""
    real_machine = deps_manifest.platform.machine
    real_os = deps_manifest.host_os_key
    try:
        deps_manifest.platform.machine = lambda: machine
        deps_manifest.host_os_key = lambda: os_key
        yield
    finally:
        deps_manifest.platform.machine = real_machine
        deps_manifest.host_os_key = real_os


def test_machine_spellings_map_onto_arch_keys() -> None:
    # Windows reports AMD64, Linux x86_64 and aarch64, macOS arm64; anything else names no arch key at all.
    for machine, want in (("AMD64", "x64"), ("x86_64", "x64"), ("aarch64", "arm64"), ("arm64", "arm64"),
                          ("riscv64", None), ("ppc64le", None), ("i686", None)):
        with on_machine(machine):
            check(f"host_arch_key on {machine}", deps_manifest.host_arch_key(), want)
            want_keys = ["linux"] if want is None else ["linux", f"linux-{want}"]
            check(f"host_keys on {machine}", deps_manifest.host_keys(), want_keys)


def test_unknown_machine_is_refused_by_a_per_arch_upstream() -> None:
    # An upstream that ships per machine on this OS ships nothing for a machine it never names.
    # Reading riscv64 as x64 would install an x86_64 archive there, which is what this case pins shut.
    for machine in ("riscv64", "ppc64le", "i686"):
        with on_machine(machine):
            check(f"OIDN on linux-{machine}", deps_manifest.one(EXTERN / "oidn").is_available, False)
            check(f"DXC on linux-{machine}", deps_manifest.one(EXTERN / "dxc").is_available, False)
    with on_machine("x86_64"):
        check("OIDN on linux-x86_64", deps_manifest.one(EXTERN / "oidn").is_available, True)
        check("DXC on linux-x86_64", deps_manifest.one(EXTERN / "dxc").is_available, True)
    with on_machine("aarch64"):
        check("OIDN on linux-aarch64", deps_manifest.one(EXTERN / "oidn").is_available, False)
    with on_machine("AMD64", os_key="windows"):
        check("OIDN on windows-AMD64", deps_manifest.one(EXTERN / "oidn").is_available, True)


def test_unknown_machine_keeps_an_upstream_without_arch_keys() -> None:
    # A list naming only other OSes, or none at all, says nothing about machines, so an unknown one stays available.
    with on_machine("riscv64"):
        check("macos-only list on linux-riscv64", deps_manifest.one(_manifest_with(["macos"])).is_available, True)
        check("windows-arm64 list on linux-riscv64",
              deps_manifest.one(_manifest_with(["windows-arm64"])).is_available, True)
        check("bare linux on linux-riscv64", deps_manifest.one(_manifest_with(["linux"])).is_available, False)


def test_oidn_matches_the_published_assets() -> None:
    # Upstream publishes x64 Windows, x86_64 Linux and arm64 macOS, and nothing else.
    got = availability(EXTERN / "oidn")
    want = {("windows", "x64"): True, ("windows", "arm64"): False,
            ("linux", "x64"): True, ("linux", "arm64"): False,
            ("macos", "x64"): False, ("macos", "arm64"): True}
    check("OIDN availability", got, want)


def test_dxc_matches_the_published_assets() -> None:
    # The Windows zip carries both architectures; the Linux tarball is x86_64 only; macOS has no release at all.
    got = availability(EXTERN / "dxc")
    want = {("windows", "x64"): True, ("windows", "arm64"): True,
            ("linux", "x64"): True, ("linux", "arm64"): False,
            ("macos", "x64"): False, ("macos", "arm64"): False}
    check("DXC availability", got, want)


def _manifest_with(unavailable_on: list[str]) -> Path:
    directory = Path(tempfile.mkdtemp())
    entry = {"name": "Probe", "source": "git", "track": "tags", "digest_algo": "sha256",
             "pin_hash": "0" * 40, "license": "MIT", "license_text": "probe",
             "unavailable_on": unavailable_on}
    (directory / "dependency.yml").write_text(yaml.safe_dump({"upstreams": [entry]}), encoding="utf-8")
    return directory


def test_bare_os_key_covers_every_architecture() -> None:
    # An upstream that ships nothing for a platform names it once, rather than once per machine.
    got = availability(_manifest_with(["linux"]))
    check("bare key on linux-x64", got[("linux", "x64")], False)
    check("bare key on linux-arm64", got[("linux", "arm64")], False)
    check("bare key leaves windows alone", got[("windows", "x64")], True)


def test_unknown_key_is_refused() -> None:
    # A key nobody recognises would silently mean "available everywhere", which is the failure this whole file guards.
    # The message is asserted too, so an unrelated manifest error cannot pass for this refusal.
    for spelling in ("windows_arm64", "win-arm64", "linux-aarch64", "arm64"):
        try:
            deps_manifest.one(_manifest_with([spelling]))
            failures.append(f"unavailable_on {spelling!r} was accepted, but names no host")
        except ValueError as error:
            if "unavailable_on" not in str(error):
                failures.append(f"unavailable_on {spelling!r} was refused for another reason: {error}")


def main() -> int:
    for name, case in sorted(globals().items()):
        if name.startswith("test_"):
            case()
    if failures:
        print("manifest-self-test FAILED")
        for line in failures:
            print(f"  {line}")
        return 1
    print("manifest-self-test: OK (host resolution, unknown machines, availability across 6 hosts, key validation)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
