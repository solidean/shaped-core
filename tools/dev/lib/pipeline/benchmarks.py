"""Benchmark discovery: which `BENCHMARK` declarations exist, and which binary carries each one.

A benchmark name says nothing about the binary holding it, so resolution is a cross-binary lookup: probe every
`*-test` target with `--list-tests-json - --benchmarks` and merge the answers.
That is the same nexus query `examples.py` and `eligibility.py` use, against a different bucket.

Selection follows `dev.py test` rather than `dev.py example`: a pattern may select several.
Running a family of related benchmarks together is the normal case, and forcing one at a time would defeat the
comparison a benchmark body exists to make.

Probing the whole corpus on every invocation would not scale, so listings are cached per artifact under the build dir
and revalidated by (mtime, size) — a rebuilt binary re-probes, an untouched one does not.

Public API:
    collect_benchmarks(preset, targets, root, ...) -> list[Benchmark]
    select_benchmarks(benchmarks, match) -> (selected, diagnostic)
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from ..core.models import Preset, Target
from ..toolchain import jsruntime as jsr
from .eligibility import _suggest, query_listing

_CACHE_FILE = "benchmark-listings.json"


@dataclass(frozen=True)
class Benchmark:
    """One `BENCHMARK` declaration, and the binary that carries it."""

    name: str
    target: str
    file: str
    line: int


def _artifact_stamp(artifact: Path | None) -> list | None:
    """(mtime, size) for the cache key, or None when the artifact is not there to stamp."""
    try:
        st = artifact.stat() if artifact is not None else None
    except OSError:
        return None
    return [st.st_mtime_ns, st.st_size] if st is not None else None


def _load_cache(path: Path) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}  # a corrupt cache is a miss, never an error — it only ever costs a re-probe


def _store_cache(path: Path, cache: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(cache, f)
    except OSError:
        pass  # an unwritable build dir must not fail the command it was speeding up


def collect_benchmarks(
    preset: Preset,
    targets: list[Target],
    *,
    root: Path,
    binary_names: list[str] | None = None,
    launcher: jsr.LazyLauncher | None = None,
) -> list[Benchmark]:
    """Every benchmark in the selected `*-test` binaries, sorted by name.

    `binary_names` narrows which targets are probed (that is what `--target` does); None probes them all.
    A binary that cannot answer the query contributes nothing — not an error, it just holds no benchmarks we can see.
    """
    selected = [t for t in targets if t.kind == "EXECUTABLE" and t.name.endswith("-test")]
    if binary_names is not None:
        wanted = set(binary_names)
        selected = [t for t in selected if t.name in wanted]

    cache_path = preset.build_dir / _CACHE_FILE
    cache = _load_cache(cache_path)
    fresh: dict = {}

    benchmarks: list[Benchmark] = []
    for target in selected:
        stamp = _artifact_stamp(target.artifact)
        cached = cache.get(target.name)
        if stamp is not None and cached is not None and cached.get("stamp") == stamp:
            records = cached.get("tests", [])
        else:
            listing = query_listing(preset, target, test_name=None, extra_args=["--benchmarks"], root=root, launcher=launcher)
            if listing is None:
                continue
            records = [t for t in listing.tests if t.get("bucket") == "benchmark"]

        if stamp is not None:
            fresh[target.name] = {"stamp": stamp, "tests": records}

        for record in records:
            benchmarks.append(
                Benchmark(
                    name=str(record.get("name", "")),
                    target=target.name,
                    file=str(record.get("file", "")),
                    line=int(record.get("line", 0)),
                )
            )

    if fresh != cache:
        _store_cache(cache_path, fresh)

    return sorted(benchmarks, key=lambda b: b.name)


def select_benchmarks(benchmarks: list[Benchmark], match: str) -> tuple[list[Benchmark], str | None]:
    """The benchmarks `match` names, or a diagnostic explaining why none.

    An exact name wins outright and selects only itself, so a benchmark whose name is a prefix of another stays
    reachable.
    Otherwise the match is a case-insensitive substring and every hit is selected — unlike `dev.py example`, where
    ambiguity is an error because running "the first one that matched" would run the wrong thing.
    """
    if not benchmarks:
        return [], "no benchmarks found. Has anything declared a BENCHMARK?"

    exact = [b for b in benchmarks if b.name == match]
    if exact:
        return exact, None

    needle = match.lower()
    hits = [b for b in benchmarks if needle in b.name.lower()]
    if hits:
        return hits, None

    message = f"no benchmark matches {match!r}"
    if suggestions := _suggest(match, [b.name for b in benchmarks]):
        message += ". Did you mean: " + ", ".join(suggestions)
    return [], message
