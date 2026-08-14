"""Read and query a preset's compilation database (compile_commands.json).

The ground truth of what the compiler is actually invoked with, per translation unit — the same database clangd reads.
`info compile-command` uses it to print the exact command for one source file.
CMake writes absolute, forward-slash `file` paths, and matching here is separator- and case-insensitive so a repo-relative path or a bare filename resolves on Windows too.
"""

from __future__ import annotations

import json
import os
import shlex
from pathlib import Path


def load_entries(build_dir: Path) -> list[dict]:
    """Parse build/<preset>/compile_commands.json into its list of entries.

    Each entry is `{directory, command, file, output}`. Raises FileNotFoundError
    when the database is missing (the preset hasn't been configured).
    """
    path = build_dir / "compile_commands.json"
    if not path.is_file():
        raise FileNotFoundError(f"No compile_commands.json in {build_dir}; configure the preset first.")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _norm(p: str | Path) -> str:
    """Normalize a path for comparison: absolute-agnostic, case- and separator-folded."""
    return os.path.normcase(os.path.normpath(str(p)))


def find_entry(entries: list[dict], file: Path, root: Path) -> dict | None:
    """Locate the entry for `file`: by absolute or repo-relative path, else a unique filename-or-suffix tail.

    None when nothing matches, or when the tail is ambiguous — the caller surfaces suggestions.
    """
    raw = str(file)
    target = file if file.is_absolute() else root / file
    want = _norm(target)
    for entry in entries:
        if _norm(entry.get("file", "")) == want:
            return entry

    tail = _norm(raw)
    matches = [
        entry
        for entry in entries
        if _norm(entry.get("file", "")).endswith(os.sep + tail) or _norm(entry.get("file", "")) == tail
    ]
    return matches[0] if len(matches) == 1 else None


def tokenize_windows(cmd: str) -> list[str]:
    """Split a Windows command line the way CommandLineToArgvW does.

    Splitting on whitespace is wrong, and wrong quietly.
    A define like `-DIMGUI_USER_CONFIG=\\"imgui/imgui_config.hh\\"` survives it as a token still carrying its backslashes.
    That reaches the compiler as a literal `\\"...\\"` and fails as `expected "FILENAME"`.
    The rule that actually applies: backslashes are literal unless they precede a quote, where 2n collapse to n and toggle nothing, and 2n+1 collapse to n plus a literal quote.
    """
    args: list[str] = []
    cur: list[str] = []
    backslashes = 0
    in_quotes = False
    started = False

    def flush_backslashes(count: int) -> None:
        cur.extend("\\" * count)

    for ch in cmd:
        if ch == "\\":
            backslashes += 1
            continue
        if ch == '"':
            flush_backslashes(backslashes // 2)
            if backslashes % 2:
                cur.append('"')
            else:
                in_quotes = not in_quotes
                started = True
            backslashes = 0
            continue
        flush_backslashes(backslashes)
        backslashes = 0
        if ch.isspace() and not in_quotes:
            if cur or started:
                args.append("".join(cur))
                cur, started = [], False
            continue
        cur.append(ch)
    flush_backslashes(backslashes)
    if cur or started:
        args.append("".join(cur))
    return args


def split_command(entry: dict) -> list[str]:
    """An entry's compile command as argv.

    A database may carry `arguments` already split, which is always preferred over re-parsing a quoted string.
    Otherwise the string is tokenized by the host's rules, since ninja quotes it for the shell it will run under.
    """
    if isinstance(entry.get("arguments"), list):
        return list(entry["arguments"])
    cmd = entry["command"]
    return tokenize_windows(cmd) if os.name == "nt" else shlex.split(cmd)


# The MSVC-frontend PCH flags, in the joined form CMake emits (`/Yu<hxx>`, `/Fp<pch>`, `/FI<hxx>`).
# The `/`-prefixed spelling is matched case-insensitively; the `-`-prefixed one is not, because `-fi` also opens
# `-finline-functions` and a dozen other real flags.
_PCH_SLASH = ("/yu", "/fp", "/fi")
_PCH_DASH = ("-Yu", "-Fp", "-FI")


def strip_pch_flags(argv: list[str]) -> list[str]:
    """`argv` without the flags that force a precompiled header onto the translation unit.

    Anything that re-runs a compile command out of the database must strip these, and for two different reasons.
    A tool that only reads the TU — clang-tidy — would otherwise need the target's `.pch` to exist, which it does not on a configured-but-unbuilt tree.
    A tool that *measures* — `compile-time` — would otherwise time a header whose whole transitive closure is already deserialized, and report ~0.03 s for everything without erroring.
    That second one is the dangerous half: it produces a plausible number rather than a failure.

    Covers the MSVC frontend's joined `/Yu` / `/Fp` / `/FI` and the GCC-frontend `-include-pch` / `-include` pairs, each with the `-Xclang` that introduces it.
    A `-include` naming something other than CMake's generated `cmake_pch.hxx` is left alone, since that is a deliberate flag rather than PCH wiring.
    """
    n = len(argv)
    drop: set[int] = set()

    for i, tok in enumerate(argv):
        if tok.lower().startswith(_PCH_SLASH) or tok.startswith(_PCH_DASH):
            drop.add(i)
        elif tok == "-include-pch":
            drop.add(i)
            j = i + 1
            while j < n and argv[j] == "-Xclang":
                drop.add(j)
                j += 1
            if j < n:
                drop.add(j)
        elif tok == "-include":
            j = i + 1
            while j < n and argv[j] == "-Xclang":
                j += 1
            if j < n and Path(argv[j]).name.startswith("cmake_pch"):
                drop.update(range(i, j + 1))

    # Backwards, so a `-Xclang` whose only purpose was to introduce a dropped token goes with it even in a chain.
    for i in range(n - 1, -1, -1):
        if argv[i] == "-Xclang" and (i + 1) in drop:
            drop.add(i)

    return [tok for i, tok in enumerate(argv) if i not in drop]


def suggest_files(entries: list[dict], file: Path, limit: int = 10) -> list[str]:
    """Files in the database that look related to `file`, by same name then same stem, for a 'did you mean' hint when find_entry comes up empty."""
    name = _norm(Path(str(file)).name)
    same_name = [e["file"] for e in entries if _norm(Path(e["file"]).name) == name]
    if same_name:
        return same_name[:limit]
    stem = _norm(Path(str(file)).stem)
    return [e["file"] for e in entries if stem and stem in _norm(e["file"])][:limit]
