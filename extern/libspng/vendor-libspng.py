#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor libspng in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. libspng
is the PNG codec behind babel::png (see the SC_USE_VENDORED_SPNG option in
extern/CMakeLists.txt). This script regenerates the vendored copy from a pinned
upstream commit: it shallow-clones the tag into a transient `.clone/` dir,
asserts the tag resolves to the pinned hash (the hash is the authority; the tag
is a human-readable convenience), copies the minimal subset we actually build,
and deletes the clone.

The subset is the whole library: upstream is two files, spng.c and spng.h. What
is dropped is only the scaffolding around them -- CMakeLists.txt, meson.build
and cmake/ (build machinery we replace), tests/, examples/, docs/ and the CI
definitions. That is the smallest vendored payload here, which is what puts
libspng in the xxhash tier rather than the fetched SQLite one.

libspng carries no inflate/deflate of its own. It builds against the vendored
zlib, not the miniz alternative SPNG_USE_MINIZ selects: zlib is already in the
tree for cc::compress, and a second Deflate implementation compiled into the
same binary is a cost with no matching benefit.

The two licenses are joint rather than a choice. Upstream's own code is
BSD-2-Clause and ships as `LICENSE`; the SSE2 and NEON defilters at the end of
spng.c are derived from libpng and carry the PNG Reference Library License
version 2 as an inline comment, with no file of its own anywhere upstream. So
this script extracts that notice into `LICENSE-libpng` rather than leaving a
hand-copied text to rot -- and aborts if it cannot find it, because a
restructured upstream must be re-vetted rather than silently vendored without
half its license.

Upstream keeps both files in spng/, and we split them into include/ + src/ to
match the zlib and lz4 layout. spng.c reaches spng.h with a quote include, which
misses in src/ and then resolves against include/ off the target's include path.

Re-running is idempotent: the previously vendored files are wiped first, so
files dropped upstream do not linger. The vendored payload (include/, src/, the
two LICENSE files) is committed to the repo; this script is only needed to bump
or re-vet the version.
"""

import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/libspng/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
# The tag is for humans; pin_hash is the authority — the clone is rejected unless the tag resolves to exactly that commit.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream-relative source -> vendored destination (relative to DEST).
COPY_MAP = {
    "spng/spng.h": "include/spng.h",
    "spng/spng.c": "src/spng.c",
    "LICENSE": "LICENSE",
}

# Everything we own under DEST that a re-vendor must wipe first (so a file dropped
# upstream does not linger). The script, CMakeLists.txt, and this list itself stay.
WIPE = ["include", "src", "LICENSE", "LICENSE-libpng"]

# The inline libpng notice in spng.c: a C comment block opening on the license name.
# Anchored on the name rather than on a line number, so a shifted upstream still matches and a renamed one fails loudly.
LIBPNG_NOTICE = re.compile(r"/\*\n\* PNG Reference Library License version 2\n.*?\n\*/", re.DOTALL)


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


def extract_libpng_notice(source: Path) -> str:
    """The PNG Reference Library License v2 text carried inline in spng.c, with its comment markers stripped."""
    match = LIBPNG_NOTICE.search(source.read_text(encoding="utf-8"))
    if match is None:
        sys.exit(
            f"could not find the libpng notice in {source.name}.\n"
            "Upstream restructured it, or the derived SIMD code is gone. Re-vet the licensing before vendoring."
        )

    body = match.group(0).splitlines()[1:-1]
    return "\n".join(line.removeprefix("*").removeprefix(" ").rstrip() for line in body).strip() + "\n"


def main() -> int:
    up = deps_manifest.one(DEST)

    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)
    CLONE.parent.mkdir(parents=True, exist_ok=True)

    # Shallow-clone just the pinned tag, then verify it is the pinned commit.
    print(f"cloning {up.repo} @ {up.tag} ...")
    run("git", "clone", "--depth", "1", "--branch", up.tag, up.repo, str(CLONE))
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != up.pin_hash:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: tag {up.tag} resolved to {head}, expected {up.pin_hash}.\n"
            "Update tag and pin_hash together in dependency.yml, after vetting the new commit."
        )

    # Read the second license out of the source before anything is wiped, so a failure costs nothing.
    libpng_notice = extract_libpng_notice(CLONE / "spng/spng.c")

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

    (DEST / "LICENSE-libpng").write_text(libpng_notice, encoding="utf-8")

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored {up.name} {up.tag} ({up.pin_hash[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
