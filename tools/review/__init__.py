"""The review tool's flat facade over lib/.

A command reaches the whole machinery as `review.X` without knowing which lib/ group a helper sits in.
Deliberately does not import `cmd`, which would be a cycle.
tools/review/readme.md is the tool, and tools/review/docs/_index.md the design behind it.
"""

from __future__ import annotations

# The one place the review tool couples to shaped-core.
# Colours are the only thing worth sharing, and keeping the import here means a downstream fork edits one line.
from tools.dev.lib.core import console

from .lib.changeset.commits import candidates_for_commit
from .lib.changeset.commits import collect as collect_commit_candidates
from .lib.changeset.ids import allocate, allocate_many, digest_of
from .lib.changeset.ingest import (
    Candidate,
    IngestResult,
    bulk_candidate,
    candidates_for,
    register,
)
from .lib.changeset.ledger import Change, Ledger
from .lib.core.atomic import append_jsonl, read_json, read_jsonl, stat_key, write_atomic, write_json
from .lib.core.config import (
    GOALS,
    TOOL_VERSION,
    ConfigError,
    ReviewConfig,
    dump,
    goal_help,
    load,
    now,
    save,
)
from .lib.core.log import record
from .lib.core.paths import (
    ReviewNameError,
    ReviewPaths,
    default_root,
    entry_slug,
    validate_name,
)
from .lib.entry.answers import Answer, AnswerFile
from .lib.entry.askhash import canonical as ask_canonical
from .lib.entry.askhash import hash_ask
from .lib.entry.generate import COVERAGE_SLUG, OVERVIEW_SLUG
from .lib.entry.generate import coverage_body, coverage_front, ensure as ensure_entry, overview_body, overview_front
from .lib.entry.grammar import (
    BLOCK_TYPES,
    CONTEXT_TIERS,
    OPTION_KINDS,
    SEVERITIES,
    STATES,
    Option,
    ReviewParseError,
)
from .lib.entry.parse import Block, Entry
from .lib.entry.parse import parse_file as parse_entry_file
from .lib.entry.parse import parse_text as parse_entry_text
from .lib.entry.write import (
    append_text,
    check_immutable,
    compose,
    stamp_rounds,
    word_warnings,
    write_entry,
)
from .lib.git.diffparse import FileDiff, Hunk
from .lib.git.diffparse import parse as parse_diff
from .lib.git.run import Commit, Git, GitError
from .lib.space.intervals import IntervalList
from .lib.space.netspace import ADDED, REMOVED, FileAtom, LineSpace
from .lib.space.netspace import build as build_net_space
from .lib.goals.finalize import ARTIFACTS
from .lib.goals.skeleton import describe as describe_group
from .lib.goals.skeleton import finalizer_for, groups_for
from .lib.render.text import render_entry, render_summary
from .lib.space.netspace import space_of

__all__ = [
    "ADDED",
    "ARTIFACTS",
    "Answer",
    "AnswerFile",
    "BLOCK_TYPES",
    "Block",
    "CONTEXT_TIERS",
    "COVERAGE_SLUG",
    "Candidate",
    "Change",
    "Commit",
    "ConfigError",
    "Entry",
    "FileAtom",
    "FileDiff",
    "GOALS",
    "Git",
    "GitError",
    "Hunk",
    "IngestResult",
    "IntervalList",
    "Ledger",
    "LineSpace",
    "OPTION_KINDS",
    "OVERVIEW_SLUG",
    "Option",
    "REMOVED",
    "ReviewConfig",
    "ReviewNameError",
    "ReviewParseError",
    "ReviewPaths",
    "SEVERITIES",
    "STATES",
    "TOOL_VERSION",
    "allocate",
    "allocate_many",
    "append_jsonl",
    "append_text",
    "ask_canonical",
    "build_net_space",
    "bulk_candidate",
    "candidates_for_commit",
    "collect_commit_candidates",
    "candidates_for",
    "check_immutable",
    "compose",
    "console",
    "coverage_body",
    "coverage_front",
    "default_root",
    "describe_group",
    "digest_of",
    "dump",
    "ensure_entry",
    "entry_slug",
    "finalizer_for",
    "goal_help",
    "groups_for",
    "hash_ask",
    "load",
    "now",
    "overview_body",
    "overview_front",
    "parse_diff",
    "parse_entry_file",
    "parse_entry_text",
    "read_json",
    "read_jsonl",
    "record",
    "register",
    "render_entry",
    "render_summary",
    "save",
    "space_of",
    "stamp_rounds",
    "stat_key",
    "validate_name",
    "word_warnings",
    "write_atomic",
    "write_entry",
    "write_json",
]
