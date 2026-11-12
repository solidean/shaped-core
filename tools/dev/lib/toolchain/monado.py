"""The Monado `dev.py install monado` builds in ~/tools/monado, and how test runs find it.

The pin is `tools/monado.version`: one `<name> <commit>` line each for Monado and for the vcpkg its dependencies come from.

`test_env()` is what a test run adds, and it is deliberately not process-wide the way the Vulkan SDK's `activate()` is.
XR_RUNTIME_JSON decides which runtime an OpenXR application gets, so setting it for every dev.py run would send `dev.py example xr-cube` to the simulator rather than the headset.
"""

from __future__ import annotations

import os
from pathlib import Path

from ..core.layout import DRIVER_ROOT


def pin() -> dict[str, str]:
    """The pinned commit per name, the driver's own like the Vulkan SDK's."""
    lines = (DRIVER_ROOT / "tools" / "monado.version").read_text(encoding="utf-8").split("\n")
    return dict(line.split() for line in lines if line.strip())


def install_dir() -> Path:
    """Where `dev.py install monado` builds the pinned commit, a folder per commit."""
    return Path.home() / "tools" / "monado" / pin()["monado"][:12]


def manifest_of(install: Path) -> Path:
    """The runtime manifest a build of Monado leaves, which names its library by absolute path."""
    return install / "build" / "openxr_monado-dev.json"


def is_complete(install: Path) -> bool:
    """Whether `install` holds a runtime the loader can load, not merely the manifest of one."""
    library = install / "build" / "src" / "xrt" / "targets" / "openxr" / "openxr_monado.dll"
    return manifest_of(install).is_file() and library.is_file()


def env_for(manifest: Path | None, environ: dict[str, str] | os._Environ[str]) -> dict[str, str]:
    """What selects `manifest` as the simulated headset, or nothing where the environment chose a runtime already."""
    if manifest is None or environ.get("XR_RUNTIME_JSON"):
        return {}
    return {
        "XR_RUNTIME_JSON": str(manifest),
        "XRT_COMPOSITOR_NULL": "1",
        "SIMULATED_ENABLE": "1",
        "SIMULATED_LEFT": "simple",
        "SIMULATED_RIGHT": "simple",
    }


def test_env() -> dict[str, str]:
    """The environment a test binary gets, which opts shaped-xr's session tests in where Monado is installed."""
    install = install_dir()
    return env_for(manifest_of(install) if is_complete(install) else None, os.environ)
