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
import platform
import sys
from pathlib import Path

import yaml

MANIFEST_NAME = "dependency.yml"

# `source` says how we obtain the upstream; `track` says how "what is current" is defined.
# They are separate because stb, ImPlot and ImGuizmo are ordinary git clones whose newest version is a branch head, not a tag.
SOURCES = {"git", "github-release", "github-files", "url"}
TRACKS = {"tags", "default-branch", "github-releases", "sqlite", "none"}
DIGEST_ALGOS = {"git-commit", "sha256", "sha3-256"}
# The keys `unavailable_on` may name a host by.
# A bare OS key covers every architecture, an arch-qualified one exactly one machine.
# Both spellings are needed because upstreams routinely ship a release for a platform without shipping it for every machine that platform runs on.
HOST_OS_KEYS = ("windows", "linux", "macos")
HOST_ARCH_KEYS = ("x64", "arm64")
HOST_KEYS = HOST_OS_KEYS + tuple(f"{os_key}-{arch}" for os_key in HOST_OS_KEYS for arch in HOST_ARCH_KEYS)
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
    # Host keys this upstream has no release for at all — see `HOST_KEYS` for the spelling.
    # Distinct from a missing per-OS key, which stays an error: that means nobody has looked, and this means somebody did.
    unavailable_on: list[str] = field(default_factory=list)
    license_files: list[str] = field(default_factory=list)

    # For `source: github-files`: the individual files fetched, each with its own digest.
    # A whole repository is the wrong unit when what is wanted is three files out of several hundred megabytes.
    files: list[dict] = field(default_factory=list)
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
    def is_available(self) -> bool:
        """Whether this upstream has a release for the host at all.

        False means the manifest says so deliberately — see `unavailable_on`.
        Such an upstream carries no pin and no asset here, so every field that would name one is empty.
        """
        return not any(key in self.unavailable_on for key in host_keys())

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


def host_os_key() -> str:
    """The suffix a per-OS manifest key carries for this host: `windows`, `linux` or `macos`.

    An upstream shipping one asset per platform declares `asset_<key>` / `pin_hash_<key>` instead of the bare keys, and
    the host's pair is what `_build` resolves into the plain fields — so everything downstream sees one asset and one
    pin without knowing the distinction exists.
    """
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def host_arch_key() -> str:
    """The architecture half of a host key: `x64` or `arm64`.

    Anything that is not a recognised 64-bit ARM machine reads as `x64`, since every target this repo builds is one or the other.
    A third architecture should fail loudly downstream rather than silently name a key nothing declares.
    """
    return "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"


def host_keys() -> list[str]:
    """Every key `unavailable_on` may name this host by, widest first.

    An upstream that ships nothing for the platform lists the bare OS key.
    One that ships for some of its machines lists the arch-qualified key instead, and matching against both is what lets either spelling mean what it says.
    """
    os_key = host_os_key()
    return [os_key, f"{os_key}-{host_arch_key()}"]


def _build(path: Path, directory: Path, entry: object) -> Upstream:
    if not isinstance(entry, dict):
        raise ValueError(f"{path}: each `upstreams` entry must be a mapping")

    def need(key: str) -> str:
        value = entry.get(key)
        if not isinstance(value, str) or not value:
            raise ValueError(f"{path}: upstream {entry.get('name', '?')!r} is missing `{key}`")
        return value

    # A per-OS key wins over the bare one where the manifest declares it, so an upstream with one asset per platform
    # needs no special handling anywhere downstream.
    # A manifest declaring per-OS keys but not this host's is an error rather than a silent fallback: it means the
    # dependency has not been ported here, and a bare key from another platform would fetch the wrong archive.
    suffix = host_os_key()

    # An upstream the manifest says has no release here resolves to empty per-OS values rather than raising.
    # Without this an absent key was indistinguishable from an un-ported one, so DXC — which ships no macOS build — took
    # down every consumer of the whole manifest set on a Mac, `deps list` and `deps licenses` included.
    unavailable = [str(x) for x in entry.get("unavailable_on", [])]
    # An unrecognised key must be refused rather than ignored, since ignoring it silently means "available everywhere" — the opposite of what the manifest says.
    unknown = [key for key in unavailable if key not in HOST_KEYS]
    if unknown:
        raise ValueError(
            f"{path}: upstream {entry.get('name', '?')!r} lists unknown `unavailable_on` key(s) {unknown} — "
            f"must be one of {list(HOST_KEYS)}"
        )
    host_unavailable = any(key in unavailable for key in host_keys())

    def per_os(key: str, *, required: bool) -> str:
        host_key = f"{key}_{suffix}"
        if host_key in entry:
            return need(host_key)
        if any(k.startswith(f"{key}_") for k in entry):
            if host_unavailable:
                return ""
            raise ValueError(f"{path}: upstream {entry.get('name', '?')!r} declares per-OS `{key}` but none for {suffix}")
        return need(key) if required else str(entry.get(key, ""))

    up = Upstream(
        name=need("name"),
        directory=directory,
        source=need("source"),
        track=need("track"),
        install=entry.get("install", "vendored"),
        pin_hash=per_os("pin_hash", required=True),
        digest_algo=need("digest_algo"),
        license=need("license"),
        homepage=entry.get("homepage", ""),
        repo=entry.get("repo", ""),
        tag=entry.get("tag"),
        version=str(entry.get("version", "")),
        year=str(entry.get("year", "")),
        asset=per_os("asset", required=False),
        tag_pattern=entry.get("tag_pattern", ""),
        unavailable_on=unavailable,
        license_files=list(entry.get("license_files", [])),
        files=[dict(f) for f in entry.get("files", [])],
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
    if up.source == "github-files" and not up.files:
        raise ValueError(f"{path}: {up.name}: `source: github-files` needs a `files` list")
    if not up.license_files and not up.license_text:
        raise ValueError(f"{path}: {up.name}: needs `license_files` or `license_text`")

    return up
