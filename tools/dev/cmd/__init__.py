"""Command entry points for dev.py — one module per command.

Each command module exposes `NAME`, `add_parser(sub)` (which registers its own
subparser, including any nested subcommands), and `run(args, ctx)`. dev.py
imports the modules and owns the command registry (its `COMMANDS` list is the map
of the CLI); this package only provides the shared `Context` / `Policy` seam they
all run against.
"""

from __future__ import annotations

from .context import Context, Policy

__all__ = ["Context", "Policy"]
