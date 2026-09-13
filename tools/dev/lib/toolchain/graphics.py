"""Graphics-environment checks: what the sg backends and sr::window need beyond the core toolchain.

Every check here is advisory, never a failure.
The repo configures, builds and passes its whole suite with none of it — the graphics libraries degrade instead of disappearing, which is exactly what makes a gap invisible.
A checkout with no windowing headers still builds an `sr::window_system::try_create` that always fails, and a Vulkan swapchain that reports the platform unsupported.
Nothing says so until an example draws nothing.
Naming that cost is the whole point of these lines.

Each check mirrors one CMake gate, and the mirror must stay exact:
`find_package(Vulkan)` in libs/graphics/shaped-graphics/CMakeLists.txt, the `check_include_file` probes in
libs/graphics/shaped-graphics/backends/vulkan/CMakeLists.txt and extern/sdl3/CMakeLists.txt, and the `.install/pin.txt` fetch markers.

The per-backend roll-up comes first and the ingredient probes follow it as the evidence.
The roll-up re-probes nothing — it reads the verdicts the probes already produced, so it cannot disagree with the lines printed under it.

Backend availability is a build-time question plus a run-time one, and both are reported: a backend can compile in and still create no device.
What is deliberately not answered is ray-tracing capability, which needs a real device and a `CheckFeatureSupport` call, well past what belongs on a doctor's fast path.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
import subprocess
from pathlib import Path

from ..project import pins

# The windowing systems a Linux swapchain can present to, each with the header whose presence turns it on.
# The names are sg::window_platform's, since that is what a failure reports back.
_SURFACE_HEADERS = (("xlib", "X11/Xlib.h"), ("xcb", "xcb/xcb.h"), ("wayland", "wayland-client.h"))

# What SDL3 needs to configure at all — a subset, because SDL has no XCB video driver.
_SDL_HEADERS = ("X11/Xlib.h", "wayland-client.h")

_INSTALL_HINT = (
    "install the X11 and wayland development headers "
    "(https://wiki.libsdl.org/SDL3/README-linux#build-dependencies)"
)


def _is_unix_windowed() -> bool:
    """Whether this is a platform whose surfaces come from X11 or wayland, rather than from Win32 or Metal."""
    return platform.system() not in ("Windows", "Darwin", "")


def _vulkan_include_dirs() -> list[Path]:
    """The include directories FindVulkan adds on top of the compiler's own search path.

    Only `$VULKAN_SDK`, and both spellings of its include dir, because that is all FindVulkan itself contributes:
    a distro `vulkan-headers` package is found through the default search path instead.
    """
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        return []
    return [d for d in (Path(sdk) / "include", Path(sdk) / "Include") if d.is_dir()]


def _header_reachable(cxx: str, header: str, include_dirs: list[Path] | None = None) -> bool | None:
    """Whether `#include <header>` compiles, the way CMake's check_include_file asks it.

    Returns None when the question could not be put to the compiler at all, which must not read as "missing".
    """
    cmd = [cxx, "-x", "c++", "-fsyntax-only"]
    for d in include_dirs or []:
        cmd += ["-I", str(d)]
    cmd.append("-")
    try:
        out = subprocess.run(cmd, input=f"#include <{header}>\n", capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.returncode == 0


def _vulkan_headers_check(cxx: str | None) -> tuple[str, bool | None, str]:
    label = "vulkan headers"
    if cxx is None:
        return (label, None, "no compiler resolved, so the header probe could not run")

    dirs = _vulkan_include_dirs()
    reachable = _header_reachable(cxx, "vulkan/vulkan.h", dirs)
    if reachable is None:
        return (label, None, "the header probe could not run")

    sdk = os.environ.get("VULKAN_SDK")
    if reachable:
        where = f"VULKAN_SDK={sdk}" if dirs else "the compiler's default include path"
        return (label, True, f"vulkan/vulkan.h via {where}")
    hint = "install the Vulkan SDK (https://vulkan.lunarg.com) and set VULKAN_SDK"
    if sdk:
        hint = f"VULKAN_SDK={sdk} has no include/vulkan/vulkan.h — {hint}"
    return (label, None, f"vulkan/vulkan.h not found, so the vulkan backend is skipped at configure; {hint}")


def _vulkan_icd_dirs() -> list[Path]:
    """Where the loader looks for driver manifests, in the order it reads them.

    An explicit VK_DRIVER_FILES / VK_ICD_FILENAMES overrides the search entirely, so it is not merged with the rest.
    """
    override = os.environ.get("VK_DRIVER_FILES") or os.environ.get("VK_ICD_FILENAMES")
    if override:
        return [Path(p) for p in override.split(os.pathsep) if p]

    dirs = [Path("/etc/vulkan/icd.d"), Path("/usr/local/share/vulkan/icd.d"), Path("/usr/share/vulkan/icd.d")]
    home = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    dirs.append(Path(home) / "vulkan" / "icd.d")
    return dirs


def _vulkan_runtime_check() -> tuple[str, bool | None, str]:
    """Whether a Vulkan *device* can be created here, which the header check says nothing about.

    Headers are a build-time fact and the loader plus an installed driver are a run-time one, and a container or a
    headless CI box routinely has the first without the second.
    That gap surfaces as `create_vulkan_context` failing with no graphics device, far from its cause.
    """
    label = "vulkan runtime"
    system = platform.system()
    soname = {"Windows": "vulkan-1", "Darwin": "vulkan"}.get(system, "vulkan")
    loader = ctypes.util.find_library(soname)
    if loader is None:
        return (label, None, f"no Vulkan loader found — install the runtime (lib{soname}) for a GPU device")

    if system != "Linux":
        # Windows enumerates drivers through the registry and macOS ships MoltenVK inside the SDK, so neither has a
        # manifest directory worth counting; the loader is as far as a cheap check goes.
        return (label, True, f"loader {loader}")

    dirs = _vulkan_icd_dirs()
    # By driver rather than by manifest: a distro ships one per architecture, and "radeon, intel" is the fact worth
    # reading where "radeon_icd.i686.json, radeon_icd.x86_64.json, ..." is six lines of the same answer.
    drivers = sorted({m.name.split("_icd")[0] for d in dirs for m in d.glob("*.json")})
    if not drivers:
        return (label, None,
                f"loader {loader}, but no ICD manifest in {', '.join(str(d) for d in dirs)} "
                f"— install a driver (mesa, or the vendor's) for a GPU device")
    return (label, True, f"loader {loader}, driver(s): {', '.join(drivers)}")


# D3D_FEATURE_LEVEL_11_0, the level the dx12 backend's device creation asks for.
_D3D_FEATURE_LEVEL_11_0 = 0xB000


class _GUID(ctypes.Structure):
    _fields_ = [("d1", ctypes.c_uint32), ("d2", ctypes.c_uint16), ("d3", ctypes.c_uint16),
                ("d4", ctypes.c_ubyte * 8)]


# IID_ID3D12Device — the riid D3D12CreateDevice is asked for.
_IID_ID3D12DEVICE = _GUID(0x189819F1, 0x1DB6, 0x4B57, (ctypes.c_ubyte * 8)(0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7))


def _dx12_runtime_check() -> tuple[str, bool | None, str]:
    """Whether a D3D12 device can be created here, which the build gate says nothing about.

    dx12 builds on every Windows checkout, so the only thing left to establish is whether the machine has an adapter that
    supports it — a headless VM or a stripped container routinely does not, and that surfaces as `create_dx12_context`
    failing far from its cause.
    This is the dx12 half of what `_vulkan_runtime_check` answers for vulkan.

    A null `ppDevice` is the documented support probe: it returns S_FALSE when the device *would* be created and creates
    nothing, so nothing has to be released afterwards.
    """
    label = "dx12 runtime"
    if platform.system() != "Windows":
        return (label, None, "no D3D12 outside Windows")
    try:
        d3d12 = ctypes.WinDLL("d3d12")
    except OSError:
        return (label, None, "d3d12.dll not loadable — no D3D12 runtime on this machine")
    try:
        hr = d3d12.D3D12CreateDevice(None, _D3D_FEATURE_LEVEL_11_0, ctypes.byref(_IID_ID3D12DEVICE), None)
    except (AttributeError, OSError) as e:
        return (label, None, f"the D3D12CreateDevice probe could not run ({e})")
    # S_OK and S_FALSE both mean supported; only a negative HRESULT is a real "no adapter".
    if hr >= 0:
        return (label, True, "a D3D12 feature-level 11_0 adapter is present")
    return (label, None, f"no D3D12 adapter at feature level 11_0 (HRESULT 0x{hr & 0xFFFFFFFF:08X}) — "
                         f"dx12 compiles in, but creating a context will fail")


def _surface_check(cxx: str | None) -> tuple[str, bool | None, str]:
    """Which windowing systems a Vulkan swapchain can present to, mirroring the backend's own header probes.

    A build with none of them still presents headlessly, so the cost is precisely a *windowed* swapchain.
    """
    label = "vulkan surface"
    if platform.system() == "Windows":
        return (label, True, "win32 (VK_USE_PLATFORM_WIN32_KHR is unconditional there)")
    if not _is_unix_windowed():
        return (label, None, "no windowed surface on this platform — sg has no metal backend yet")
    if cxx is None:
        return (label, None, "no compiler resolved, so the header probe could not run")

    found = [name for name, header in _SURFACE_HEADERS if _header_reachable(cxx, header)]
    if found:
        return (label, True, ", ".join(found))
    return (label, None,
            f"none — a windowed swapchain reports the platform unsupported (headless present is unaffected); {_INSTALL_HINT}")


def _window_backend_check(root: Path, cxx: str | None) -> tuple[str, bool | None, str]:
    """Whether `sr::window_system::try_create` will have a backend, which decides SR_HAS_WINDOW.

    Two independent ways to end up without one — SDL3 never fetched, or fetched and then skipped for missing headers —
    and they are worth telling apart, since only the first is fixed by running the fetch script.
    """
    label = "sr::window (SDL3)"
    state = pins.install_state(root, "sdl3")
    if state == "missing":
        return (label, None, "SDL3 not fetched — run: uv run extern/sdl3/fetch-sdl3.py")
    if state == "stale":
        return (label, None, "SDL3 installed at the wrong pin — run: uv run extern/sdl3/fetch-sdl3.py")
    if not _is_unix_windowed():
        return (label, True, "SDL3 fetched, pin current")
    if cxx is None:
        return (label, None, "SDL3 fetched; no compiler resolved, so the header probe could not run")

    found = [h for h in _SDL_HEADERS if _header_reachable(cxx, h)]
    if found:
        return (label, True, f"SDL3 fetched, video via {', '.join(found)}")
    return (label, None,
            f"SDL3 fetched but skipped at configure: SDL needs {' or '.join(_SDL_HEADERS)}; {_INSTALL_HINT}")


def _shader_compiler_check(root: Path) -> tuple[str, bool | None, str]:
    """Whether ssc::dxc is built, which is what turns a shader package's HLSL into DXIL or SPIR-V.

    Without it every shader package still compiles, and every consumer of one fails at `acquire`.
    """
    label = "shader compiler (DXC)"
    state = pins.install_state(root, "dxc")
    if state == "current":
        return (label, True, "extern/dxc/.install, pin current")
    if state == "stale":
        return (label, None, "DXC installed at the wrong pin — run: uv run extern/dxc/download-dxc.py")
    return (label, None, "DXC not fetched — run: uv run extern/dxc/download-dxc.py")


def _backend_rollup(
    headers: tuple[str, bool | None, str],
    vk_runtime: tuple[str, bool | None, str],
    dx12_runtime: tuple[str, bool | None, str],
    cxx: str | None,
) -> list[tuple[str, bool | None, str]]:
    """One line per sg backend: will it build here, and will it create a device.

    Reads the probes' verdicts rather than repeating them, so this section and the evidence below it can never disagree.
    The build gates mirrored are shaped-graphics/CMakeLists.txt's: dx12 is `WIN32` alone, since the DirectX 12 libs ship
    with the Windows SDK, and vulkan is `find_package(Vulkan)` on a non-web target.
    """
    out: list[tuple[str, bool | None, str]] = []

    if platform.system() != "Windows":
        out.append(("sg backend dx12", None, "not built off Windows"))
    elif dx12_runtime[1]:
        out.append(("sg backend dx12", True, "built (Windows SDK); " + dx12_runtime[2]))
    else:
        out.append(("sg backend dx12", None, "built (Windows SDK), but " + dx12_runtime[2]))

    if not headers[1] and cxx is None:
        # An unaskable question is not a no: MSVC reaches its compiler through an environment rather than a path, so
        # the header probe never ran and configure may well find Vulkan anyway.
        out.append(("sg backend vulkan", None, "undetermined — " + headers[2]))
    elif not headers[1]:
        out.append(("sg backend vulkan", None, "not built — " + headers[2]))
    elif vk_runtime[1]:
        out.append(("sg backend vulkan", True, "built; " + vk_runtime[2]))
    else:
        out.append(("sg backend vulkan", None, "built, but " + vk_runtime[2]))

    out.append(("sg backend metal/webgpu", None, "intended tiers, no backend in sg yet"))
    return out


def checks(root: Path, cxx: str | None) -> list[tuple[str, bool | None, str]]:
    """The graphics environment, as (label, ok, detail) triples in the order doctor prints them.

    The per-backend roll-up comes first, then the probes it was derived from as the evidence.
    `cxx` is the compiler the selected preset configures, so the header probes see the same search path CMake will —
    which is the whole answer on a machine whose headers live in a sysroot rather than in /usr/include.
    None where the preset has no resolvable compiler path (MSVC), and the probes then report that rather than guessing.
    """
    headers = _vulkan_headers_check(cxx)
    vk_runtime = _vulkan_runtime_check()
    dx12_runtime = _dx12_runtime_check()
    return [
        *_backend_rollup(headers, vk_runtime, dx12_runtime, cxx),
        headers,
        vk_runtime,
        dx12_runtime,
        _surface_check(cxx),
        _window_backend_check(root, cxx),
        _shader_compiler_check(root),
    ]
