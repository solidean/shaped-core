#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/lib/quality/imports.py — what a self-test's gate watches, read from its imports.

Each case lays out a small repository of its own, with a facade of the dev package's shape, and reads it back.

    uv run tools/dev/imports-self-test.py [-v]
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.dev.lib.quality import imports  # noqa: E402

VERBOSE = "-v" in sys.argv
failures = 0

FACADE = """
_LAZY: dict[str, tuple[str, str]] = {
    'run_step': ('.lib.core.process', 'run_step'),
    'console': ('.lib.core', 'console'),
}
"""


def check(condition: bool, what: str, detail: str = "") -> None:
    global failures
    if condition:
        if VERBOSE:
            print(f"ok    {what}")
        return
    failures += 1
    print(f"FAIL  {what}" + (f"\n{detail}" if detail else ""))


def layout(root: Path, files: dict[str, str]) -> None:
    for rel, text in {"tools/__init__.py": "", "tools/dev/__init__.py": FACADE, "tools/dev/lib/__init__.py": "",
                      "tools/dev/lib/core/__init__.py": "", "tools/dev/lib/core/console.py": "",
                      "tools/dev/lib/core/process.py": "from . import console\n",
                      "tools/dev/lib/core/unrelated.py": "", **files}.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text, encoding="utf-8")


def watched(root: Path, script: str) -> set[str]:
    imports._closure.cache_clear()
    imports._facade_table.cache_clear()
    return set(imports.closure(root, script))


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="imports-self-test-") as tmp:
        root = Path(tmp)
        layout(root, {
            "a-self-test.py": "import os\nfrom tools import dev\ndev.run_step(['x'])\n",
            "b-self-test.py": "from tools.dev.lib.core import console\n",
            "c-self-test.py": "from tools.dev import console\n",
            "d-self-test.py": "import json\n",
        })

        a = watched(root, "a-self-test.py")
        check("tools/dev/lib/core/process.py" in a, "a facade use reaches the module behind its name", str(sorted(a)))
        check("tools/dev/lib/core/console.py" in a, "and what that module imports, relatively", str(sorted(a)))
        check("tools/dev/lib/core/unrelated.py" not in a, "but no module the facade could reach and was not asked for")

        b = watched(root, "b-self-test.py")
        check("tools/dev/lib/core/console.py" in b, "a direct import is watched")
        check("tools/dev/lib/core/process.py" not in b, "and nothing beside it")
        check("tools/dev/lib/core/__init__.py" in b, "a package's __init__ runs first, so it is watched too")

        c = watched(root, "c-self-test.py")
        check("tools/dev/lib/core/console.py" in c, "a name imported from the facade resolves through its table")

        d = watched(root, "d-self-test.py")
        check(d == {"d-self-test.py"}, "the standard library is nobody's to watch", str(sorted(d)))
    print(f"imports-self-test: {'all passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
