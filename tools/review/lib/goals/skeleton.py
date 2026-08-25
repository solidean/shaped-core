"""What a review's goal implies about its shape.

A goal is not a label: it decides which groups the navigation has, whether there is a changeset to account for at all,
and what the review ends by producing.
Those differ enough that guessing one would be worse than refusing to start without it.
"""

from __future__ import annotations

# The reading order of the navigation, per goal.
# Meta first because it is the context everything below leans on, nits and LGTM last because they are skimmed.
_CHANGE_GROUPS = (
    "meta",
    "design",
    "correctness",
    "api-shape",
    "testing",
    "docs",
    "nits",
    "lgtm",
    "tooling",
    "finalize",
)

_DESIGN_GROUPS = (
    "framing",
    "topics",
    "open-questions",
    "tooling",
    "finalize",
)

_DESCRIPTIONS = {
    "meta": "what this change is, the API it moves, and the examples that show it",
    "design": "the calls that outlive the code, ranked first because they are hardest to undo",
    "correctness": "what the type system does not catch",
    "api-shape": "signatures, ownership, and whether the mistake is expressible",
    "testing": "what is pinned, and what a regression would slip past",
    "docs": "claims the branch no longer supports",
    "nits": "everything that costs a minute",
    "lgtm": "fine as it stands, here to be skimmed and discharged",
    "tooling": "friction in the review tool itself rather than in what is being reviewed",
    "finalize": "the round's end artifact",
    "framing": "what problem this is, and what would count as solved",
    "topics": "one group per area under discussion",
    "open-questions": "what is not decided yet",
}


def groups_for(goals: list[str]) -> tuple[str, ...]:
    """The navigation groups a review with these goals uses."""
    return _DESIGN_GROUPS if goals == ["design"] else _CHANGE_GROUPS


def describe(group: str) -> str:
    return _DESCRIPTIONS.get(group, "")


def finalizer_for(goals: list[str]) -> str:
    """Which end artifact the finalize entry produces, when a review carries more than one goal.

    A comment for someone else is the most constrained artifact, so it wins where both apply.
    """
    for goal in ("pr-comment", "land-changes", "design"):
        if goal in goals:
            return goal
    return "design"
