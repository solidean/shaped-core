#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Puts WGSL through every WebGPU implementation on this machine, and says which of them accepts it.

Takes `.wgsl` files, and `.sgl` files with --entry, which it compiles through the `sgl` tool first.

node's binding is Dawn, which Chrome ships, and deno carries wgpu, which Firefox ships, so running both is how
SGL's output gets checked against both browser engines without a browser.
Exit 1 when some implementation rejected something, 0 when all of them accepted everything, 2 when no runtime
could be driven at all, which is neither answer.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROBE = HERE / "probe.mjs"
REPO = HERE.parents[4]
# The `webgpu` package shaped-core installs, so the common case needs no argument.
NODE_MODULES = REPO / "tools" / "dev" / "js" / "node_modules"


def find(name: str, extra: list[Path]) -> str | None:
    """`name` on PATH, or at one of `extra` — a freshly installed runtime is often not on an open shell's PATH."""
    found = shutil.which(name)
    if found:
        return found
    for candidate in extra:
        if candidate.is_file():
            return str(candidate)
    return None


def emit(source: Path, entry: str, out_dir: Path) -> Path:
    """`source` as WGSL, through the sgl tool, so a probe run says something about the compiler rather than a file."""
    # A nested `uv run` inherits this script's own interpreter through the environment and then refuses to start,
    # so dev.py is handed a clean one.
    env = {k: v for k, v in os.environ.items()
           if k not in ("PYTHONHOME", "PYTHONPATH", "PYTHONEXECUTABLE", "VIRTUAL_ENV", "UV_INTERNAL__PARENT_INTERPRETER")}
    result = subprocess.run(
        ["uv", "run", "dev.py", "run", "sgl", "--", "emit", str(source), "--entry", entry, "--target", "wgsl"],
        cwd=REPO, capture_output=True, text=True, env=env)
    lines = result.stdout.splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith("// SGL")), None)
    if start is None:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"sgl emitted nothing for {source} --entry {entry}")

    body = []
    for line in lines[start:]:
        # dev.py's own trailing lines, which are indented and never part of a shader.
        if line.startswith("  -> ") or line.lstrip().startswith(("sgl succeeded", "sgl failed")):
            break
        body.append(line)

    written = out_dir / (source.stem + ".wgsl")
    written.write_text("\n".join(body).rstrip() + "\n", encoding="utf-8")
    return written


def run(label: str, argv: list[str], env: dict[str, str]) -> bool:
    # Flushed: the child writes straight to the same stream, and an unflushed label lands after its own output.
    print(f"--- {label}", flush=True)
    result = subprocess.run(argv, env=env, text=True)
    return result.returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help=".wgsl files, or .sgl files with --entry")
    ap.add_argument("--entry", help="the entry point to compile an .sgl file for")
    ap.add_argument("--out-dir", default=None, help="where emitted WGSL is written (default: beside the source)")
    ap.add_argument("--node", default=None)
    ap.add_argument("--deno", default=None)
    ap.add_argument("--node-modules", default=None, help="a node_modules directory holding the `webgpu` package")
    args = ap.parse_args()

    sources = [Path(f) for f in args.files]
    if any(s.suffix == ".sgl" for s in sources) and not args.entry:
        ap.error("an .sgl file needs --entry")

    out_dir = Path(args.out_dir) if args.out_dir else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    shaders = []
    for source in sources:
        if source.suffix == ".sgl":
            shaders.append(emit(source, args.entry, out_dir or source.parent))
        else:
            shaders.append(source)

    node = args.node or find("node", [])
    deno = args.deno or find("deno", [Path(r"C:\ProgramData\chocolatey\lib\deno\deno.exe")])

    node_modules = args.node_modules or (str(NODE_MODULES) if NODE_MODULES.is_dir() else None)

    ok = True
    ran = False
    if node:
        env = dict(os.environ)
        if node_modules:
            env["NODE_PATH"] = node_modules
        ok &= run("node (Dawn)", [node, str(PROBE), *map(str, shaders)], env)
        ran = True
    if deno:
        ok &= run("deno (wgpu)",
                  [deno, "run", "--allow-read", "--unstable-webgpu", str(PROBE), *map(str, shaders)],
                  dict(os.environ))
        ran = True

    if not ran:
        print("no runtime could be driven; install node with the `webgpu` package, or deno")
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
