"""Locating LLVM tools (llvm-profdata, llvm-cov, ...) for coverage and PGO.

Both pipelines need the same resolution rules: an env override wins, then PATH, then the directory beside the configured compiler, where a Windows LLVM install ships its tools off-PATH.
Kept in one place so coverage and pgo stay in sync.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path


def find_tool(name: str, env_var: str) -> str | None:
    """Locate an llvm-* tool by env override then PATH, with no build dir needed.

    `env_var` (LLVM_PROFDATA, say) wins if set, so a specific install can be pinned; otherwise PATH is searched.
    Returns the resolved path or command, or None.
    """
    override = os.environ.get(env_var)
    if override:
        return override
    return shutil.which(name)


def _compiler_from_cache(build_dir: Path) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_CXX_COMPILER:"):
                return line.partition("=")[2].strip()
    except OSError:
        return None
    return None


def resolve_tool(name: str, env_var: str, *build_dirs: Path) -> str | None:
    """Like find_tool, but also looks beside the compiler configured in each build dir.

    On Windows, clang-cl and llvm-profdata/llvm-cov ship in the same LLVM bin/ that is often not on PATH.
    Falling back to the compiler's directory keeps the versions matched, and the tools must match the clang that built the binaries.

    Several build dirs may be offered and are tried in order.
    That is for inspecting a *foreign* build tree, which may be MSVC-built or have no CMakeCache at all, in which case a local tree that does know an LLVM install is the better guess.
    """
    found = find_tool(name, env_var)
    if found:
        return found
    exe = name + (".exe" if os.name == "nt" else "")
    for build_dir in build_dirs:
        cxx = _compiler_from_cache(build_dir)
        if cxx:
            cand = Path(cxx).parent / exe
            if cand.is_file():
                return str(cand)
    return None
