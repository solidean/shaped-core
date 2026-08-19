"""Read the per-dependency `extern/<dep>/dependency.yml` manifests.

Each dependency directory declares its upstreams — identity, pin and license — in one manifest,
and the `vendor-*.py` / `fetch-*.py` script next to it reads its pin from here rather than from constants of its own.
That is the whole point: a pin is written once.
The copy mechanics (`COPY_MAP`, `WIPE`, `STRIP_PREFIX`, `ARCH_MAP`, rewrites) stay in the scripts, being an executable plan rather than configuration.

This module is imported, not run, so it carries no PEP 723 block — but it needs pyyaml, so every importer must declare it.
`tools/dev/lib/pipeline/prereqs.py` deliberately does not import this: it reads `pin_hash` with a narrow line scan, to stay stdlib-only.

`pin_hash` is uniform across every upstream — whatever a clone's HEAD must resolve to, or whatever `.install/pin.txt` must equal.
`digest_algo` says what it is (`git-commit`, `sha256` or `sha3-256`), because the value alone cannot tell you.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import yaml

MANIFEST_NAME = "dependency.yml"

# `source` says how we obtain the upstream; `track` says how "what is current" is defined.
# They are separate because stb, ImPlot and ImGuizmo are ordinary git clones whose newest version is a branch head, not a tag.
SOURCES = {"git", "github-release", "url"}
TRACKS = {"tags", "default-branch", "github-releases", "sqlite", "none"}
DIGEST_ALGOS = {"git-commit", "sha256", "sha3-256"}
# `vendored` is committed in-tree; `fetched` hydrates a gitignored .install/ on demand, so it can be absent or stale on a given checkout.
# `bundled` arrives inside another upstream in the same directory — Zycore, which the Zydis amalgamation folds in — so it has no install state of its own.
INSTALLS = {"vendored", "fetched", "bundled"}


@dataclass(frozen=True)
class Upstream:
    """One external upstream. A dependency directory holds one or more — imgui holds three, zydis two."""

    name: str
    directory: Path
    source: str
    track: str
    install: str
    pin_hash: str
    digest_algo: str
    license: str
    homepage: str = ""
    repo: str = ""
    tag: str | None = None
    version: str = ""
    year: str = ""
    asset: str = ""
    # For `track: tags`, a regex selecting which tags are versions at all — upstreams tag far more than releases.
    # Empty means the default "looks like a version number" pattern.
    tag_pattern: str = ""
    license_files: list[str] = field(default_factory=list)
    # Verbatim license text, for an upstream that ships no file of its own — sqlite's amalgamation is the only one.
    license_text: str = ""
    used_by: str = ""
    notes: str = ""

    @property
    def url(self) -> str:
        """The archive download URL.

        Only an upstream we download an archive for has one — Zydis is a `github-release` we clone instead, and declares no asset.
        """
        if not self.asset:
            raise ValueError(f"{self.name}: declares no `asset`, so it has no archive URL")
        if self.source == "github-release":
            return f"{self.repo}/releases/download/{self.tag}/{self.asset}"
        if self.source == "url":
            return f"https://sqlite.org/{self.year}/{self.asset}"
        raise ValueError(f"{self.name}: source {self.source!r} has no archive URL")

    @property
    def is_fetched(self) -> bool:
        return self.install == "fetched"

    @property
    def install_dir(self) -> Path:
        """Where a fetched dependency hydrates; meaningless for a vendored one."""
        return self.directory / ".install"

    @property
    def pin_file(self) -> Path:
        """The file whose content must equal `pin_hash` for a fetched install to be current."""
        return self.install_dir / "pin.txt"

    def installed_pin(self) -> str | None:
        """What the install on disk is actually at, or None when nothing is installed."""
        if not self.is_fetched or not self.pin_file.is_file():
            return None
        return self.pin_file.read_text(encoding="utf-8").strip()

    @property
    def slug(self) -> str:
        """Filename-safe form of the name, used for `docs/licenses/<slug>.txt`."""
        return self.name.lower().replace(" ", "-")

    def license_paths(self) -> list[Path]:
        """`license_files` resolved against the dependency directory."""
        return [self.directory / p for p in self.license_files]


def manifest_path(directory: Path) -> Path:
    return directory / MANIFEST_NAME


def load(directory: Path) -> list[Upstream]:
    """Every upstream declared in `<directory>/dependency.yml`, in declaration order."""
    path = manifest_path(directory)
    if not path.is_file():
        raise FileNotFoundError(f"no {MANIFEST_NAME} in {directory}")

    doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    entries = doc.get("upstreams")
    if not isinstance(entries, list) or not entries:
        raise ValueError(f"{path}: `upstreams` must be a non-empty list")

    return [_build(path, directory, e) for e in entries]


def one(directory: Path) -> Upstream:
    """The single upstream of a one-upstream dependency; an error if the manifest declares several."""
    ups = load(directory)
    if len(ups) != 1:
        raise ValueError(f"{manifest_path(directory)}: expected exactly one upstream, found {len(ups)}")
    return ups[0]


def by_name(directory: Path, name: str) -> Upstream:
    for up in load(directory):
        if up.name == name:
            return up
    raise KeyError(f"{manifest_path(directory)}: no upstream named {name!r}")


def load_all(extern_dir: Path) -> list[Upstream]:
    """Every upstream under `extern/`, dependency directories in sorted order."""
    out: list[Upstream] = []
    for path in sorted(extern_dir.glob(f"*/{MANIFEST_NAME}")):
        out.extend(load(path.parent))
    return out


def _build(path: Path, directory: Path, entry: object) -> Upstream:
    if not isinstance(entry, dict):
        raise ValueError(f"{path}: each `upstreams` entry must be a mapping")

    def need(key: str) -> str:
        value = entry.get(key)
        if not isinstance(value, str) or not value:
            raise ValueError(f"{path}: upstream {entry.get('name', '?')!r} is missing `{key}`")
        return value

    up = Upstream(
        name=need("name"),
        directory=directory,
        source=need("source"),
        track=need("track"),
        install=entry.get("install", "vendored"),
        pin_hash=need("pin_hash"),
        digest_algo=need("digest_algo"),
        license=need("license"),
        homepage=entry.get("homepage", ""),
        repo=entry.get("repo", ""),
        tag=entry.get("tag"),
        version=str(entry.get("version", "")),
        year=str(entry.get("year", "")),
        asset=entry.get("asset", ""),
        tag_pattern=entry.get("tag_pattern", ""),
        license_files=list(entry.get("license_files", [])),
        license_text=entry.get("license_text", ""),
        used_by=entry.get("used_by", ""),
        notes=entry.get("notes", ""),
    )

    if up.source not in SOURCES:
        raise ValueError(f"{path}: {up.name}: `source` must be one of {sorted(SOURCES)}, got {up.source!r}")
    if up.track not in TRACKS:
        raise ValueError(f"{path}: {up.name}: `track` must be one of {sorted(TRACKS)}, got {up.track!r}")
    if up.install not in INSTALLS:
        raise ValueError(f"{path}: {up.name}: `install` must be one of {sorted(INSTALLS)}, got {up.install!r}")
    if up.digest_algo not in DIGEST_ALGOS:
        raise ValueError(f"{path}: {up.name}: `digest_algo` must be one of {sorted(DIGEST_ALGOS)}, got {up.digest_algo!r}")
    if not up.license_files and not up.license_text:
        raise ValueError(f"{path}: {up.name}: needs `license_files` or `license_text`")

    return up
