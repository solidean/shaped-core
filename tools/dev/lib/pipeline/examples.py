"""Example discovery: which `EXAMPLE` declarations exist, and which binary carries the one you named.

An example name is a slash path (`clean-core/vector`) that says nothing about the binary holding it, so resolution is a
cross-binary lookup: probe every `*-example` target with `--list-tests-json - --examples` and merge the answers.
That is the same nexus query `eligibility.py` uses before a filtered test run.

Probing the whole corpus on every invocation would not scale, so listings are cached per artifact under the build dir
and revalidated by (mtime, size) — a rebuilt binary re-probes, an untouched one does not.

An example binary may also carry ordinary TESTs — the machinery a bigger example grew is worth pinning — so
`drop_testless_examples` is what lets `dev.py test` include the ones that do and skip the ones that do not.

Public API:
    collect_examples(preset, targets, root, ...) -> list[Example]
    resolve_example(examples, match) -> (example, diagnostic)
    drop_testless_examples(preset, targets, names, ...) -> list[str]
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from ..core.models import Preset, Target
from ..toolchain import jsruntime as jsr
from .eligibility import _suggest, query_listing

_CACHE_FILE = "example-listings.json"


@dataclass(frozen=True)
class Example:
    """One `EXAMPLE` declaration, and the binary that carries it."""

    name: str  # the slash path the EXAMPLE was declared with
    target: str  # the *-example target holding it
    file: str
    line: int


def capture_directory(preset: Preset, example_name: str, shot: str = "") -> Path:
    """Where one capture's artifacts land: `build/<preset>/captures/<example>/<shot>/`.

    Under the build directory rather than beside the example, because a capture must never dirty the source tree — it is runnable by anyone, at any time, including from CI.
    Copying the image next to its example is the separate refresh step, and only a capture that succeeded is ever copied.

    That location also inherits what the build directory already has: the gitignore, the log archiving, and the CI
    upload that makes a runner-only failure diagnosable.
    """
    parts = example_name.split("/")
    return preset.build_dir / "captures" / Path(*parts) / (shot or "default")


def is_example_target(target: Target) -> bool:
    """Project convention: example executables are named '*-example'."""
    return target.kind == "EXECUTABLE" and target.name.endswith("-example")


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


def collect_examples(
    preset: Preset,
    targets: list[Target],
    *,
    root: Path,
    binary_names: list[str] | None = None,
    launcher: jsr.LazyLauncher | None = None,
) -> list[Example]:
    """Every example in the selected `*-example` binaries, sorted by name.

    `binary_names` narrows which targets are probed (that is what `--target` does); None probes them all.
    A binary that cannot answer the query contributes nothing — it is not an error, it just holds no examples we can see.
    """
    selected = [t for t in targets if is_example_target(t)]
    if binary_names is not None:
        wanted = set(binary_names)
        selected = [t for t in selected if t.name in wanted]

    cache_path = preset.build_dir / _CACHE_FILE
    cache = _load_cache(cache_path)
    fresh: dict = {}

    examples: list[Example] = []
    for target in selected:
        stamp = _artifact_stamp(target.artifact)
        cached = cache.get(target.name)
        if stamp is not None and cached is not None and cached.get("stamp") == stamp:
            records = cached.get("tests", [])
        else:
            listing = query_listing(preset, target, test_name=None, extra_args=["--examples"], root=root, launcher=launcher)
            if listing is None:
                continue
            records = [t for t in listing.tests if t.get("bucket") == "example"]

        if stamp is not None:
            fresh[target.name] = {"stamp": stamp, "tests": records}

        for record in records:
            examples.append(
                Example(
                    name=str(record.get("name", "")),
                    target=target.name,
                    file=str(record.get("file", "")),
                    line=int(record.get("line", 0)),
                )
            )

    if fresh != cache:
        _store_cache(cache_path, fresh)

    return sorted(examples, key=lambda e: e.name)


def select_examples(examples: list[Example], match: str) -> list[Example]:
    """Every example `match` selects, for a sweep — the many-match sibling of `resolve_example`.

    Deliberately not the same function.
    Running one example must refuse an ambiguous match, because running "the first one that matched" would silently run
    the wrong thing.
    A sweep wants exactly the opposite, and an empty match means the whole corpus.

    That is only safe because a sweep is headless.
    `--all` is refused for running examples precisely because it would open every window there is, and a capture opens none.
    """
    if not match:
        return list(examples)

    needle = match.lower()
    return [e for e in examples if needle in e.name.lower()]


def resolve_example(examples: list[Example], match: str) -> tuple[Example | None, str | None]:
    """The one example `match` names, or a diagnostic explaining why not.

    An exact name wins outright, so an example whose name is a prefix of another is always reachable.
    Otherwise the match is a case-insensitive substring, and anything but exactly one hit is an error:
    running "the first one that matched" would silently run the wrong example.
    """
    if not examples:
        return None, "no examples found. Is SC_BUILD_EXAMPLES on, and has anything declared an EXAMPLE?"

    exact = [e for e in examples if e.name == match]
    if len(exact) == 1:
        return exact[0], None
    if len(exact) > 1:
        binaries = ", ".join(sorted(e.target for e in exact))
        return None, f"example {match!r} is declared in several binaries: {binaries}"

    needle = match.lower()
    hits = [e for e in examples if needle in e.name.lower()]
    if len(hits) == 1:
        return hits[0], None
    if len(hits) > 1:
        names = "\n  ".join(f"{e.name}  ({e.target})" for e in hits)
        return None, f"{match!r} matches {len(hits)} examples:\n  {names}"

    message = f"no example matches {match!r}"
    if suggestions := _suggest(match, [e.name for e in examples]):
        message += ". Did you mean: " + ", ".join(suggestions)
    return None, message


def drop_testless_examples(
    preset: Preset,
    targets: list[Target],
    binary_names: list[str],
    *,
    is_example,
    test_name: str | None,
    root: Path,
    extra_args: list[str] | None = None,
) -> list[str]:
    """`binary_names` minus the example binaries that carry no ordinary test.

    An example binary is a legitimate place for TESTs, so `dev.py test` runs the ones that have them.
    Most have none — their EXAMPLEs live in another bucket entirely — and a sweep that ran one anyway would fail on
    "the current schedule did not select any tests", which is exactly right for a test binary and wrong for this.

    Test binaries are never dropped: an empty one IS a failure, and this must not quietly turn that into a pass.
    A binary that cannot answer the query is kept, so a probe that fails never silently removes coverage.
    """
    by_name = {t.name: t for t in targets}
    out: list[str] = []
    for name in binary_names:
        target = by_name.get(name)
        if target is None or not is_example(target):
            out.append(name)
            continue
        listing = query_listing(
            preset, target, test_name=test_name, extra_args=list(extra_args or []), root=root
        )
        if listing is None or listing.eligible_count > 0 or listing.eligible_alias_count > 0:
            out.append(name)
    return out
