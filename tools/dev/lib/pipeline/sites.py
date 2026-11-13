"""Sites: resolve the opaque source sites a failed test printed, `module+0xoffset`, to file, line and function.

Below `SC_SOURCE_SITES=FULL` a check, an assert or a test names its site by code address alone (docs/platforms.md, "Source sites").
After a failed run this reads the binary's logs, resolves every site of a module beside the binary through `llvm-symbolizer` (atos for a Mach-O) and the build's debug info,
and prints each as `file:line (function)` with the source line there, since an assert below FULL carries no condition text.
The frames of clean-core's `source_location.hh` are skipped: it takes the address, so its frames are inlined innermost into every site.

Best-effort by contract: a missing symbolizer, a binary without debug info or a symbolizer error prints the raw site and why, and never fails the run.

Public API:
    report(logs, artifact, build_dir, root) -> list[str]   (prints and returns the lines it added)
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..core import console, ui

# `name+0xhex` as cc::describe_site prints it; the name is a module's file name, so whether it is one is checked against the disk.
_SITE = re.compile(r"(?<![\w.+-])([\w.-]+)\+0x([0-9a-fA-F]+)\b")

# The header whose frames sit innermost in every site (clean-core/platform/source_location.hh).
_SITE_HEADER = "clean-core/platform/source_location.hh"

# Enough to name every distinct failure of a typical run; a test failing thousands of checks repeats the same few sites.
_MAX_SITES = 24

_SYMBOLIZER_TIMEOUT_S = 120


@dataclass(frozen=True)
class Frame:
    function: str
    file: str
    line: int


def find_sites(text: str) -> list[tuple[str, int]]:
    """Every distinct (module, offset) the text names, in order of first mention."""
    seen: dict[tuple[str, int], None] = {}
    for m in _SITE.finditer(text):
        seen.setdefault((m.group(1), int(m.group(2), 16)), None)
    return list(seen)


def parse_frames(stdout: str, count: int) -> list[list[Frame]]:
    """llvm-symbolizer's answer, one block per address, as frames innermost first.

    A block is pairs of lines, function then `file:line:column`, and ends at a blank line.
    """
    blocks: list[list[Frame]] = []
    current: list[str] = []
    for line in stdout.splitlines() + [""]:
        if line.strip():
            current.append(line.strip())
            continue
        if current:
            frames = []
            for function, where in zip(current[0::2], current[1::2]):
                file, line_no = _split_location(where)
                frames.append(Frame(function=function, file=file, line=line_no))
            blocks.append(frames)
            current = []
    return (blocks + [[] for _ in range(count)])[:count]


def _split_location(where: str) -> tuple[str, int]:
    """`C:\\a\\b.cc:42:7` -> (`C:\\a\\b.cc`, 42); `??:0:0` -> (`??`, 0)."""
    parts = where.rsplit(":", 2)
    if len(parts) == 3 and parts[1].isdigit():
        return parts[0], int(parts[1])
    parts = where.rsplit(":", 1)
    if len(parts) == 2 and parts[1].isdigit():
        return parts[0], int(parts[1])
    return where, 0


def outside_site_header(frames: list[Frame]) -> list[Frame]:
    """The frames below the site header's own, the first of which is the site."""
    return [f for f in frames if not f.file.replace("\\", "/").endswith(_SITE_HEADER)]


def find_symbolizer(build_dir: Path) -> str | None:
    """llvm-symbolizer: LLVM_SYMBOLIZER, else beside the build's compiler, the install that wrote its debug info, else PATH."""
    override = os.environ.get("LLVM_SYMBOLIZER")
    if override:
        return override
    exe = "llvm-symbolizer" + (".exe" if os.name == "nt" else "")
    try:
        cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        cache = []
    # the compiler may be cached as a bare name (`clang-cl`), so the tools CMake found beside it are asked too
    for key in ("CMAKE_CXX_COMPILER:", "CMAKE_CXX_COMPILER_AR:", "CMAKE_LINKER:"):
        for line in cache:
            if line.startswith(key):
                value = line.partition("=")[2].strip()
                tool = value if Path(value).is_absolute() else shutil.which(value)
                if tool and (Path(tool).parent / exe).is_file():
                    return str(Path(tool).parent / exe)
    return shutil.which("llvm-symbolizer")


_MH_MAGIC_64 = 0xFEEDFACF
_LC_SEGMENT_64 = 0x19


def macho_text_vmaddr(module: Path) -> int | None:
    """`__TEXT`'s vmaddr in a 64-bit Mach-O, or None for any other file.

    A Mach-O module's offset is from its header, which is `__TEXT`'s first byte, while its symbols are keyed on vmaddr.
    """
    try:
        with open(module, "rb") as f:
            head = f.read(32)
            if len(head) < 32 or int.from_bytes(head[0:4], "little") != _MH_MAGIC_64:
                return None
            ncmds = int.from_bytes(head[16:20], "little")
            commands = f.read(int.from_bytes(head[20:24], "little"))
    except OSError:
        return None
    at = 0
    for _ in range(ncmds):
        cmd = int.from_bytes(commands[at:at + 4], "little")
        size = int.from_bytes(commands[at + 4:at + 8], "little")
        if cmd == _LC_SEGMENT_64 and commands[at + 8:at + 24].rstrip(b"\0") == b"__TEXT":
            return int.from_bytes(commands[at + 24:at + 32], "little")
        if size <= 0:
            break
        at += size
    return None


# `function (in module) (/path/file.cc:42)`; a frame without line info ends `(in module) + 12` or is a bare address
_ATOS_FRAME = re.compile(r"^(.*?) \(in [^)]*\)(?: \((.*):(\d+)\))?")


def parse_atos(stdout: str, count: int) -> list[list[Frame]]:
    """atos -i's answer, one block per address, as frames innermost first.

    A block is its frame lines and then one blank line; an address atos has no symbol for still takes one empty line first.
    """
    blocks: list[list[Frame]] = []
    lines = stdout.split("\n")
    i = 0
    while i < len(lines) and len(blocks) < count:
        frames: list[Frame] = []
        first = True
        while i < len(lines) and (first or lines[i].strip()):
            line = lines[i].strip()
            i += 1
            first = False
            if not line:
                continue
            m = _ATOS_FRAME.match(line)
            if m is None:
                frames.append(Frame(function=line, file="??", line=0))
            else:
                frames.append(Frame(function=m.group(1), file=m.group(2) or "??", line=int(m.group(3) or 0)))
        if i < len(lines) and not lines[i].strip():
            i += 1  # the blank line closing the block
        blocks.append(frames)
    return (blocks + [[] for _ in range(count)])[:count]


def _symbolize_macho(module: Path, vmaddr: int, offsets: list[int]) -> list[list[Frame]]:
    """atos, which reads the debug map into the objects; llvm-symbolizer finds no lines in a Mach-O without a dSYM."""
    xcrun = shutil.which("xcrun")
    if xcrun is None:
        raise RuntimeError("xcrun not found, which runs atos")
    cmd = [xcrun, "atos", "-fullPath", "-i", "-o", str(module), "-l", f"{vmaddr:#x}"] + [f"{vmaddr + o:#x}" for o in offsets]
    completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                               timeout=_SYMBOLIZER_TIMEOUT_S, check=False)
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or f"exit code {completed.returncode}")
    return parse_atos(completed.stdout, len(offsets))


def _symbolize(symbolizer: str, module: Path, offsets: list[int]) -> list[list[Frame]]:
    """One symbolizer process for all of a module's offsets."""
    if (vmaddr := macho_text_vmaddr(module)) is not None:
        return _symbolize_macho(module, vmaddr, offsets)
    cmd = [symbolizer, f"--obj={module}", "--inlining", "--demangle", "--functions=linkage"]
    # A COFF module's offset is an RVA; an ELF one is already the address its DWARF is keyed on, for a PIE loaded at its bias.
    if module.suffix.lower() in (".exe", ".dll"):
        cmd.append("--relative-address")
    completed = subprocess.run(cmd, input="".join(f"{o:#x}\n" for o in offsets), capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=_SYMBOLIZER_TIMEOUT_S, check=False)
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or f"exit code {completed.returncode}")
    return parse_frames(completed.stdout, len(offsets))


def _source_file(file: str, roots: list[Path]) -> Path | None:
    p = Path(file)
    if p.is_absolute():
        return p if p.is_file() else None
    for r in roots:
        if (r / p).is_file():
            return r / p
    return None


def _display(path: Path | None, file: str, root: Path) -> str:
    if path is None:
        return file
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def _source_line(path: Path | None, line: int) -> str | None:
    if path is None or line <= 0:
        return None
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for i, text in enumerate(f, 1):
                if i == line:
                    return text.rstrip("\r\n").strip()
    except OSError:
        return None
    return None


def resolve_lines(sites: list[tuple[str, int]], modules: dict[str, Path], symbolizer: str, build_dir: Path,
                  root: Path) -> list[str]:
    """The report lines for `sites`, each module symbolized once."""
    roots = [root, build_dir]
    resolved: dict[tuple[str, int], list[Frame] | str] = {}
    for name, path in modules.items():
        offsets = [o for m, o in sites if m == name]
        try:
            for offset, frames in zip(offsets, _symbolize(symbolizer, path, offsets)):
                resolved[(name, offset)] = frames
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as e:
            for offset in offsets:
                resolved[(name, offset)] = f"llvm-symbolizer failed: {e}"

    lines: list[str] = []
    for site in sites:
        label = f"{site[0]}+{site[1]:#x}"
        answer = resolved.get(site)
        if isinstance(answer, str):
            lines.append(f"  {label}: {answer}")
            continue
        frames = outside_site_header(answer or [])
        if not frames or frames[0].file in ("??", ""):
            lines.append(f"  {label}: not resolved (no debug info for it)")
            continue
        head = frames[0]
        path = _source_file(head.file, roots)
        lines.append(f"  {label} -> {_display(path, head.file, root)}:{head.line} ({head.function})")
        text = _source_line(path, head.line)
        # a function's entry, a test's own site among them, is its opening brace; the declaration is the line above
        if text == "{" and (above := _source_line(path, head.line - 1)):
            lines.append(f"      {head.line - 1} | {above}")
        if text:
            lines.append(f"      {head.line} | {text}")
        for f in frames[1:]:
            lines.append(f"      inlined into {f.function} at {_display(_source_file(f.file, roots), f.file, root)}:{f.line}")
    return lines


def report(logs: list[Path | None], artifact: Path, build_dir: Path, root: Path) -> list[str]:
    """Print the sites a failed run's logs name, resolved, and return the lines printed.

    Only names of files beside `artifact` count as modules, so a stray `word+0x1f` in a message is left alone.
    """
    text = ""
    for log in logs:
        if log is not None and log.is_file():
            text += log.read_text(encoding="utf-8", errors="replace") + "\n"
    modules: dict[str, Path] = {}
    sites = []
    for name, offset in find_sites(text):
        path = modules.get(name) or (artifact.parent / name)
        if path.is_file():
            modules[name] = path
            sites.append((name, offset))
    if not sites:
        return []
    dropped = max(0, len(sites) - _MAX_SITES)
    sites = sites[:_MAX_SITES]
    modules = {n: p for n, p in modules.items() if any(s[0] == n for s in sites)}

    symbolizer = find_symbolizer(build_dir)
    # Mach-O modules go through atos, which needs no llvm-symbolizer
    if symbolizer is None and all(macho_text_vmaddr(p) is not None for p in modules.values()):
        symbolizer = ""
    if symbolizer is None:
        lines = [f"  {m}+{o:#x}" for m, o in sites]
        lines.append("  llvm-symbolizer not found beside the compiler or on PATH; set LLVM_SYMBOLIZER to resolve these")
    else:
        lines = resolve_lines(sites, modules, symbolizer, build_dir, root)
    if dropped:
        lines.append(f"  ... and {dropped} more site(s) in the logs")
    lines.insert(0, "  sites:")
    for line in lines:
        ui.write_line(console.yellow(line) if line.startswith("  sites") else line)
    return lines
