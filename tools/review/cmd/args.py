"""Argparse fragments shared by more than one command."""

from __future__ import annotations

import argparse


def review_name(p: argparse.ArgumentParser) -> None:
    p.add_argument("name", help="the review's name, which is also its folder name")


def as_json(p: argparse.ArgumentParser) -> None:
    p.add_argument("--json", action="store_true", help="emit machine-readable JSON on stdout instead of a report")
