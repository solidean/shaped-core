#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///

"""External dependency inventory for shaped-core.

Where the deps logic actually lives: `dev.py deps` is thin wiring that shells out to this script.
It is also usable standalone: `uv run tools/deps/deps.py list --offline`.

Two commands over one source of truth, the `extern/<dep>/dependency.yml` manifests:

- `list` — every upstream, what we pin, what upstream has moved on to, and whether a fetched dependency is actually installed.
  We do not bump dependencies often enough to automate it, so this is visibility rather than an updater; there is deliberately no `update`.
- `licenses` — regenerate `docs/licenses/` from those manifests, so the directory is a complete shipping bundle including our own license.

`list` reaches the network by default, since a pinned version with no notion of what is current answers half the question.
`--offline` restricts it to the manifests and the installed pins.
Results are cached under `.tmp/deps/updates.json` for a day, because the answer changes on upstream's release cadence rather than ours.

`licenses --check` needs no network at all, which is what makes it cheap enough for the pre-commit gate.
It never deletes a committed license for a dependency that is not currently fetched — regenerating on a machine without DXC
must not silently gut the bundle, so an unfetched dependency warns and keeps what is committed.

tools/dev machinery is reused — the console colours — by putting the repo root on sys.path, and the manifest
reader is shared with the vendor/fetch scripts as extern/deps_manifest.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "extern"))

import deps_manifest  # noqa: E402
from tools.dev import console  # noqa: E402

EXTERN = ROOT / "extern"
LICENSE_DIR = ROOT / "docs" / "licenses"
POLICY_FILE = ROOT / "tools" / "deps" / "license-policy.yml"
CACHE_FILE = ROOT / ".tmp" / "deps" / "updates.json"
CACHE_TTL_SECONDS = 24 * 60 * 60

CACHE_SCHEMA = "deps/1"
GITHUB_API = "https://api.github.com"

# Exit codes, matching tools/lint/clang-tidy.py: 2 is a setup or usage error, 1 a real finding, 0 clean.
EXIT_OK = 0
EXIT_FINDINGS = 1
EXIT_SETUP = 2


# --- upstream resolution ------------------------------------------------------------------------


@dataclass
class Latest:
    """What upstream currently offers, as far as we could determine it."""

    version: str = ""
    date: str = ""
    pinned_date: str = ""
    behind: int | None = None
    unit: str = ""
    error: str = ""

    @property
    def known(self) -> bool:
        return bool(self.version) or self.behind is not None


def _github_repo(up: "deps_manifest.Upstream") -> tuple[str, str] | None:
    match = re.match(r"https://github\.com/([^/]+)/([^/]+?)/?$", up.repo)
    return (match.group(1), match.group(2)) if match else None


def _get_json(url: str, token: str | None) -> object:
    headers = {"User-Agent": "shaped-core-deps", "Accept": "application/vnd.github+json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=20) as response:  # noqa: S310 (fixed api.github.com / sqlite.org hosts)
        return json.loads(response.read().decode("utf-8"))


def _get_text(url: str) -> str:
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-deps"})
    with urllib.request.urlopen(request, timeout=20) as response:  # noqa: S310
        return response.read().decode("utf-8", errors="replace")


def _day(timestamp: str) -> str:
    """The date half of an ISO timestamp — the resolution anyone actually reads."""
    return (timestamp or "")[:10]


def resolve(up: "deps_manifest.Upstream", token: str | None) -> Latest:
    """Ask upstream what is current, by whatever means that upstream's `track` defines."""
    try:
        if up.track == "none":
            return Latest()
        if up.track == "sqlite":
            return _resolve_sqlite(up)
        if up.track == "github-releases":
            return _resolve_github_release(up, token)
        if up.track == "tags":
            return _resolve_github_tags(up, token)
        if up.track == "default-branch":
            return _resolve_github_compare(up, token)
    except urllib.error.HTTPError as e:
        # A rate limit is the common one, and it deserves to say so rather than read as "no update".
        hint = " (rate limited — set GITHUB_TOKEN)" if e.code in (403, 429) else ""
        return Latest(error=f"HTTP {e.code}{hint}")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return Latest(error=str(e))
    except (ValueError, KeyError) as e:
        return Latest(error=f"unexpected response: {e}")
    return Latest(error=f"no resolver for track {up.track!r}")


def _resolve_github_release(up, token) -> Latest:
    repo = _github_repo(up)
    if repo is None:
        return Latest(error=f"not a github repo: {up.repo!r}")
    data = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/releases/latest", token)
    return Latest(version=data.get("tag_name", ""), date=_day(data.get("published_at", "")))


# The tags endpoint returns no dates and no useful order — its sequence is neither chronological nor
# semantic, which is why BLAKE3 would otherwise report a `guts_0.0.0` tag and mimalloc a 2019 `win-m4`.
# So tags are filtered to the ones that are versions at all, then sorted numerically here.
_DEFAULT_TAG_PATTERN = r"^v?\d+(\.\d+)*$"

# GitHub pages tags at 100; a repo with more than this many is not one whose newest version we would miss.
_TAG_PAGES = 3


def _version_key(tag: str) -> tuple:
    return tuple(int(part) for part in re.findall(r"\d+", tag))


def _resolve_github_tags(up, token) -> Latest:
    """The newest version tag, plus how many tags sit between it and ours.

    The date costs a second call, since the tags endpoint carries none.
    """
    repo = _github_repo(up)
    if repo is None:
        return Latest(error=f"not a github repo: {up.repo!r}")

    pattern = re.compile(up.tag_pattern or _DEFAULT_TAG_PATTERN)
    names: list[str] = []
    for page in range(1, _TAG_PAGES + 1):
        batch = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/tags?per_page=100&page={page}", token)
        if not batch:
            break
        names += [t["name"] for t in batch]
        if len(batch) < 100:
            break

    versions = sorted({n for n in names if pattern.match(n)}, key=_version_key, reverse=True)
    if not versions:
        return Latest(error=f"no tags matching {pattern.pattern}")

    newest = versions[0]
    behind = versions.index(up.tag) if up.tag in versions else None

    date = ""
    if newest != up.tag:
        commit = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/commits/{newest}", token)
        date = _day(commit.get("commit", {}).get("committer", {}).get("date", ""))

    return Latest(version=newest, date=date, behind=behind, unit="releases")


def _resolve_github_compare(up, token) -> Latest:
    """How far the branch head has moved past our pinned commit, with both dates, in one call."""
    repo = _github_repo(up)
    if repo is None:
        return Latest(error=f"not a github repo: {up.repo!r}")
    data = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/compare/{up.pin_hash}...HEAD", token)
    ahead = data.get("ahead_by")
    commits = data.get("commits") or []
    head = commits[-1] if commits else data.get("base_commit", {})
    return Latest(
        version=(head.get("sha", "") or "")[:12],
        date=_day(head.get("commit", {}).get("committer", {}).get("date", "")),
        pinned_date=_day(data.get("base_commit", {}).get("commit", {}).get("committer", {}).get("date", "")),
        behind=ahead,
        unit="commits",
    )


def _resolve_sqlite(up) -> Latest:
    """sqlite.org has no API, but download.html carries a machine-readable PRODUCT block.

    Each line is `PRODUCT,version,relpath,size,sha3` — we want the amalgamation's version.
    """
    page = _get_text("https://sqlite.org/download.html")
    for line in page.splitlines():
        if not line.startswith("PRODUCT,"):
            continue
        parts = line.split(",")
        if len(parts) >= 3 and "sqlite-amalgamation-" in parts[2]:
            packed = re.search(r"sqlite-amalgamation-(\d+)\.zip", parts[2])
            if packed:
                return Latest(version=packed.group(1))
    return Latest(error="no amalgamation in the PRODUCT block")


# --- update cache -------------------------------------------------------------------------------


def load_cache(refresh: bool) -> dict:
    if refresh or not CACHE_FILE.is_file():
        return {}
    try:
        data = json.loads(CACHE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if data.get("schema") != CACHE_SCHEMA:
        return {}
    if time.time() - data.get("generated_at", 0) > CACHE_TTL_SECONDS:
        return {}
    return data.get("upstreams", {})


def save_cache(entries: dict) -> None:
    CACHE_FILE.parent.mkdir(parents=True, exist_ok=True)
    payload = {"schema": CACHE_SCHEMA, "generated_at": time.time(), "upstreams": entries}
    CACHE_FILE.write_text(json.dumps(payload, indent=2), encoding="utf-8")


# --- list -----------------------------------------------------------------------------------------


def install_state(up: "deps_manifest.Upstream") -> str:
    """For a fetched dependency, whether .install/ exists and matches the pin.

    A vendored one is always in the tree, and a bundled one rides along inside its parent, so neither has a state to report.
    """
    if not up.is_fetched:
        return up.install
    installed = up.installed_pin()
    if installed is None:
        return "not fetched"
    return "current" if installed == up.pin_hash else "stale"


def status_of(up: "deps_manifest.Upstream", latest: Latest) -> str:
    if up.track == "none":
        return "pinned by parent"
    if latest.error:
        return "unknown"
    if not latest.known:
        return "unknown"
    if latest.behind is not None:
        if latest.behind == 0:
            return "up to date"
        unit = latest.unit if latest.behind != 1 else latest.unit.rstrip("s")
        return f"{latest.behind} {unit} behind"
    ours = up.tag or up.version
    return "up to date" if latest.version == ours else f"{latest.version} available"


def pinned_label(up: "deps_manifest.Upstream") -> str:
    version = up.tag or up.version
    short = up.pin_hash[:12]
    return f"{version} · {short}" if version else short


def run_list(*, offline: bool, refresh: bool, as_json: bool) -> int:
    upstreams = deps_manifest.load_all(EXTERN)

    cache = {} if offline else load_cache(refresh)
    resolved: dict[str, Latest] = {}
    token = os.environ.get("GITHUB_TOKEN") or None

    if not offline:
        fresh: dict[str, dict] = {}
        for up in upstreams:
            key = f"{up.directory.name}/{up.name}"
            hit = cache.get(key)
            if hit is not None and hit.get("pin_hash") == up.pin_hash:
                resolved[key] = Latest(**{k: v for k, v in hit.items() if k != "pin_hash"})
                fresh[key] = hit
                continue
            print(f"  resolving {up.name} ...", file=sys.stderr)
            latest = resolve(up, token)
            resolved[key] = latest
            fresh[key] = {**latest.__dict__, "pin_hash": up.pin_hash}
        save_cache(fresh)

    rows = []
    for up in upstreams:
        key = f"{up.directory.name}/{up.name}"
        latest = resolved.get(key, Latest())
        rows.append(
            {
                "name": up.name,
                "directory": up.directory.name,
                "pinned": up.tag or up.version,
                "pin_hash": up.pin_hash,
                "digest_algo": up.digest_algo,
                "source": up.source,
                "track": up.track,
                "license": up.license,
                "used_by": up.used_by,
                "install": install_state(up),
                "latest": latest.version,
                "latest_date": latest.date,
                "behind": latest.behind,
                "status": "not checked" if offline else status_of(up, latest),
                "error": latest.error,
            }
        )

    if as_json:
        print(json.dumps({"schema": CACHE_SCHEMA, "upstreams": rows}, indent=2))
        return EXIT_OK

    _print_table(rows, offline=offline)
    return EXIT_OK


def _print_table(rows: list[dict], *, offline: bool) -> None:
    name_w = max(len("dependency"), max(len(r["name"]) for r in rows))
    pin_w = max(len("pinned"), max(len(f"{r['pinned']} · {r['pin_hash'][:12]}".strip(" ·")) for r in rows))
    latest_w = max(len("latest"), max(len(_latest_cell(r)) for r in rows))

    header = f"  {'dependency':<{name_w}}  {'pinned':<{pin_w}}  {'latest':<{latest_w}}  status"
    print(header)
    print("  " + "-" * (len(header) - 2))

    for r in rows:
        pinned = f"{r['pinned']} · {r['pin_hash'][:12]}".strip(" ·")
        status = r["status"]
        if status == "up to date":
            status = console.green(status)
        elif status.endswith("behind") or status.endswith("available"):
            status = console.yellow(status)
        elif status in ("unknown",):
            status = console.red(status)
        else:
            status = console.dim(status)
        print(f"  {r['name']:<{name_w}}  {pinned:<{pin_w}}  {_latest_cell(r):<{latest_w}}  {status}")
        if r["error"]:
            print(f"  {'':<{name_w}}  {console.red(r['error'])}")

    fetched = [r for r in rows if r["install"] in ("current", "stale", "not fetched")]
    if fetched:
        print("\n  fetched on demand:")
        for r in fetched:
            state = r["install"]
            mark = console.green(state) if state == "current" else console.yellow(state)
            print(f"    {r['name']:<{name_w}}  {mark}")

    if offline:
        print(f"\n  {console.dim('offline — upstream not checked')}")


def _latest_cell(row: dict) -> str:
    if not row["latest"]:
        return "-"
    return f"{row['latest']} · {row['latest_date']}" if row["latest_date"] else row["latest"]


# --- licenses -------------------------------------------------------------------------------------


@dataclass
class Bundle:
    """One license file destined for docs/licenses/, and where its text came from."""

    filename: str
    text: str
    title: str
    missing: bool = False


def load_policy() -> tuple[set[str], dict[str, str]]:
    """The allowlist, as a set of accepted identifiers plus the reason attached to each denied one."""
    import yaml

    if not POLICY_FILE.is_file():
        sys.exit(f"missing license policy: {POLICY_FILE}")
    doc = yaml.safe_load(POLICY_FILE.read_text(encoding="utf-8")) or {}
    allowed = {e["identifier"] for e in doc.get("allowed", [])}
    allowed |= {e["identifier"] for e in doc.get("exceptions", [])}
    denied = {e["identifier"]: e.get("why", "") for e in doc.get("denied", [])}
    return allowed, denied


def split_expression(expression: str) -> list[str]:
    """The identifiers in an SPDX expression.

    `A OR B` is allowed only when every branch is, since we may end up relying on any of them.
    """
    return [part.strip() for part in re.split(r"\bOR\b", expression) if part.strip()]


def check_policy(upstreams: list, allowed: set[str], denied: dict[str, str]) -> list[str]:
    problems = []
    for up in upstreams:
        for identifier in split_expression(up.license):
            if identifier in allowed:
                continue
            why = denied.get(identifier)
            if why:
                problems.append(f"{up.name}: {identifier} is denied by policy — {why}")
            else:
                problems.append(
                    f"{up.name}: {identifier} is not on the license allowlist. "
                    f"Read the terms, then add it to {POLICY_FILE.relative_to(ROOT).as_posix()} with a reason."
                )
    return problems


def collect(upstreams: list) -> tuple[list[Bundle], list[str]]:
    """The license text for every upstream, plus a warning per one we could not read."""
    bundles: list[Bundle] = []
    warnings: list[str] = []

    for up in upstreams:
        if up.license_text:
            bundles.append(Bundle(f"{up.slug}.txt", up.license_text, up.name))
            continue

        paths = up.license_paths()
        for path in paths:
            # A dependency under several licenses gets one file each, suffixed from the source file's own name.
            suffix = "" if len(paths) == 1 else "-" + path.name.lower().removeprefix("license").strip("._-")
            filename = f"{up.slug}{suffix}.txt"
            if path.is_file():
                bundles.append(Bundle(filename, path.read_text(encoding="utf-8", errors="replace"), up.name))
            else:
                bundles.append(Bundle(filename, "", up.name, missing=True))
                warnings.append(
                    f"{up.name}: {path.relative_to(ROOT).as_posix()} is not present "
                    f"({'not fetched' if up.is_fetched else 'missing'}) — keeping the committed copy"
                )

    return bundles, warnings


def render_index(upstreams: list) -> str:
    lines = [
        "# Third-party licenses",
        "",
        "Every external dependency shaped-core builds against, and the license each is under.",
        "Our own license is in [shaped-core.txt](shaped-core.txt), so this directory is a complete bundle rather than everything-except-ours.",
        "",
        "**Generated — do not edit by hand.**",
        "`uv run dev.py deps licenses` regenerates it from the `extern/<dep>/dependency.yml` manifests, and `--check` verifies it has not drifted.",
        "",
        "| Dependency | Version | License | Used by | Text |",
        "|---|---|---|---|---|",
    ]

    for up in upstreams:
        version = up.tag or up.version or up.pin_hash[:12]
        links = ", ".join(f"[{b.filename}]({b.filename})" for b in collect([up])[0])
        lines.append(f"| [{up.name}]({up.homepage}) | {version} | `{up.license}` | {up.used_by} | {links} |")

    lines += [
        "| shaped-core | — | `MIT` | this repository | [shaped-core.txt](shaped-core.txt) |",
        "",
        "The allowlist these are gated against is [tools/deps/license-policy.yml](../../tools/deps/license-policy.yml),",
        "which `dev.py check` enforces so a version bump cannot quietly introduce a copyleft dependency.",
        "",
    ]
    return "\n".join(lines)


def run_licenses(*, check: bool) -> int:
    upstreams = deps_manifest.load_all(EXTERN)

    allowed, denied = load_policy()
    problems = check_policy(upstreams, allowed, denied)

    bundles, warnings = collect(upstreams)
    bundles.append(Bundle("shaped-core.txt", (ROOT / "LICENSE").read_text(encoding="utf-8"), "shaped-core"))

    wanted = {b.filename: b for b in bundles}
    index = render_index(upstreams)

    for warning in warnings:
        print(f"  {console.yellow('warning')}: {warning}", file=sys.stderr)

    drifted: list[str] = []

    def compare(path: Path, text: str) -> None:
        current = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else None
        if current != text:
            drifted.append(path.relative_to(ROOT).as_posix())

    if check:
        compare(LICENSE_DIR / "_index.md", index)
        for name, bundle in wanted.items():
            if bundle.missing:
                # Absent upstream text cannot be verified, and must never be treated as drift.
                if not (LICENSE_DIR / name).is_file():
                    drifted.append(f"docs/licenses/{name} (missing, and its dependency is not fetched)")
                continue
            compare(LICENSE_DIR / name, bundle.text)

        for existing in sorted(LICENSE_DIR.glob("*.txt")) if LICENSE_DIR.is_dir() else []:
            if existing.name not in wanted:
                drifted.append(f"docs/licenses/{existing.name} (no manifest declares it)")

        for problem in problems:
            print(f"  {console.red('policy')}: {problem}", file=sys.stderr)
        for path in drifted:
            print(f"  {console.red('drift')}: {path}", file=sys.stderr)

        if problems or drifted:
            print(
                f"\ndeps licenses: {len(problems)} policy problem(s), {len(drifted)} drifted file(s). "
                "Run `uv run dev.py deps licenses` to regenerate.",
                file=sys.stderr,
            )
            return EXIT_FINDINGS

        print(f"deps licenses: OK ({len(wanted)} licenses, {len(upstreams)} upstreams)")
        return EXIT_OK

    for problem in problems:
        print(f"  {console.red('policy')}: {problem}", file=sys.stderr)

    LICENSE_DIR.mkdir(parents=True, exist_ok=True)
    written = 0
    for name, bundle in wanted.items():
        if bundle.missing:
            continue  # never overwrite a committed license with nothing
        path = LICENSE_DIR / name
        if not path.is_file() or path.read_text(encoding="utf-8", errors="replace") != bundle.text:
            path.write_text(bundle.text, encoding="utf-8")
            written += 1

    skipped = sum(1 for b in wanted.values() if b.missing)

    index_path = LICENSE_DIR / "_index.md"
    if not index_path.is_file() or index_path.read_text(encoding="utf-8") != index:
        index_path.write_text(index, encoding="utf-8")
        written += 1

    total = len(wanted) + 1  # the license files, plus the generated index
    print(f"deps licenses: {total} files in docs/licenses/ ({written} written, {total - written - skipped} unchanged, {skipped} kept)")
    return EXIT_FINDINGS if problems else EXIT_OK


# --- cli ------------------------------------------------------------------------------------------


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")

    ap = argparse.ArgumentParser(description="External dependency inventory and license collection.")
    ap.add_argument("--color", choices=("auto", "always", "never"), default="auto")
    sub = ap.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="every dependency, its pin, and what upstream offers now")
    p_list.add_argument("--offline", action="store_true", help="manifests and installed pins only, no network")
    p_list.add_argument("--refresh", action="store_true", help="ignore the cached upstream lookups")
    p_list.add_argument("--json", action="store_true", dest="as_json", help="machine-readable output")

    p_lic = sub.add_parser("licenses", help="regenerate docs/licenses/ from the manifests")
    p_lic.add_argument("--check", action="store_true", help="verify without writing; no network needed")

    args = ap.parse_args()
    console.configure("colored" if args.color == "always" else "plain" if args.color == "never" else "auto")

    try:
        if args.command == "list":
            return run_list(offline=args.offline, refresh=args.refresh, as_json=args.as_json)
        if args.command == "licenses":
            return run_licenses(check=args.check)
    except (FileNotFoundError, ValueError, KeyError) as e:
        print(f"deps: {e}", file=sys.stderr)
        return EXIT_SETUP

    ap.error(f"unknown command {args.command!r}")
    return EXIT_SETUP


if __name__ == "__main__":
    raise SystemExit(main())
