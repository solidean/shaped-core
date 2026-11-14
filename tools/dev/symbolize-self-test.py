#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/cmd/symbolize.py — reading wasm code offsets out of a report — and for lib/pipeline/sites.py,
which resolves the native opaque sites a failed test printed.

The extraction is the part that fails quietly.
A pattern that misses the shape clean-core actually prints reports "no offsets found" and looks like a tooling
problem rather than a parsing one; a pattern that is too eager pulls a hex number out of an unrelated line and
resolves it to a confident, wrong name.
Order and duplicates matter as much as the values: this is a STACK, and a recursive call site appears once per
level.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/symbolize-self-test.py [-v]
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.cmd.symbolize import _JS_FRAME_BIT, _offsets_in  # noqa: E402
from tools.dev.lib.pipeline.sites import Frame, find_sites, outside_site_header, parse_atos, parse_frames  # noqa: E402

VERBOSE = "-v" in sys.argv


def test_reads_the_shape_cc_stacktrace_prints() -> None:
    # What cc::stacktrace's wasm arm renders for a build with no name section.
    text = "wasm+0x34f\nwasm+0x358\n"
    assert _offsets_in(text) == [0x34F, 0x358]


def test_reads_the_shape_the_engine_prints() -> None:
    # The engine's own frame text, which turns up in anything captured before the parser saw it.
    text = "    at app.wasm.render (wasm://wasm/app.wasm-0012cc2a:wasm-function[26]:0x9f3)\n"
    assert _offsets_in(text) == [0x9F3]


def test_reads_a_named_frame_and_ignores_its_name() -> None:
    # The full line cc::stacktrace prints when the build DID keep names; the offset is still the thing to resolve.
    text = "render_frame (wasm+0x9f3)\n"
    assert _offsets_in(text) == [0x9F3]


def test_order_is_the_stack_order() -> None:
    text = "wasm+0x300\nwasm+0x100\nwasm+0x200\n"
    assert _offsets_in(text) == [0x300, 0x100, 0x200]


def test_duplicates_are_kept_because_recursion_is_real() -> None:
    # A recursive call site is one address repeated once per level, and collapsing it would lose the depth.
    text = "wasm+0x473\n" * 4
    assert _offsets_in(text) == [0x473] * 4


def test_one_offset_per_line_even_when_a_line_holds_two() -> None:
    # A line naming two frames is not a thing any writer here produces, and guessing which one is meant would be
    # worse than taking the first.
    text = "wasm+0x100 wasm+0x200\n"
    assert _offsets_in(text) == [0x100]


def test_unrelated_text_yields_nothing() -> None:
    text = "reason: a test exceeded its deadline\n  test \"some/test\" running for 2.0s\n"
    assert _offsets_in(text) == []


def test_a_bare_offset_alone_on_a_line_counts() -> None:
    assert _offsets_in("  0x4d1  \n") == [0x4D1]


def test_a_hex_number_inside_a_sentence_does_not() -> None:
    # The eager pattern is the dangerous one: this would resolve to a name that means nothing.
    assert _offsets_in("the arena is 0x400000 bytes\n") == []


def test_js_frames_are_tagged_so_the_caller_can_drop_them() -> None:
    # A JS frame's address is a script line with the top bit set, and resolving one against the module is nonsense.
    # symbolize() filters on this bit, so what matters here is that the bit survives extraction.
    tagged = 0x80000433
    assert _offsets_in(f"0x{tagged:x}\n") == [tagged]
    assert tagged & _JS_FRAME_BIT


def test_sites_read_the_shape_describe_site_prints() -> None:
    text = "check failed: x + 1 == 4 at math-test.exe+0x12d6c\n  in test at math-test.exe+0x12fd8\n"
    assert find_sites(text) == [("math-test.exe", 0x12D6C), ("math-test.exe", 0x12FD8)]


def test_sites_are_distinct_in_first_mention_order() -> None:
    # A test failing one check a thousand times names one site, and the report should name it once.
    text = "at a-test+0x20\nat a-test+0x10\nat a-test+0x20\n"
    assert find_sites(text) == [("a-test", 0x20), ("a-test", 0x10)]


def test_sites_skip_an_offset_inside_an_expression() -> None:
    # Whether a name is a module is the disk's to say; an expression's last term never even gets that far.
    assert find_sites("n+1+0x10 and wasm+0x34f") == [("wasm", 0x34F)]


def test_symbolizer_blocks_split_into_frames() -> None:
    out = (
        r"cc::source_location::current()" "\n" r"C:\src\clean-core\platform\source_location.hh:94:0" "\n"
        r"_nx_test_fn_4" "\n" r"C:\src\round-test.cc:42:0" "\n\n"
        "??\n??:0:0\n\n"
    )
    blocks = parse_frames(out, 3)
    assert len(blocks) == 3 and blocks[2] == []
    assert blocks[0][1] == Frame("_nx_test_fn_4", r"C:\src\round-test.cc", 42)
    assert blocks[1] == [Frame("??", "??", 0)]


def test_atos_blocks_split_into_frames() -> None:
    # what `atos -fullPath -i` printed for a resolved address, one in the Mach-O header, and one past every symbol
    out = (
        "cc::source_location::current() (in a-test) (/src/clean-core/platform/source_location.hh:94)\n"
        "_nx_test_fn_4 (in a-test) (/src/round-test.cc:42)\n"
        "\n"
        "\n"
        "\n"
        "0x1fff00000\n"
        "\n"
        "main (in a-test) + 12\n"
        "\n"
    )
    blocks = parse_atos(out, 4)
    assert len(blocks) == 4
    assert blocks[0][1] == Frame("_nx_test_fn_4", "/src/round-test.cc", 42)
    assert blocks[1] == []
    assert blocks[2] == [Frame("0x1fff00000", "??", 0)]
    assert blocks[3] == [Frame("main", "??", 0)]


def test_the_site_header_frames_are_skipped() -> None:
    frames = [
        Frame("current", "x/libs/base/clean-core/src/clean-core/platform/source_location.hh", 94),
        Frame("current", r"x\clean-core\platform\source_location.hh", 90),
        Frame("check", "lib/round-test.cc", 42),
        Frame("caller", "lib/other.cc", 7),
    ]
    assert [f.function for f in outside_site_header(frames)] == ["check", "caller"]


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]

    failed = 0
    for t in tests:
        try:
            t()
        except Exception as e:  # noqa: BLE001 — a self-test reports every failure rather than stopping at the first
            failed += 1
            print(f"FAIL {t.__name__}: {type(e).__name__}: {e}")
        else:
            if VERBOSE:
                print(f"ok   {t.__name__}")

    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
