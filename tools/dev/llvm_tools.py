"""Locating LLVM tools (llvm-profdata, llvm-cov, ...) for coverage and PGO.

Both pipelines need the same resolution rules: an env override wins, then PATH,
then the directory beside the configured compiler (where a Windows LLVM install
ships its tools off-PATH). Kept in one place so coverage and pgo stay in sync.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path


def find_tool(name: str, env_var: str) -> str | None:
    """Locate an llvm-* tool by env override then PATH (no build dir needed).

    `env_var` (e.g. LLVM_PROFDATA) wins if set, so a user can pin a specific
    install; otherwise PATH is searched. Returns the resolved path/command or None.
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


def resolve_tool(name: str, env_var: str, build_dir: Path) -> str | None:
    """Like find_tool, but also looks beside the configured compiler.

    On Windows clang-cl and llvm-profdata/llvm-cov ship in the same LLVM bin/ that
    often isn't on PATH; falling back to the compiler's directory keeps the
    versions matched (the tools must match the clang that built the binaries).
    """
    found = find_tool(name, env_var)
    if found:
        return found
    cxx = _compiler_from_cache(build_dir)
    if cxx:
        exe = name + (".exe" if os.name == "nt" else "")
        cand = Path(cxx).parent / exe
        if cand.is_file():
            return str(cand)
    return None
