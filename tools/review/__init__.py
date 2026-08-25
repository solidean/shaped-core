"""The review tool's flat facade over lib/.

A command reaches the whole machinery as `review.X` without knowing which lib/ group a helper sits in.
Deliberately does not import `cmd`, which would be a cycle.
tools/review/readme.md is the tool, and tools/review/docs/_index.md the design behind it.
"""

from __future__ import annotations

# The one place the review tool couples to shaped-core.
# Colours are the only thing worth sharing, and keeping the import here means a downstream fork edits one line.
from tools.dev.lib.core import console

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
from .lib.git.diffparse import FileDiff, Hunk
from .lib.git.diffparse import parse as parse_diff
from .lib.git.run import Commit, Git, GitError
from .lib.space.intervals import IntervalList
from .lib.space.netspace import ADDED, REMOVED, FileAtom, LineSpace
from .lib.space.netspace import build as build_net_space
from .lib.space.netspace import space_of

__all__ = [
    "ADDED",
    "GOALS",
    "REMOVED",
    "TOOL_VERSION",
    "Candidate",
    "Change",
    "Commit",
    "ConfigError",
    "FileAtom",
    "FileDiff",
    "Git",
    "GitError",
    "Hunk",
    "IngestResult",
    "IntervalList",
    "Ledger",
    "LineSpace",
    "ReviewConfig",
    "ReviewNameError",
    "ReviewPaths",
    "allocate",
    "allocate_many",
    "append_jsonl",
    "build_net_space",
    "bulk_candidate",
    "candidates_for",
    "console",
    "default_root",
    "digest_of",
    "dump",
    "entry_slug",
    "goal_help",
    "load",
    "now",
    "parse_diff",
    "read_json",
    "read_jsonl",
    "record",
    "register",
    "save",
    "space_of",
    "stat_key",
    "validate_name",
    "write_atomic",
    "write_json",
]
