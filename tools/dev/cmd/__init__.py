"""Command entry points for dev.py — one module per command.

Each exposes `NAME`, `add_parser(sub)` and `run(args, ctx)`; dev.py owns the registry that imports them.
The shared `Context` / `Policy` seam they all run against is context.py, and docs/dev-py-driver.md is the design.
"""

from __future__ import annotations

from .context import Context, Policy

__all__ = ["Context", "Policy"]
