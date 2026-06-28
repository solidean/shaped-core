"""`test-web` — build and open the Emscripten browser test runner via emrun."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "test-web"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Open the browser test runner (Emscripten); serves + opens via emrun")
    a.preset(p)
    a.emsdk(p)
    p.add_argument("library", nargs="?",
                   help="Library to show alone (e.g. clean-core); omit for the combined page of all libraries")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    # The browser test runner is Emscripten-only. No library arg -> the aggregate page (all libraries);
    # a library arg -> just that library's page. Either way we build the module(s), then serve+open the
    # page with emrun (the page loads its wasm test module(s) and runs them one per animation frame).
    presets = ctx.resolve_presets(args.preset or [ctx.policy.web_preset])
    preset = presets[0]
    if not preset.is_emscripten:
        ctx.die(f"test-web needs an Emscripten preset (got {preset.name!r}); e.g. --preset {ctx.policy.web_preset}")

    if args.library:
        # Accept "clean-core", "clean-core-test", or "clean-core-test-web" — all name the same runner.
        lib = args.library.removesuffix("-web").removesuffix("-test")
        target: str | None = f"{lib}-test-web"
        page = f"{lib}-web.html"
    else:
        target = None  # build everything so all modules + the aggregate page are present
        page = "tests-web.html"

    if not args.no_build:
        results = dev.build(
            presets, [target] if target else None, root=ctx.root,
            mirror=args.mirror_output, verbose=args.verbose, emsdk_path=args.emsdk_path,
        )
        if not all(r.ok for r in results):
            ctx.fail_build(results, presets)

    page_path = preset.build_dir / page
    if not page_path.is_file():
        ctx.die(f"no page at {ctx.rel(page_path)} - "
                + (f"library {args.library!r} has no web runner?" if args.library else "build may have failed"))

    env = dev.emsdk_env(args.emsdk_path)
    if env is None:
        ctx.die("emsdk not found - pass --emsdk-path or activate emsdk (see: uv run dev.py doctor)")
    emrun = shutil.which("emrun", path=env.get("PATH"))
    if emrun is None:
        ctx.die("emrun not found in the emsdk environment")

    # emrun serves the page's directory (the build root) so the page can reach its libs/ modules, and
    # opens the default browser. It runs in the foreground until you stop it (Ctrl-C). .bat needs cmd.
    launch = ["cmd", "/c", emrun] if emrun.lower().endswith((".bat", ".cmd")) else [emrun]
    print(console.dim(f"serving {ctx.rel(page_path)} via emrun (Ctrl-C to stop)..."), file=sys.stderr)
    result = subprocess.run([*launch, str(page_path)], env=env, cwd=str(preset.build_dir))
    sys.exit(result.returncode)
