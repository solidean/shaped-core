#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor the Dear ImGui bundle in-tree at pinned commits.

The bundle is Dear ImGui plus two addons drawn through it: ImPlot (immediate-mode plotting) and ImGuizmo (3D manipulation gizmos).
All three live under extern/imgui/ and compile into one `imgui` CMake target, so one script vendors them together — a lone re-vendor of any single library would wipe the others out of the shared include/ + src/ dirs.

shaped-core builds offline and reproducibly, so third-party dependencies live in-tree rather than as submodules or fetched-at-configure-time packages.
ImGui backs the debug/tooling UI;
the renderer that draws its output is ours, written against sg (libs/graphics/shaped-rendering).
This script regenerates the vendored copy from pinned upstream commits:
for each library it fetches the pinned commit into a transient `.clone/` dir,
asserts it resolves to the pinned hash (the hash is the authority; the tag, where one exists, is a human-readable convenience),
copies the minimal subset we actually build,
and deletes the clone.

We track ImGui's *docking* branch, which upstream tags per release as `v<version>-docking`.
Pinning that tag rather than the branch head is what keeps this reproducible — the branch itself moves.
ImPlot's latest release predates ImGui 1.92's draw/texture API, and ImGuizmo cuts no release tags at all.
So both pin a raw commit on their default branch and fetch that sha directly.

ImGui and ImGuizmo keep their sources flat (ImGui at the repo root, ImGuizmo under src/), but we mirror the mimalloc layout for all three: translation units to src/, headers to include/imgui/.
The extra imgui/ level is what lets a consumer write `#include <imgui/imgui.h>` — the whole bundle namespaced under one dir on a single public include root.
The vendored .cpp/.h still `#include "imgui.h"` and their own headers by bare name; those resolve because CMake also puts include/imgui/ on the path privately, for our build only (a consumer must use the <imgui/...> form).

Deliberately not vendored:
  ImGui backends/       we draw ImGui through sg, so upstream's per-API backends (dx12, vulkan, win32, ...) would be dead weight and would pull in exactly the native calls this repo keeps out of its libs.
  ImGui misc/freetype/  the built-in stb_truetype rasterizer is enough; freetype would be a second vendored dependency for a font quality we do not need yet.
  ImGuizmo extras       the repo's other widgets (ImSequencer, ImCurveEdit, ImGradient, GraphEditor, ...) are separate opt-ins; only the core gizmo (ImGuizmo.h/.cpp) is wanted so far.
  examples/, docs/, .github/

The demos *are* vendored — imgui_demo.cpp and implot_demo.cpp: they are the payload the renderer's GPU test draws, and the fastest way to exercise every draw path, scissor rect and glyph-atlas update at once.

The vendored imconfig.h is kept byte-identical to upstream on purpose.
The wipe deletes include/ on every re-vendor, so a local edit there would be silently clobbered on the next bump.
Build-affecting defines live on the CMake target instead (see CMakeLists.txt), and allocation is routed through mimalloc at runtime via ImGui::SetAllocatorFunctions.

Our own additions to the vendored library live in shaped/imgui/ — forward declarations (imgui_fwd.hh), the injected user config (imgui_config.hh), and the shaped-code interop umbrella (imgui_sc.hh, over impl/imgui_cc.hh + impl/imgui_tg.hh).
They mirror the imgui/ level so they include as <imgui/...> alongside the vendored headers.
That directory sits outside include/ and src/ precisely so the wipe never touches it;
CMakeLists.txt puts shaped/ on the include path.
Anything that must survive a re-vendor goes there, never in include/.

The imstb_*.h files are stb (rect-pack, truetype, textedit) as imgui bundles it: renamed files, unmodified symbols.
Nothing of stb reaches global scope from us —
imgui compiles the implementations with STBRP_STATIC / STBTT_STATIC (internal linkage)
and we set IMGUI_STB_NAMESPACE to scope the types as well, which is what keeps another library's stb from colliding with this one.
That is a build-level setting rather than a rewrite of these files, deliberately:
a symbol-prefixing pass here would have to be re-derived and re-vetted on every version bump, and would silently miss anything upstream added.

TODO: get off stb entirely.
It is a hobby-grade dependency — no release process, known parsing robustness gaps in truetype against hostile or malformed font files — and it sits on the path that loads user-supplied fonts.
The replacement is either freetype (upstream supports it via misc/freetype) or our own rasterizer.

Re-running is idempotent: the previously vendored files are wiped first, so files dropped upstream do not linger.
The vendored payload (include/, src/, LICENSE-*.txt) is committed to the repo;
this script is only needed to bump or re-vet a version.
"""

import os
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class Library:
    # The vendoring plan for one library in the bundle: what to copy, and what to fix up afterwards.
    # Its pin and its license destination come from dependency.yml — `name` is what pairs the two, so it must match a `name:` there.
    name: str
    # Upstream-relative source -> vendored destination (relative to DEST).
    copy_map: dict[str, str] = field(default_factory=dict)
    # Upstream-relative path of the library's LICENSE; its destination is dependency.yml's first `license_files` entry.
    license_src: str = ""
    # Post-copy text substitutions on a vendored destination: {dest: (old, new)}.
    # Kept to filename-consistency renames, never logic — the sole use is fixing ImGuizmo's bare self-include after its lowercase rename.
    rewrites: dict[str, tuple[str, str]] = field(default_factory=dict)


# The copy plan, one entry per library in the bundle. The pins live in dependency.yml.
LIBRARIES = [
    Library(
        name="Dear ImGui",
        copy_map={
            "imgui.h": "include/imgui/imgui.h",
            "imconfig.h": "include/imgui/imconfig.h",
            "imgui_internal.h": "include/imgui/imgui_internal.h",
            "imstb_rectpack.h": "include/imgui/imstb_rectpack.h",
            "imstb_textedit.h": "include/imgui/imstb_textedit.h",
            "imstb_truetype.h": "include/imgui/imstb_truetype.h",
            "imgui.cpp": "src/imgui.cpp",
            "imgui_draw.cpp": "src/imgui_draw.cpp",
            "imgui_tables.cpp": "src/imgui_tables.cpp",
            "imgui_widgets.cpp": "src/imgui_widgets.cpp",
            "imgui_demo.cpp": "src/imgui_demo.cpp",
        },
        license_src="LICENSE.txt",
    ),
    Library(
        name="ImPlot",
        copy_map={
            "implot.h": "include/imgui/implot.h",
            "implot_internal.h": "include/imgui/implot_internal.h",
            "implot.cpp": "src/implot.cpp",
            "implot_items.cpp": "src/implot_items.cpp",
            "implot_demo.cpp": "src/implot_demo.cpp",
        },
        license_src="LICENSE",
    ),
    Library(
        name="ImGuizmo",
        copy_map={
            "src/ImGuizmo.h": "include/imgui/imguizmo.h",
            "src/ImGuizmo.cpp": "src/imguizmo.cpp",
        },
        # Lowercased for consistency with imgui.h / implot.h; fix the .cpp's bare self-include to match.
        rewrites={"src/imguizmo.cpp": ('#include "ImGuizmo.h"', '#include "imguizmo.h"')},
        license_src="LICENSE",
    ),
]

# This script lives in extern/imgui/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pins live in dependency.yml next to this script, so no pin is written twice.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Everything we own under DEST that a re-vendor must wipe first, so a file dropped upstream does not linger.
# The per-library license destinations complete it, so it is built in main() once the manifest is loaded.
# The script, CMakeLists.txt, shaped/, and this list itself stay.
WIPE_DIRS = ["include", "src"]


def _force_rmtree(path: Path) -> None:
    """rmtree that survives Windows: git packs the .git objects read-only, which blocks os.unlink — clear the read-only bit on error and retry."""

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


def fetch(up: "deps_manifest.Upstream", into: Path) -> None:
    """Fetch the upstream's pinned commit into `into`, then assert it is exactly that commit.

    A tagged library is cloned by tag and cross-checked against the pinned hash, which catches a mistyped hash.
    An untagged library (ImPlot, ImGuizmo) fetches the sha directly — GitHub serves an arbitrary commit on request.
    """
    if up.tag is not None:
        print(f"cloning {up.repo} @ {up.tag} ...")
        run("git", "clone", "--depth", "1", "--branch", up.tag, up.repo, str(into))
    else:
        print(f"fetching {up.repo} @ {up.pin_hash[:12]} ...")
        run("git", "init", "--quiet", str(into))
        run("git", "-C", str(into), "remote", "add", "origin", up.repo)
        run("git", "-C", str(into), "fetch", "--depth", "1", "--quiet", "origin", up.pin_hash)
        run("git", "-C", str(into), "checkout", "--quiet", "FETCH_HEAD")

    head = run("git", "-C", str(into), "rev-parse", "HEAD")
    if head != up.pin_hash:
        _force_rmtree(CLONE)
        ref = up.tag if up.tag is not None else up.pin_hash
        sys.exit(
            f"pin mismatch for {up.name}: {ref} resolved to {head}, expected {up.pin_hash}.\n"
            "Update pin_hash (and tag) together in dependency.yml, after vetting the new commit."
        )


def main() -> int:
    # Pair each library's copy plan with its manifest entry; a name in one and not the other is a hard error.
    upstreams = {up.name: up for up in deps_manifest.load(DEST)}
    missing = [lib.name for lib in LIBRARIES if lib.name not in upstreams]
    if missing or len(upstreams) != len(LIBRARIES):
        sys.exit(f"dependency.yml and LIBRARIES disagree: {sorted(upstreams)} vs {[lib.name for lib in LIBRARIES]}")

    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)
    CLONE.mkdir(parents=True, exist_ok=True)

    # Wipe the previously vendored payload up front, so a file dropped upstream in any library does not linger.
    for name in WIPE_DIRS + [f for up in upstreams.values() for f in up.license_files]:
        target = DEST / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()

    for lib in LIBRARIES:
        up = upstreams[lib.name]
        clone_dir = CLONE / lib.name.replace(" ", "_")
        fetch(up, clone_dir)

        # Copy the minimal subset we build, plus the license under the per-library name dependency.yml gives it.
        for src, dst in {**lib.copy_map, lib.license_src: up.license_files[0]}.items():
            dest_path = DEST / dst
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(clone_dir / src, dest_path)

        # Apply filename-consistency rewrites, on bytes so line endings survive.
        for dst, (old, new) in lib.rewrites.items():
            path = DEST / dst
            content = path.read_bytes()
            if old.encode() not in content:
                _force_rmtree(CLONE)
                sys.exit(f"rewrite target not found in {dst}: {old}")
            path.write_bytes(content.replace(old.encode(), new.encode()))

        _force_rmtree(clone_dir)

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored the Dear ImGui bundle: {len(vendored)} files into {DEST.as_posix()}")
    for up in upstreams.values():
        print(f"  {up.name} {up.tag or up.pin_hash[:12]} ({up.pin_hash[:12]})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
