"""Which test binaries a change can affect, from the target graph and two policies.

The graph answers ownership and dependency; the two `Watch` policies are the caller's and say what the graph cannot:

- `build_inputs`: a file that feeds every target, a CMakeLists or a preset file, so a change to one selects everything.
- `prose`: a file no test reads, a doc or the harness's own Python, so a change to one selects nothing.

A changed file that is neither and that no target owns selects everything, and the selection says which file it was.
The rules narrow; they never guess.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from ..project.graph import TargetGraph
from .watch import Watch


@dataclass
class TestSelection:
    #: The candidates to run, in the caller's order.
    binaries: list[str]
    #: The candidates the change cannot affect.
    skipped: list[str]
    #: Why every candidate is in `binaries` although the change did not reach each one, empty when selection narrowed.
    reasons: list[str] = field(default_factory=list)

    @property
    def everything(self) -> bool:
        return bool(self.reasons)


def select_test_binaries_for_change(
    graph: TargetGraph,
    paths: list[str],
    candidates: list[str],
    *,
    build_inputs: Watch,
    prose: Watch,
) -> TestSelection:
    """The subset of `candidates` a change to `paths` can affect.

    `build_inputs` wins over `prose`, so a harness file that both policies name is a build input.
    """
    reasons: list[str] = []
    owners: set[str] = set()
    for p in paths:
        if build_inputs.matches(p):
            reasons.append(f"{p} is a build input")
            continue
        if prose.matches(p):
            continue
        here = graph.owners_of(p)
        if here is None:
            reasons.append(f"no target owns {p}")
            continue
        owners |= here
    if reasons:
        return TestSelection(list(candidates), [], reasons)
    affected = graph.affected(owners)
    chosen = [c for c in candidates if c in affected]
    return TestSelection(chosen, [c for c in candidates if c not in affected])
