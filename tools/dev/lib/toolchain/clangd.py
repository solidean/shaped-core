"""clangd integration: locate clangd and run offline `--check` diagnostics.

`--check` parses a single file against a compilation database — the same compile_commands.json the editor's clangd reads — and reports diagnostics with no LSP server running.
That is what lets `dev.py diagnose clangd <file>` and the doctor check reproduce an editor-only problem from the command line.

One caveat is baked into the parser here.
`--check` also self-tests every refactor tweak at every token and logs those failures at error level ("tweak: ExtractFunction ==> FAIL: ...").
Its "All checks completed, N errors" summary counts them too.
Those are not code diagnostics, so only the structured diagnostic lines are parsed, which look like:

    E[08:40:11.093] [undeclared_var_use] Line 4: use of undeclared identifier 'x'
"""

from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

# The leading level letter (E/I/W/N) gives severity.
# The "[code] Line N: msg" structure is what distinguishes a real diagnostic from a tweak-test failure, which shares the E level but has no such structure.
_DIAG_RE = re.compile(
    r"^(?P<level>[EIWN])\[[\d:.]+\]\s+\[(?P<code>[\w.-]+)\]\s+Line\s+(?P<line>\d+):\s+(?P<message>.*)$"
)

# clangd logs "Failed to find compilation database for <file>" when the file is covered by no compile_commands.json.
# It then falls back to generic flags, which makes diagnostics unreliable: wrong language standard, missing includes.
_NO_CDB_RE = re.compile(r"Failed to find compilation database")

_SEVERITY = {"E": "error", "W": "warning", "I": "warning", "N": "note"}

# Common Windows install locations to try when clangd is not on PATH.
_FALLBACK_PATHS = (
    Path(r"C:\Program Files\LLVM\bin\clangd.exe"),
    Path(r"C:\Program Files (x86)\LLVM\bin\clangd.exe"),
)


@dataclass(frozen=True)
class Diagnostic:
    """One diagnostic for the checked file.

    clangd reports the line but not the column in `--check` mode, so `column` is absent here.
    """

    line: int
    severity: str  # "error" | "warning" | "note"
    code: str
    message: str

    @property
    def is_error(self) -> bool:
        return self.severity == "error"


@dataclass(frozen=True)
class CheckResult:
    """Outcome of a `clangd --check` run over a single file."""

    clangd: str
    file: Path
    returncode: int
    diagnostics: tuple[Diagnostic, ...]
    found_database: bool
    log: str

    @property
    def errors(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if d.is_error]

    @property
    def warnings(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if not d.is_error]

    @property
    def ok(self) -> bool:
        """True when clangd reported no error-level diagnostics for the file.

        Deliberately ignores `--check`'s tweak-test failures and its inflated error summary; only structured error diagnostics count.
        """
        return not self.errors


def find_clangd(explicit: str | None = None) -> str | None:
    """Locate the clangd executable: an explicit path or name, then PATH, then the common LLVM install locations.

    None when nothing usable is found.
    """
    if explicit:
        if Path(explicit).is_file():
            return explicit
        return shutil.which(explicit)
    found = shutil.which("clangd")
    if found:
        return found
    for candidate in _FALLBACK_PATHS:
        if candidate.is_file():
            return str(candidate)
    return None


def version(clangd: str) -> str | None:
    """Return clangd's version line, or None if it cannot be run."""
    try:
        result = subprocess.run([clangd, "--version"], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None
    out = (result.stdout or "").strip()
    return out.splitlines()[0] if out else None


def parse_diagnostics(log: str) -> list[Diagnostic]:
    """Extract structured diagnostics from a `clangd --check` log, ignoring the tweak-test failures and progress lines."""
    diagnostics: list[Diagnostic] = []
    for raw in log.splitlines():
        match = _DIAG_RE.match(raw)
        if match is None:
            continue
        diagnostics.append(
            Diagnostic(
                line=int(match.group("line")),
                severity=_SEVERITY.get(match.group("level"), "warning"),
                code=match.group("code"),
                message=match.group("message").rstrip(),
            )
        )
    return diagnostics


def check_file(
    clangd: str,
    file: Path,
    *,
    compile_commands_dir: Path | None = None,
    timeout: float | None = None,
) -> CheckResult:
    """Run `clangd --check` on `file` and parse the diagnostics it reports.

    `compile_commands_dir` is the directory containing compile_commands.json.
    When None, clangd searches upward from the file and falls back to generic flags, so `found_database` is False and the diagnostics are unreliable.
    """
    cmd = [clangd, f"--check={file}"]
    if compile_commands_dir is not None:
        cmd.append(f"--compile-commands-dir={compile_commands_dir}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    # clangd writes its log (including diagnostics) to stderr; merge defensively.
    log = (result.stderr or "") + (result.stdout or "")
    return CheckResult(
        clangd=clangd,
        file=file,
        returncode=result.returncode,
        diagnostics=tuple(parse_diagnostics(log)),
        found_database=_NO_CDB_RE.search(log) is None,
        log=log,
    )
