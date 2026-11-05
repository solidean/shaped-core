"""The Vulkan SDK `dev.py install vulkan-sdk` puts in ~/tools/vulkan-sdk, and how every dev.py run finds it.

The pin is `tools/vulkan-sdk.version`: the version, then one `<kit> <sha256>` line per kit LunarG publishes for it.
An installed SDK is the folder named after its version, which is also what known_issues reads a version from.

`activate()` makes the pinned install what the build and the tests see, unless VULKAN_SDK already names an SDK:
an SDK the user chose wins over the one dev.py installed.
"""

from __future__ import annotations

import os
import platform
from pathlib import Path

from ..core.layout import DRIVER_ROOT

# LunarG's kit name per system; there is no Windows-on-ARM or Linux-on-ARM kit to pin.
KITS = {"Windows": "windows", "Linux": "linux"}


def pin() -> tuple[str, dict[str, str]]:
    """The pinned version, and the SHA-256 of its installer per kit.

    The driver's pin, like deno's: tools/ of shaped-core, also when a consumer runs it.
    """
    lines = (DRIVER_ROOT / "tools" / "vulkan-sdk.version").read_text(encoding="utf-8").split("\n")
    lines = [line.strip() for line in lines if line.strip()]
    return lines[0], dict(line.split() for line in lines[1:])


def default_root() -> Path:
    """Where `dev.py install vulkan-sdk` installs, one folder per version."""
    return Path.home() / "tools" / "vulkan-sdk"


def sdk_dir(version: str) -> Path:
    """What VULKAN_SDK names for an install of `version`.

    On Windows that is the install folder itself; on Linux it is the kit's platform folder inside it.
    """
    base = default_root() / version
    return base / "x86_64" if platform.system() == "Linux" else base


def is_complete(sdk: Path) -> bool:
    """Whether `sdk` holds what FindVulkan looks for: the header and the loader's import library."""
    if platform.system() == "Windows":
        return (sdk / "Include" / "vulkan" / "vulkan.h").is_file() and (sdk / "Lib" / "vulkan-1.lib").is_file()
    return (sdk / "include" / "vulkan" / "vulkan.h").is_file() and any((sdk / "lib").glob("libvulkan.so*"))


def installed_sdk_dir() -> Path | None:
    """The pinned SDK when it is installed and complete, else None."""
    version, _ = pin()
    sdk = sdk_dir(version)
    return sdk if is_complete(sdk) else None


def _prepend(var: str, entry: Path) -> None:
    existing = os.environ.get(var, "")
    os.environ[var] = os.pathsep.join([str(entry)] + ([existing] if existing else []))


def activate() -> Path | None:
    """Point this process, and every child it starts, at the pinned SDK; returns it, or None when nothing changed.

    Nothing changes when VULKAN_SDK is set already, or when the pinned SDK is not installed.
    On Linux VK_ADD_LAYER_PATH is what finds the validation layer, since an unpacked tarball registers no layer.
    Windows needs none: the installer runs elevated and registers its layers, and naming them again makes the loader report each one twice.
    """
    if os.environ.get("VULKAN_SDK"):
        return None
    sdk = installed_sdk_dir()
    if sdk is None:
        return None
    os.environ["VULKAN_SDK"] = str(sdk)
    if platform.system() == "Windows":
        _prepend("PATH", sdk / "Bin")
    else:
        _prepend("PATH", sdk / "bin")
        _prepend("LD_LIBRARY_PATH", sdk / "lib")
        _prepend("VK_ADD_LAYER_PATH", sdk / "share" / "vulkan" / "explicit_layer.d")
    return sdk
