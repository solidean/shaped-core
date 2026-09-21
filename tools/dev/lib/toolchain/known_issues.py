"""Known external issues: something on this machine we already know misbehaves under our code.

Each one is diagnosed in its own docs/bugs-external/<name>/ folder, and an entry here is how a machine that has it finds out.
Probes are cheap by contract — an environment variable, a directory name, never a subprocess — since every command that runs our code asks first.
That puts the warning ahead of the failure it explains, rather than after an hour of chasing it.
`doctor` lists every entry it can answer for, affected or not.
Every command that runs our binaries prints the active ones, once per invocation.
"""

from __future__ import annotations

import os
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from ..core import console, ui
from ..core.models import Preset

# A doctor row's `ok` for a known issue this machine has: yellow, and never a failure of doctor itself.
WARN = "warn"

# Preset name fragments for cross-targets, which never run a native graphics driver.
_NON_NATIVE = ("wasm", "emscripten", "web", "android", "ios")


@dataclass(frozen=True)
class ProbeResult:
    affected: bool
    detail: str  # what the probe saw; for an affected machine, also what to do about it


@dataclass(frozen=True)
class KnownIssue:
    label: str  # the doctor row's label
    doc: str  # the docs/bugs-external folder that diagnoses it
    applies_to: Callable[[Preset], bool]
    probe: Callable[[], ProbeResult | None]  # None when this machine cannot say, which reports nothing


def _is_native(preset: Preset) -> bool:
    return not any(tag in preset.name.lower() for tag in _NON_NATIVE)


def _vulkan_sdk_version() -> tuple[int, int, int] | None:
    """$VULKAN_SDK's version, read from the install directory's name after resolving symlinks such as a `latest` one.

    On Windows VULKAN_SDK names that directory; on Linux and macOS it names a platform subdirectory of it (`1.4.357.1/x86_64`).
    """
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        return None
    resolved = Path(sdk).resolve()
    for name in (resolved.name, resolved.parent.name):
        parts = name.split(".")
        if len(parts) >= 3 and all(p.isdigit() for p in parts[:3]):
            return (int(parts[0]), int(parts[1]), int(parts[2]))
    return None


# The first SDK whose validation layer no longer drops a deferred submit batch unvalidated.
_VULKAN_SDK_FIXED = (1, 4, 350)


def _probe_vulkan_wait_before_signal() -> ProbeResult | None:
    version = _vulkan_sdk_version()
    if version is None:
        return None
    shown = ".".join(str(v) for v in version)
    if version >= _VULKAN_SDK_FIXED:
        return ProbeResult(False, f"SDK {shown}")
    wanted = ".".join(str(v) for v in _VULKAN_SDK_FIXED)
    return ProbeResult(
        True,
        f"SDK {shown}'s validation layer mishandles a submit that waits on a timeline value not yet signalled, "
        f"and reports hazards and image layouts our transfers never produce — install SDK {wanted} or newer "
        f"and point VULKAN_SDK at it",
    )


ISSUES: tuple[KnownIssue, ...] = (
    KnownIssue(
        label="vulkan validation layer",
        doc="docs/bugs-external/vulkan-syncval-wait-before-signal-false-race",
        applies_to=_is_native,
        probe=_probe_vulkan_wait_before_signal,
    ),
)


def doctor_rows() -> list[tuple[str, bool | None | str, str]]:
    """One row per issue this machine can answer for: OK when unaffected, WARN when affected."""
    rows: list[tuple[str, bool | None | str, str]] = []
    for issue in ISSUES:
        result = issue.probe()
        if result is None:
            continue
        if result.affected:
            rows.append((issue.label, WARN, f"{result.detail} (see {issue.doc})"))
        else:
            rows.append((issue.label, True, result.detail))
    return rows


_warned: set[str] = set()


def warn_active(presets: list[Preset]) -> None:
    """Print one warning per known issue that affects a run under any of `presets`, once per process."""
    for issue in ISSUES:
        if issue.label in _warned or not any(issue.applies_to(p) for p in presets):
            continue
        result = issue.probe()
        if result is None or not result.affected:
            continue
        _warned.add(issue.label)
        ui.write_line(console.yellow(f"WARNING: known issue, {issue.label}: {result.detail} (see {issue.doc})"))
