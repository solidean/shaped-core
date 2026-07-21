#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Vendor Dear ImGui in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. ImGui
backs the debug/tooling UI; the renderer that draws its output is ours, written
against sg (libs/graphics/shaped-rendering). This script regenerates the
vendored copy from a pinned upstream commit: it shallow-clones the tag into a
transient `.clone/` dir, asserts the tag resolves to the pinned hash (the hash
is the authority; the tag is a human-readable convenience), copies the minimal
subset we actually build, and deletes the clone.

We track the *docking* branch, which upstream tags per release as
`v<version>-docking`. Pinning that tag rather than the branch head is what keeps
this reproducible — the branch itself moves.

ImGui keeps its sources flat at the repo root, but we mirror the mimalloc layout
(include/ + src/) for consistency: headers to include/, translation units to
src/. The .cpp files `#include "imgui.h"` by bare name, resolved via the
include/ dir our CMake puts on the path.

Deliberately not vendored:
  backends/       we draw ImGui through sg, so upstream's per-API backends
                  (dx12, vulkan, win32, ...) would be dead weight and would pull
                  in exactly the native calls this repo keeps out of its libs.
  misc/freetype/  the built-in stb_truetype rasterizer is enough; freetype would
                  be a second vendored dependency for a font quality we do not
                  need yet.
  examples/, docs/, .github/

imgui_demo.cpp *is* vendored: it is the payload the renderer's GPU test draws,
and the fastest way to exercise every draw path, scissor rect and glyph-atlas
update at once.

The vendored imconfig.h is kept byte-identical to upstream on purpose. WIPE
deletes include/ on every re-vendor, so a local edit there would be silently
clobbered on the next bump. Build-affecting defines live on the CMake target
instead (see CMakeLists.txt), and allocation is routed through mimalloc at
runtime via ImGui::SetAllocatorFunctions.

Re-running is idempotent: the previously vendored files are wiped first, so
files dropped upstream do not linger. The vendored payload (include/, src/,
LICENSE.txt) is committed to the repo; this script is only needed to bump or
re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# Pinned upstream. The tag is for humans; PIN_HASH is the authority — the clone is
# rejected unless the tag resolves to exactly this commit. Bump PIN_TAG and PIN_HASH
# together, only after vetting the new commit.
REPO_URL = "https://github.com/ocornut/imgui"
PIN_TAG = "v1.92.8-docking"
PIN_HASH = "b61e56346a92cfcaf1f43a545ca37b0b32239654"

# This script lives in extern/imgui/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# Upstream-relative source -> vendored destination (relative to DEST). ImGui's sources
# sit at the repo root; we remap them into include/ + src/ to match the mimalloc layout.
COPY_MAP = {
    "imgui.h": "include/imgui.h",
    "imconfig.h": "include/imconfig.h",
    "imgui_internal.h": "include/imgui_internal.h",
    "imstb_rectpack.h": "include/imstb_rectpack.h",
    "imstb_textedit.h": "include/imstb_textedit.h",
    "imstb_truetype.h": "include/imstb_truetype.h",
    "imgui.cpp": "src/imgui.cpp",
    "imgui_draw.cpp": "src/imgui_draw.cpp",
    "imgui_tables.cpp": "src/imgui_tables.cpp",
    "imgui_widgets.cpp": "src/imgui_widgets.cpp",
    "imgui_demo.cpp": "src/imgui_demo.cpp",
    "LICENSE.txt": "LICENSE.txt",
}

# Everything we own under DEST that a re-vendor must wipe first (so a file dropped
# upstream does not linger). The script, CMakeLists.txt, and this list itself stay.
WIPE = ["include", "src", "LICENSE.txt"]


def _force_rmtree(path: Path) -> None:
    """rmtree that survives Windows: git packs the .git objects read-only, which
    blocks os.unlink — clear the read-only bit on error and retry."""

    def on_error(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    shutil.rmtree(path, onexc=on_error)


def run(*args: str, cwd: Path | None = None) -> str:
    """Run a git command, returning stripped stdout; abort loudly on failure."""
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"command failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout.strip()


def main() -> int:
    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)
    CLONE.parent.mkdir(parents=True, exist_ok=True)

    # Shallow-clone just the pinned tag, then verify it is the pinned commit.
    print(f"cloning {REPO_URL} @ {PIN_TAG} ...")
    run("git", "clone", "--depth", "1", "--branch", PIN_TAG, REPO_URL, str(CLONE))
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != PIN_HASH:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: tag {PIN_TAG} resolved to {head}, expected {PIN_HASH}.\n"
            "Update PIN_TAG/PIN_HASH together after vetting the new commit."
        )

    # Wipe the previously vendored payload so dropped-upstream files do not linger.
    for name in WIPE:
        target = DEST / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()

    # Copy the minimal subset we build.
    for src, dst in COPY_MAP.items():
        dest_path = DEST / dst
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CLONE / src, dest_path)

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored Dear ImGui {PIN_TAG} ({PIN_HASH[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
