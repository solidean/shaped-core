#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Run the shared binding corpus against the Python half of the binding pass.

tests/data/binding-corpus.txt is one file of HLSL snippets and the parse each must produce.
Both halves of the pass read it -- this script, and shaped-shader-library-test's own corpus test.
So a grammar case is added once rather than twice, and the two halves cannot drift on a case anybody thought of.

Run by hand as `uv run libs/graphics/shaped-shader-library/cmake/binding-grammar-self-test.py`, and by
`uv run dev.py check` as the `shader-grammar` gate.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from binding_grammar import BindingError, parse_binding_groups  # noqa: E402

CORPUS = Path(__file__).parent.parent / "tests" / "data" / "binding-corpus.txt"


class Case:
    def __init__(self, name: str) -> None:
        self.name = name
        self.hlsl: list[str] = []
        self.groups: list[tuple[str, int, list[dict]]] = []
        self.error: str | None = None

    @property
    def source(self) -> str:
        return "".join(line + "\n" for line in self.hlsl)


def read_corpus(text: str) -> list[Case]:
    cases: list[Case] = []
    section = ""

    for line in text.splitlines():
        if line.startswith("--- case "):
            cases.append(Case(line[len("--- case "):]))
            section = ""
            continue
        if line in ("--- hlsl", "--- groups", "--- error"):
            section = line[4:]
            continue
        if not cases:
            continue

        current = cases[-1]
        if section == "hlsl":
            current.hlsl.append(line)
        elif section == "groups":
            words = line.split()
            if not words or line.startswith("#"):
                continue
            if not line.startswith("  "):
                current.groups.append((words[0], int(words[1].removeprefix("group=")), []))
                continue
            binding = {"name": words[0], "count": 1, "dim": None}
            for word in words[1:]:
                key, _, value = word.partition("=")
                binding["index" if key == "index" else key] = int(value) if key in ("index", "count") else value
            current.groups[-1][2].append(binding)
        elif section == "error" and line and not line.startswith("#"):
            current.error = line

    return cases


def check(case: Case) -> list[str]:
    """The differences between what the case pins and what the parse produced."""
    try:
        groups = parse_binding_groups(case.source)
    except BindingError as e:
        if case.error is None:
            return [f"was rejected: {e}"]
        return [] if str(e) == case.error else [f"reported\n    {e}\n  but must report\n    {case.error}"]

    if case.error is not None:
        return [f"was accepted, but must be rejected with: {case.error}"]

    if len(groups) != len(case.groups):
        return [f"found {len(groups)} group(s), expected {len(case.groups)}"]

    problems: list[str] = []
    for group, (name, number, expected) in zip(groups, case.groups):
        if group.name != name or group.group != number:
            problems.append(f"group '{group.name}' group={group.group}, expected '{name}' group={number}")
            continue
        if len(group.bindings) != len(expected):
            problems.append(f"group '{name}' has {len(group.bindings)} binding(s), expected {len(expected)}")
            continue
        for binding, want in zip(group.bindings, expected):
            got = (binding.name, binding.index, binding.count, binding.type, binding.dimension)
            wanted = (want["name"], want["index"], want["count"], want["type"], want["dim"])
            if got != wanted:
                problems.append(f"binding {got}, expected {wanted}")

    return problems


def main() -> int:
    if not CORPUS.is_file():
        print(f"binding corpus not found at {CORPUS}", file=sys.stderr)
        return 1

    cases = read_corpus(CORPUS.read_text(encoding="utf-8"))
    if len(cases) < 20:
        print(f"only {len(cases)} corpus case(s) read from {CORPUS} — the reader is broken", file=sys.stderr)
        return 1

    failed = 0
    for case in cases:
        problems = check(case)
        if problems:
            failed += 1
            print(f"[corpus] '{case.name}'", file=sys.stderr)
            for problem in problems:
                print(f"  {problem}", file=sys.stderr)

    if failed:
        print(f"\n{failed} of {len(cases)} corpus case(s) failed", file=sys.stderr)
        return 1

    print(f"binding grammar: {len(cases)} corpus case(s) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
