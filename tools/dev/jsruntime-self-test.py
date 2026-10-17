#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///

"""Self-test for tools/dev/lib/toolchain/jsruntime.py: the launch prefix each runtime gets, V8 flags included, and where deno is found.

A V8 flag goes wrong silently in one direction: deno ignores a flag it does not know, so a misspelled or misplaced one runs the artifact without it.

    uv run tools/dev/jsruntime-self-test.py [-v]
"""

from __future__ import annotations

import os
import sys
import tempfile
import traceback
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.toolchain import jsruntime as jsr  # noqa: E402

VERBOSE = "-v" in sys.argv
WIDE = "--experimental-wasm-wide_arithmetic"


def test_deno_takes_v8_flags_as_one_comma_list_before_the_script(base: Path) -> None:
    rt = jsr.JsRuntime(kind="deno", exe=Path("deno"), version=None, v8_flags=(WIDE, "--no-liftoff"))
    prefix = rt.launch_prefix
    assert prefix[:2] == ["deno", "run"], prefix
    assert prefix[-1] == f"--v8-flags={WIDE},--no-liftoff", prefix


def test_node_takes_v8_flags_as_its_own_options_before_the_script(base: Path) -> None:
    rt = jsr.JsRuntime(kind="node", exe=Path("node"), version=None, v8_flags=(WIDE,))
    assert rt.launch_prefix[:2] == ["node", WIDE], rt.launch_prefix


def test_no_flags_change_nothing(base: Path) -> None:
    assert not any("v8" in a for a in jsr.JsRuntime(kind="deno", exe=Path("deno"), version=None).launch_prefix)
    assert jsr.JsRuntime(kind="node", exe=Path("node"), version=None).launch_prefix[0] == "node"


def test_flags_come_from_the_command_line_before_the_environment(base: Path) -> None:
    os.environ["SC_V8_FLAGS"] = "--a  --b"
    try:
        assert jsr.JsRuntimeRequest.from_args(Namespace(v8_flag=None)).v8_flags == ("--a", "--b")
        assert jsr.JsRuntimeRequest.from_args(Namespace(v8_flag=["--c"])).v8_flags == ("--c",)
    finally:
        del os.environ["SC_V8_FLAGS"]
    assert jsr.JsRuntimeRequest.from_args(Namespace()).v8_flags == ()


def test_the_code_cache_workaround_applies_below_2_9_7_and_to_an_unknown_version(base: Path) -> None:
    def prefix(version: str | None) -> list[str]:
        return jsr.JsRuntime(kind="deno", exe=Path("deno"), version=version).launch_prefix
    assert "--no-code-cache" in prefix("deno 2.9.6 (stable, release, x86_64-pc-windows-msvc)")
    assert "--no-code-cache" in prefix(None)
    assert "--no-code-cache" not in prefix("deno 2.9.7 (stable, release, x86_64-pc-windows-msvc)")
    assert "--no-code-cache" not in prefix("deno 2.10.0 (stable, release, x86_64-pc-windows-msvc)")


def test_deno_version_number(base: Path) -> None:
    assert jsr.deno_version_number("deno 2.9.7 (stable, release, x86_64-pc-windows-msvc)") == "2.9.7"
    assert jsr.deno_version_number(None) is None
    assert jsr.deno_version_number("v24.19.0") is None


def test_the_installed_deno_is_found_ahead_of_path(base: Path) -> None:
    home = base / "home"
    installed = home / "tools" / "deno"
    on_path = base / "bin"
    name = "deno.exe" if os.name == "nt" else "deno"
    for d in (installed, on_path):
        d.mkdir(parents=True)
        (d / name).write_text("")
    saved = {k: os.environ.get(k) for k in ("HOME", "USERPROFILE", "SC_DENO_PATH")}
    os.environ["HOME"] = os.environ["USERPROFILE"] = str(home)
    os.environ.pop("SC_DENO_PATH", None)
    try:
        env = {"PATH": str(on_path)}
        assert jsr._locate("deno", None, env) == installed / name
        assert jsr._locate("deno", str(on_path), env) == on_path / name, "an explicit path still wins"
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def test_the_pin_is_a_version(base: Path) -> None:
    assert jsr.deno_version_number(f"deno {jsr.pinned_deno_version()}") == jsr.pinned_deno_version()
    assert jsr.pinned_deno_version().count(".") == 2


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        with tempfile.TemporaryDirectory(prefix="jsruntime-self-test-") as tmp:
            try:
                t(Path(tmp))
            except Exception as e:
                failed += 1
                print(f"FAIL {t.__name__}: {type(e).__name__}: {e}")
                if VERBOSE:
                    traceback.print_exc()
                continue
        if VERBOSE:
            print(f"ok   {t.__name__}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
