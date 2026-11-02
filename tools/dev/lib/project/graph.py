"""Which targets a change touches, from the CMake File API rather than from paths anyone writes down.

The codemodel lists every target's sources, the directory whose CMakeLists defined it, and the targets it depends on.
That is enough to answer "which test binaries can this change affect" exactly, and it never rots: a moved file moves in the reply too.

A changed file is attributed in two steps.
A source of a target belongs to that target.
Anything else under a target's source directory belongs to every target defined there, which is what catches a shader file, a corpus or a data file no target lists as a source.
From the owners, the transitive dependents are what the change can affect.

A file neither step attributes is the caller's decision: the policy of which files are build inputs for everything, and which are prose for nothing, lives with the command that asks.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from .targets import load_target_models


@dataclass
class TargetGraph:
    """Ownership and dependency edges over target names, with every path repo-relative POSIX."""

    #: file -> the targets listing it as a source.
    owners: dict[str, set[str]] = field(default_factory=dict)
    #: source directory -> the targets defined there.
    defined_in: dict[str, set[str]] = field(default_factory=dict)
    #: target -> the targets that depend on it directly.
    dependents: dict[str, set[str]] = field(default_factory=dict)

    @staticmethod
    def load(build_dir: Path, build_type: str, *, root: Path) -> TargetGraph:
        """Read one build's codemodel.

        A source outside the repo (a generated file, a fetched upstream) is left out of `owners`, since no change under `root` names it.
        """
        models = load_target_models(build_dir, build_type)
        by_id = {data["id"]: name for name, data in models.items()}
        graph = TargetGraph()
        for name, data in models.items():
            graph.dependents.setdefault(name, set())
            for src in data.get("sources", []):
                rel = _repo_relative(src.get("path", ""), root)
                if rel is not None:
                    graph.owners.setdefault(rel, set()).add(name)
            source_dir = _repo_relative(data.get("paths", {}).get("source", ""), root)
            if source_dir is not None:
                graph.defined_in.setdefault(source_dir, set()).add(name)
            for dep in data.get("dependencies", []):
                dep_name = by_id.get(dep.get("id", ""))
                if dep_name is not None:
                    graph.dependents.setdefault(dep_name, set()).add(name)
        return graph

    def owners_of(self, rel_posix: str) -> set[str] | None:
        """The targets a changed file belongs to, or None when nothing claims it.

        The deepest source directory wins, so a file under a library's `tests/` belongs to the test binary defined there, not to the library.
        """
        direct = self.owners.get(rel_posix)
        if direct:
            return set(direct)
        parts = rel_posix.split("/")
        for depth in range(len(parts) - 1, -1, -1):
            prefix = "/".join(parts[:depth]) if depth else "."
            here = self.defined_in.get(prefix)
            if here:
                return set(here)
        return None

    def affected(self, targets: set[str]) -> set[str]:
        """`targets` and everything that depends on one of them, transitively."""
        seen: set[str] = set()
        stack = list(targets)
        while stack:
            t = stack.pop()
            if t in seen:
                continue
            seen.add(t)
            stack.extend(self.dependents.get(t, ()))
        return seen


def _repo_relative(path: str, root: Path) -> str | None:
    """A codemodel path as a repo-relative POSIX string, or None when it lies outside the repo or in the build tree.

    The reply writes a path under the top-level source directory relative to it, and any other path absolute.
    """
    if not path:
        return None
    p = Path(path)
    if p.is_absolute():
        try:
            p = p.resolve().relative_to(root.resolve())
        except ValueError:
            return None
    rel = p.as_posix()
    if rel.startswith("build/") or rel == "build":
        return None
    return rel
