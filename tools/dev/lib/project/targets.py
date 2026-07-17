"""Target discovery via the CMake File API (codemodel-v2).

We ask CMake to emit a codemodel by writing an empty query file before
configure; after configure the reply describes every target, its type, and the
real paths of its built artifacts. This is generator-agnostic and needs no
project-specific knowledge (no `bin/` assumption, no name globbing) — it is the
authoritative source for "what got built and where".
"""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path

from ..core.models import Target


class NotConfiguredError(Exception):
    """Raised when the File API reply is missing — i.e. configure hasn't run."""


_API_DIR = Path(".cmake") / "api" / "v1"
# Non-executable artifact suffixes we never want to treat as the runnable file.
_NON_EXE_SUFFIXES = {".pdb", ".ilk", ".lib", ".exp", ".manifest", ".dll.a"}


def write_query(build_dir: Path) -> None:
    """Request a codemodel-v2 reply on the next configure.

    The query is a marker file; its mere existence makes CMake write the reply.
    Safe to call repeatedly and before the build dir otherwise exists.
    """
    query_dir = build_dir / _API_DIR / "query"
    query_dir.mkdir(parents=True, exist_ok=True)
    (query_dir / "codemodel-v2").touch()


def _reply_dir(build_dir: Path) -> Path:
    return build_dir / _API_DIR / "reply"


def _load_index(build_dir: Path) -> dict:
    reply_dir = _reply_dir(build_dir)
    indices = sorted(reply_dir.glob("index-*.json"))
    if not indices:
        raise NotConfiguredError(
            f"No CMake File API reply in {reply_dir}. Run configure first."
        )
    with open(indices[-1], encoding="utf-8") as f:
        return json.load(f)


def _codemodel_file(build_dir: Path, index: dict) -> Path:
    reply_dir = _reply_dir(build_dir)
    for obj in index.get("objects", []):
        if obj.get("kind") == "codemodel":
            return reply_dir / obj["jsonFile"]
    raise NotConfiguredError("CMake File API index has no codemodel object.")


def _pick_configuration(codemodel: dict, build_type: str) -> dict:
    configs = codemodel.get("configurations", [])
    if not configs:
        raise NotConfiguredError("CMake codemodel has no configurations.")
    if build_type:
        for cfg in configs:
            if cfg.get("name", "").lower() == build_type.lower():
                return cfg
    return configs[0]  # single-config generators have exactly one


def _primary_artifact(target_data: dict, build_dir: Path) -> Path | None:
    """Resolve the primary runnable/linkable artifact path, skipping side files."""
    artifacts = target_data.get("artifacts", [])
    paths = [a["path"] for a in artifacts if "path" in a]
    # Prefer an artifact whose suffix is not a known side file (.pdb/.lib/...).
    primary = next(
        (p for p in paths if Path(p).suffix.lower() not in _NON_EXE_SUFFIXES),
        paths[0] if paths else None,
    )
    if primary is None:
        return None
    p = Path(primary)
    return p if p.is_absolute() else (build_dir / p).resolve()


def load_target_models(build_dir: Path, build_type: str) -> dict[str, dict]:
    """Map each target name to its raw File API target JSON for the build.

    This is the authoritative per-target description (compile groups, link
    fragments, sources, artifacts). `discover_targets` is the summarized view on
    top of it; callers that need the full detail (e.g. flag inspection) take the
    raw dict. First definition wins on duplicate names, matching discovery.
    """
    index = _load_index(build_dir)
    codemodel_path = _codemodel_file(build_dir, index)
    with open(codemodel_path, encoding="utf-8") as f:
        codemodel = json.load(f)

    config = _pick_configuration(codemodel, build_type)
    reply_dir = _reply_dir(build_dir)

    models: dict[str, dict] = {}
    for ref in config.get("targets", []):
        with open(reply_dir / ref["jsonFile"], encoding="utf-8") as f:
            target_data = json.load(f)
        name = target_data["name"]
        if name not in models:
            models[name] = target_data
    return models


def discover_targets(build_dir: Path, build_type: str) -> list[Target]:
    """Enumerate all CMake targets for the given build, with artifact paths."""
    targets = [
        Target(
            name=name,
            kind=data.get("type", "UNKNOWN"),
            artifact=_primary_artifact(data, build_dir),
        )
        for name, data in load_target_models(build_dir, build_type).items()
    ]
    targets.sort(key=lambda t: t.name)
    return targets


def executables(targets: list[Target]) -> list[Target]:
    """Filter to executable targets only."""
    return [t for t in targets if t.kind == "EXECUTABLE"]


def select_test_binaries(
    all_targets: list[Target],
    *,
    is_test: Callable[[Target], bool],
    wanted_names: list[str] | None = None,
    name_arg: str | None = None,
    target_label: object = None,
) -> tuple[list[str], str | None, str | None]:
    """Pick which test binaries to run and the optional test-name filter.

    Three modes, in order: `wanted_names` (from --target) selects matching
    test binaries, keeping `name_arg` as a test-name filter across them; a
    `name_arg` that names a test binary runs just that one; otherwise `name_arg`
    is a name filter applied across every `is_test` binary. Returns
    `(binary_names, test_name, error)` — `error` is a message string when nothing
    matched (the caller decides how to surface it), else None.

    Both name lookups are restricted to `is_test` binaries: the repo also builds
    non-test executables (tools/), and running one as a test would just feed it
    `--junit-xml` and report the resulting usage error as a test failure.
    """
    test_targets = [t for t in all_targets if is_test(t)]

    if wanted_names is not None:
        wanted = set(wanted_names)
        names = [t.name for t in test_targets if t.name in wanted]
        if not names:
            return [], None, f"No test binary matches --target {target_label}"
        return names, name_arg, None

    if name_arg:
        named = next((t for t in test_targets if t.name == name_arg), None)
        if named is not None:
            return [named.name], None, None
        names = [t.name for t in test_targets]
        if not names:
            return [], None, "No test binaries found (expected '*-test' executables)"
        return names, name_arg, None

    names = [t.name for t in test_targets]
    if not names:
        return [], None, "No test binaries found (expected '*-test' executables)"
    return names, None, None
