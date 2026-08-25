"""`review.toml` — what a review is pinned to, and what it has reached.

The file is TOML because a human edits it: retargeting a review, or widening its goals, should not need the CLI.
Reading uses `tomllib`; writing is the small emitter below, since the standard library has no writer and the schema is flat.

The base is stored as a **commit sha, resolved once at init**.
A three-dot range re-resolves its merge base on every run, so once the integration branch moves the net diff silently changes underneath every change id already handed out.
Pinning is what makes a review reproducible across days.
"""

from __future__ import annotations

import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .atomic import write_atomic

# Bumped when a change here alters how change ids are derived, which is the one thing a review cannot silently absorb.
TOOL_VERSION = 1

GOALS = ("pr-comment", "land-changes", "design")

_GOAL_HELP = {
    "pr-comment": "the artifact is one standalone comment for the author",
    "land-changes": "the rounds are work orders, landed and verified in this session",
    "design": "no changeset; agreement on a design that does not exist yet",
}


class ConfigError(Exception):
    """An unusable or absent review configuration, carrying the message the CLI should surface."""


def goal_help() -> str:
    return "; ".join(f"{g} — {_GOAL_HELP[g]}" for g in GOALS)


@dataclass
class ReviewConfig:
    """A review's pinned inputs and its progress.

    `base` and `head` are commit shas; `base_spec` and `head_spec` are what the user typed, kept for messages only.
    `watermark` is the last finalized round, so the next one is `watermark + 1`.
    """

    name: str
    goals: list[str]
    base: str = ""
    head: str = ""
    base_spec: str = ""
    head_spec: str = ""
    context: int = 8
    coalesce_gap: int = 20
    watermark: int = 0
    tool_version: int = TOOL_VERSION
    created: str = ""
    title: str = ""
    extra: dict = field(default_factory=dict)

    @property
    def has_changeset(self) -> bool:
        """Whether this review is about a commit range at all.

        A design review is agreement on something that does not exist yet, so it has no diff, no ledger and no coverage gate.
        Any other goal in the list brings the changeset back.
        """
        return self.goals != ["design"]

    @property
    def next_round(self) -> int:
        return self.watermark + 1

    def require_changeset(self) -> None:
        if not self.has_changeset:
            raise ConfigError("this review has goal 'design' only, so it has no commit range and nothing to ingest")
        if not self.base or not self.head:
            raise ConfigError("this review has no pinned base..head; re-run `review init` with --range")


def _quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _emit(key: str, value) -> str:
    if isinstance(value, bool):
        return f"{key} = {'true' if value else 'false'}"
    if isinstance(value, int):
        return f"{key} = {value}"
    if isinstance(value, (list, tuple)):
        return f"{key} = [{', '.join(_quote(str(v)) for v in value)}]"
    return f"{key} = {_quote(str(value))}"


def dump(cfg: ReviewConfig) -> str:
    """Render a config as TOML, with the comments a hand-editor needs."""
    lines = [
        "# A shaped-core review. `uv run review.py --help` is the CLI over it.",
        "# base and head are pinned shas: a moving branch must not change what this review already accounted for.",
        "",
        _emit("name", cfg.name),
        _emit("title", cfg.title),
        _emit("goals", cfg.goals),
        _emit("created", cfg.created),
        _emit("tool_version", cfg.tool_version),
        "",
        "# The pinned range. base is the merge-base resolved once, at init.",
        _emit("base", cfg.base),
        _emit("head", cfg.head),
        _emit("base_spec", cfg.base_spec),
        _emit("head_spec", cfg.head_spec),
        "",
        "# Ingest parameters, recorded so change ids reproduce. Changing them re-ingests.",
        _emit("context", cfg.context),
        _emit("coalesce_gap", cfg.coalesce_gap),
        "",
        "# The last finalized round.",
        _emit("watermark", cfg.watermark),
    ]
    for key, value in sorted(cfg.extra.items()):
        lines.append(_emit(key, value))
    return "\n".join(lines) + "\n"


_KNOWN = {
    "name", "title", "goals", "created", "tool_version",
    "base", "head", "base_spec", "head_spec",
    "context", "coalesce_gap", "watermark",
}


def load(path: Path) -> ReviewConfig:
    """Read a review config, raising ConfigError with an actionable message on anything unusable."""
    if not path.is_file():
        raise ConfigError(f"no review at {path.parent} (no review.toml). `review init` creates one.")
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (tomllib.TOMLDecodeError, OSError) as e:
        raise ConfigError(f"{path} is not readable TOML: {e}") from e

    goals = [str(g) for g in raw.get("goals", [])]
    if not goals:
        raise ConfigError(f"{path} declares no goals. A review without a goal has no end artifact; {goal_help()}")
    for g in goals:
        if g not in GOALS:
            raise ConfigError(f"unknown goal {g!r} in {path}. Known goals: {', '.join(GOALS)}")

    version = int(raw.get("tool_version", 0))
    if version > TOOL_VERSION:
        raise ConfigError(
            f"{path} was written by a newer review tool (version {version} > {TOOL_VERSION}); update the tool or start a new review"
        )

    return ReviewConfig(
        name=str(raw.get("name", path.parent.name)),
        title=str(raw.get("title", "")),
        goals=goals,
        base=str(raw.get("base", "")),
        head=str(raw.get("head", "")),
        base_spec=str(raw.get("base_spec", "")),
        head_spec=str(raw.get("head_spec", "")),
        context=int(raw.get("context", 8)),
        coalesce_gap=int(raw.get("coalesce_gap", 20)),
        watermark=int(raw.get("watermark", 0)),
        tool_version=version,
        created=str(raw.get("created", "")),
        extra={k: v for k, v in raw.items() if k not in _KNOWN},
    )


def save(path: Path, cfg: ReviewConfig) -> None:
    write_atomic(path, dump(cfg))


def now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S")
