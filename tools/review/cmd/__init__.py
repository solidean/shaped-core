"""Command entry points for review.py — one module per command.

Each exposes `NAME`, `add_parser(sub)` and `run(args, ctx)`; review.py owns the registry that imports them.
The shared `Context` seam they all run against is context.py.
"""

from __future__ import annotations

from .context import Context

__all__ = ["Context"]
