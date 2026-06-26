"""Terminal color support: a single global enable flag plus style helpers.

dev.py and the helper modules print plain text by default; wrapping a message in
one of the style helpers here colors it only when color is enabled, and returns
it unchanged otherwise — so call sites read the same in either mode.

`configure` resolves the mode once at startup (from --plain/--colored or, in auto
mode, from the terminal and the NO_COLOR/FORCE_COLOR conventions); every later
helper reads that one decision. Zero dependencies: just ANSI SGR escapes.
"""

from __future__ import annotations

import os
import sys

_GREEN = "\033[32m"
_YELLOW = "\033[33m"
_RED = "\033[31m"
_DIM = "\033[2m"
_BOLD = "\033[1m"
_RESET = "\033[0m"

_enabled = False


def configure(mode: str = "auto") -> None:
    """Set whether style helpers emit ANSI codes. `mode` is one of:

    - "colored": always on; "plain": always off — the explicit --colored/--plain
      flags, which override everything.
    - "auto": NO_COLOR (any value) forces off and wins over FORCE_COLOR (the
      no-color.org convention); FORCE_COLOR forces on even when piped; otherwise
      on only when both stdout and stderr are TTYs. Requiring both means piping
      either stream (the agent / `| cat` case) yields plain output and keeps ANSI
      out of redirected stdout data.
    """
    global _enabled
    if mode == "colored":
        _enabled = True
    elif mode == "plain":
        _enabled = False
    else:  # auto
        if os.environ.get("NO_COLOR") is not None:
            _enabled = False
        elif os.environ.get("FORCE_COLOR") is not None:
            _enabled = True
        else:
            _enabled = sys.stdout.isatty() and sys.stderr.isatty()


def enabled() -> bool:
    return _enabled


def _wrap(code: str, s: str) -> str:
    return f"{code}{s}{_RESET}" if _enabled else s


def green(s: str) -> str:
    return _wrap(_GREEN, s)


def yellow(s: str) -> str:
    return _wrap(_YELLOW, s)


def red(s: str) -> str:
    return _wrap(_RED, s)


def dim(s: str) -> str:
    return _wrap(_DIM, s)


def bold(s: str) -> str:
    return _wrap(_BOLD, s)
