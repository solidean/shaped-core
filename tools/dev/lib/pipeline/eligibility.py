"""Pre-flight test-eligibility query.

Before running a *filtered* `dev.py test`, ask each test binary which of its registered tests the filter actually selects.
The in-repo nexus runner answers `--list-tests-json -` with a JSON listing — every test, plus whether it `would_run` under the given args — and exits without running anything.

The query is best-effort: a binary that does not understand the flag, or cannot be launched, yields `None` and is always kept.
So a binary is skipped only when it *positively* reports zero eligible tests.

Public API:
    select_eligible_binaries(preset, targets, binary_names, ...) -> (runnable, diagnostic)
"""

from __future__ import annotations

import difflib
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..core.models import Preset, Target

# Artifact suffixes that are not directly runnable and must be launched via node.
_WASM_LAUNCH_SUFFIXES = {".js", ".mjs", ".wasm"}

# How a non-normal bucket is re-entered on the CLI, for the "wrong bucket" hint.
_BUCKET_FLAG = {"manual": "--manual", "guide_benchmark": "--guide-benchmarks"}


@dataclass
class BinaryListing:
    """Parsed result of one binary's `--list-tests-json` query."""

    name: str
    eligible_count: int
    eligible_alias_count: int  # matched aliases (a filter can select an alias with no eligible plain test)
    filter_mode: str  # how the runner read the filter: "name" (substring) or "file" (glob over the source file)
    tests: list[dict]  # raw per-test records: name, file, bucket, enabled, filter_matches, eligible, ...


def _launcher(preset: Preset, artifact: Path) -> list[str]:
    if preset.is_emscripten or artifact.suffix.lower() in _WASM_LAUNCH_SUFFIXES:
        return ["node"]
    return []


def query_listing(
    preset: Preset,
    target: Target,
    *,
    test_name: str | None,
    extra_args: list[str],
    root: Path,
    env: dict[str, str] | None = None,
    timeout: float = 30.0,
) -> BinaryListing | None:
    """Run the listing query for one binary; return its listing or None on any failure.

    None means "could not determine" — artifact missing, launch failed, non-zero exit, or unparseable output.
    """
    if target.artifact is None:
        return None
    cmd = [*_launcher(preset, target.artifact), str(target.artifact)]
    if test_name:
        cmd.append(test_name)
    cmd += ["--list-tests-json", "-", *extra_args]
    try:
        proc = subprocess.run(
            cmd, cwd=root, env=env, capture_output=True, text=True, timeout=timeout
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    try:
        data = json.loads(proc.stdout)
    except (json.JSONDecodeError, ValueError):
        return None
    return BinaryListing(
        name=target.name,
        eligible_count=int(data.get("eligible_count", 0)),
        eligible_alias_count=int(data.get("eligible_alias_count", 0)),
        filter_mode=str(data.get("filter_mode", "name")),
        tests=list(data.get("tests", [])),
    )


def _partial_ratio(needle: str, haystack: str) -> float:
    """Best similarity of `needle` against any same-length window of `haystack`.

    A filter is meant as a substring of a test name, so the score is how close the pattern comes to *some* slice of the name.
    A typo'd substring ("shedule") still aligns tightly with its slice ("schedule"), while an unrelated string aligns with none.
    Symmetric in length: the shorter slides over the longer.
    """
    if not needle or not haystack:
        return 0.0
    short, long = (needle, haystack) if len(needle) <= len(haystack) else (haystack, needle)
    width = len(short)
    best = 0.0
    for i in range(0, len(long) - width + 1):
        best = max(best, difflib.SequenceMatcher(None, short, long[i : i + width]).ratio())
        if best == 1.0:
            break
    return best


def _suggest(pattern: str, names: list[str], *, whole: bool = False) -> list[str]:
    """Up to three candidates close to `pattern`, for a "did you mean".

    Test names are descriptive sentences, so a short filter is rarely similar to a whole name.
    Each name is scored by how closely the pattern aligns with a window of it, and only strong, near-substring matches are kept, so an unrelated pattern yields nothing.
    `whole` is for candidates a filter is meant to match in full — a filename is one.
    It drops the window pass, and demands a closer whole-string match, because every filename here ends in `-test.cc` and that shared tail alone scores near the normal cutoff.
    """
    hits = difflib.get_close_matches(pattern, names, n=3, cutoff=0.75 if whole else 0.6)
    if hits or whole:
        return hits

    pat = pattern.lower()
    scored = [(s, name) for name in names if (s := _partial_ratio(pat, name.lower())) >= 0.8]
    scored.sort(key=lambda s: s[0], reverse=True)
    return [name for _, name in scored[:3]]


def _diagnose(test_name: str, zero_listings: list[BinaryListing], extra_args: list[str]) -> str:
    """Build the failure message when a filter matched no eligible test anywhere.

    Prefers a targeted fix when the filter *did* match a test excluded by its bucket or disabled status.
    Otherwise it suggests the closest candidates — test names, or source filenames once the runner fell back to file matching.
    `extra_args` is read only for the `--match-*` pins, which decide what the message may claim was tried.
    """
    matched_but_excluded = [
        t
        for lst in zero_listings
        for t in lst.tests
        if t.get("filter_matches") and not t.get("eligible")
    ]

    n = len(zero_listings)
    binaries = "binary" if n == 1 else "binaries"
    # The runner reports how it ended up reading the filter, so the message and the suggestions follow that rather than guessing from the pattern.
    # A pinned mode is the one case where only one of the two was ever tried.
    by_file = any(lst.filter_mode == "file" for lst in zero_listings)
    if "--match-files" in extra_args:
        how = "by file"
    else:
        how = "by name or by file" if by_file else "by name"
    lines = [f"no test matches {test_name!r} {how} in any of the {n} test {binaries}."]

    if matched_but_excluded:
        lines.append("some tests match the filter but are excluded:")
        seen: set[str] = set()
        for t in matched_but_excluded:
            name = t.get("name", "?")
            if name in seen:
                continue
            seen.add(name)
            if not t.get("enabled", True):
                lines.append(f"  - {name!r} is disabled; name it exactly to run it")
            else:
                bucket = t.get("bucket", "normal")
                flag = _BUCKET_FLAG.get(bucket)
                hint = f"pass {flag} or name it exactly" if flag else "name it exactly"
                lines.append(f"  - {name!r} is in the {bucket!r} bucket; {hint}")
        return "\n".join(lines)

    # Under file matching the filter is compared against source files, so suggest filenames — a test name would be no help.
    if by_file:
        needle = test_name.replace("\\", "/").rstrip("/*").rsplit("/", 1)[-1]
        candidates = sorted(
            {f.replace("\\", "/").rsplit("/", 1)[-1] for lst in zero_listings for t in lst.tests if (f := t.get("file"))}
        )
    else:
        needle = test_name
        candidates = sorted({t.get("name", "") for lst in zero_listings for t in lst.tests if t.get("name")})

    suggestions = _suggest(needle, candidates, whole=by_file)
    if suggestions:
        lines.append("did you mean:")
        lines.extend(f"  - {s!r}" for s in suggestions)
    return "\n".join(lines)


def select_eligible_binaries(
    preset: Preset,
    targets: list[Target],
    binary_names: list[str],
    *,
    test_name: str,
    extra_args: list[str] | None = None,
    root: Path,
    env: dict[str, str] | None = None,
) -> tuple[list[str], str | None]:
    """Narrow `binary_names` to those that contain a test matching `test_name`.

    Queries each binary's listing on `preset`'s artifacts; eligibility is identical across presets, since the registered tests are the same.
    Returns (runnable_names, diagnostic), where diagnostic is None on success.
    When every binary positively reports zero eligible tests, runnable is empty and diagnostic explains why.
    """
    extra_args = list(extra_args or [])
    by_name = {t.name: t for t in targets}

    runnable: list[str] = []
    zero_listings: list[BinaryListing] = []
    for name in binary_names:
        target = by_name.get(name)
        if target is None:
            runnable.append(name)  # unknown to this preset — let the run loop handle it
            continue
        listing = query_listing(
            preset, target, test_name=test_name, extra_args=extra_args, root=root, env=env
        )
        if listing is None:
            runnable.append(name)  # couldn't determine — keep it
        elif listing.eligible_count > 0 or listing.eligible_alias_count > 0:
            runnable.append(name)  # a matching plain test, or an alias the filter selects
        else:
            zero_listings.append(listing)

    if runnable:
        return runnable, None
    return [], _diagnose(test_name, zero_listings, extra_args)
