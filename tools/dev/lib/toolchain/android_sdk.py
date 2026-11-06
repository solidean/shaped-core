"""The Android SDK `dev.py install android-sdk` puts in ~/tools/android-sdk, and how every dev.py run finds it.

The pin is `tools/android-sdk.version`, one line per archive: `<folder in the SDK> <host> <archive> <sha1>`.
The checksums are Google's own, from its repository manifest, which publishes SHA-1 and nothing stronger.

`activate()` makes the install what the Android presets see, unless ANDROID_NDK_ROOT already names an NDK.
"""

from __future__ import annotations

import os
import platform
from dataclasses import dataclass
from pathlib import Path

from ..core.layout import DRIVER_ROOT

_HOSTS = {"Windows": "windows", "Linux": "linux", "Darwin": "macosx"}


@dataclass(frozen=True)
class archive:
    folder: str
    host: str
    file: str
    sha1: str


def parse_pin(text: str) -> list[archive]:
    return [archive(*line.split()) for line in text.split("\n") if line.strip()]


def pin() -> list[archive]:
    """Every pinned archive, the driver's own like the Vulkan SDK's."""
    return parse_pin((DRIVER_ROOT / "tools" / "android-sdk.version").read_text(encoding="utf-8"))


def for_host(archives: list[archive], host: str) -> list[archive]:
    return [a for a in archives if a.host in (host, "any")]


def host_name() -> str | None:
    """The manifest's name for this system, or None where Google publishes no SDK for it."""
    return _HOSTS.get(platform.system())


def default_root() -> Path:
    """Where `dev.py install android-sdk` installs: the SDK's own layout, so ANDROID_HOME can name it."""
    return Path.home() / "tools" / "android-sdk"


def is_installed(root: Path, archives: list[archive]) -> bool:
    """Whether every archive's folder exists, which each only does once it is whole: they arrive by rename."""
    return bool(archives) and all((root / a.folder).is_dir() for a in archives)


def installed_root() -> Path | None:
    """The SDK when all of this host's pinned archives are in it, else None."""
    host = host_name()
    if host is None:
        return None
    return default_root() if is_installed(default_root(), for_host(pin(), host)) else None


def activate() -> Path | None:
    """Point this process, and every child it starts, at the pinned SDK; returns it, or None when nothing changed.

    Nothing changes when ANDROID_NDK_ROOT is set already, or when the SDK is not installed.
    """
    if os.environ.get("ANDROID_NDK_ROOT"):
        return None
    root = installed_root()
    if root is None:
        return None
    ndk = next(a for a in pin() if a.folder.startswith("ndk/"))
    os.environ["ANDROID_NDK_ROOT"] = str(root / ndk.folder)
    os.environ.setdefault("ANDROID_HOME", str(root))
    return root
