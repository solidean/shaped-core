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
It also reads the release notes of every version we are behind, and prints a loud banner when any of them looks like a security fix.
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
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
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
    # Security-looking notes among the releases we are behind, as plain dicts so the cache round-trips them.
    advisories: list[dict] = field(default_factory=list)

    @property
    def known(self) -> bool:
        return bool(self.version) or self.behind is not None


# What in a release note or commit message makes a version bump worth doing now rather than eventually.
# Tuned for the phrasing upstreams actually use, and deliberately narrower than "overflow" alone, which fires on
# every arithmetic fix and would train the reader to skip the banner.
# The matched line is always shown, so a false positive costs one glance rather than a wrong decision.
_SECURITY_PATTERNS = [
    ("CVE", r"CVE-\d{4}-\d{4,7}"),
    ("security", r"\bsecurit(?:y|ies)\b"),
    ("vulnerability", r"\bvulnerab(?:le|ility|ilities)\b"),
    ("exploit", r"\bexploit(?:able|ed)?\b"),
    ("buffer overflow", r"\b(?:buffer|heap|stack|integer)[ -]overflow\b"),
    ("out-of-bounds", r"\bout[ -]of[ -]bounds\b|\bOOB\b"),
    ("use-after-free", r"\buse[ -]after[ -]free\b"),
    ("double free", r"\bdouble[ -]free\b"),
    ("memory corruption", r"\bmemory corruption\b"),
    ("denial of service", r"\bdenial[ -]of[ -]service\b"),
]

_SECURITY_REGEXES = [(label, re.compile(pattern, re.IGNORECASE)) for label, pattern in _SECURITY_PATTERNS]

# Enough of the matching line to judge it without opening the link; a release note bullet is rarely longer.
_SNIPPET_CHARS = 160


def scan_security(text: str) -> tuple[str, str] | None:
    """The first security-looking phrase in a release note or commit message, with the line it sits on.

    Returns the matched label and a trimmed snippet, or None.
    """
    for line in (text or "").splitlines():
        stripped = line.strip().lstrip("*-# ").strip()
        if not stripped:
            continue
        for label, regex in _SECURITY_REGEXES:
            if regex.search(stripped):
                snippet = stripped if len(stripped) <= _SNIPPET_CHARS else stripped[: _SNIPPET_CHARS - 1] + "…"
                return label, snippet
    return None


def github_token() -> str | None:
    """A token for the GitHub API, if one can be had without asking.

    Unauthenticated is 60 requests an hour, which a couple of `--refresh` runs exhaust — and the failure is
    silent-looking, every row reading "unknown". `gh` is already a dependency of the PR and CI workflows here,
    so borrowing its token costs the caller nothing and is what keeps that from being the normal experience.
    """
    if os.environ.get("GITHUB_TOKEN"):
        return os.environ["GITHUB_TOKEN"]
    try:
        done = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout.strip() or None if done.returncode == 0 else None


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


def _releases(repo: tuple[str, str], token) -> list[dict]:
    """Published releases, newest first.

    Drafts and prereleases are dropped, so this matches what /releases/latest would have picked while also
    handing back the notes for every release we are behind.
    """
    data = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/releases?per_page=100", token) or []
    return [r for r in data if not r.get("draft") and not r.get("prerelease")]


def _advisories_from_releases(releases: list[dict], stop_at: str | None) -> list[dict]:
    """Scan the releases newer than `stop_at` — the tag we are on — for security-looking notes."""
    found = []
    for release in releases:
        tag = release.get("tag_name", "")
        if stop_at is not None and tag == stop_at:
            break
        hit = scan_security(f"{release.get('name', '')}\n{release.get('body', '')}")
        if hit:
            found.append(
                {"version": tag, "keyword": hit[0], "snippet": hit[1], "url": release.get("html_url", "")}
            )
    return found


def _resolve_github_release(up, token) -> Latest:
    repo = _github_repo(up)
    if repo is None:
        return Latest(error=f"not a github repo: {up.repo!r}")

    releases = _releases(repo, token)
    if not releases:
        return Latest(error="no published releases")

    newest = releases[0]
    tags = [r.get("tag_name", "") for r in releases]
    behind = tags.index(up.tag) if up.tag in tags else None

    return Latest(
        version=newest.get("tag_name", ""),
        date=_day(newest.get("published_at", "")),
        behind=behind,
        unit="releases",
        advisories=_advisories_from_releases(releases, up.tag),
    )


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
        # Our own tag being in view means everything newer is too, since what follows sorts below it.
        # Usually one page, against the three a full walk of a long-lived repo's tags would cost.
        if up.tag and up.tag in names:
            break

    versions = sorted({n for n in names if pattern.match(n)}, key=_version_key, reverse=True)
    if not versions:
        return Latest(error=f"no tags matching {pattern.pattern}")

    newest = versions[0]
    behind = versions.index(up.tag) if up.tag in versions else None

    date = ""
    advisories: list[dict] = []
    if newest != up.tag:
        # A tag-tracked upstream usually publishes releases against those tags too, so one call covers both the
        # date and the notes for every version we are behind.
        #
        # Matched on the numeric version rather than the tag text, because the two need not agree.
        # We pin Dear ImGui's `v1.92.8-docking` while its releases are tagged `v1.92.8`, and a suffixed respin
        # like `v1.92.9b` is still a release we are behind; both map onto the same key.
        releases = _releases(repo, token)
        newer = {_version_key(v) for v in versions[:behind]} if behind else set()
        advisories = _advisories_from_releases(
            [r for r in releases if _version_key(r.get("tag_name", "")) in newer], None
        )

        # Matched on the exact tag, not the version key, so the date belongs to the tag the column names.
        # Dear ImGui is why: its `v1.92.9b` respin shares a version key with `v1.92.9`, and dating the
        # docking tag from the respin would put a date next to a version that was not released on it.
        for release in releases:
            if release.get("tag_name", "") == newest:
                date = _day(release.get("published_at", ""))
                break
        else:
            # Tagged without a release of its own, so the date costs its own call.
            commit = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/commits/{newest}", token)
            date = _day(commit.get("commit", {}).get("committer", {}).get("date", ""))

    return Latest(version=newest, date=date, behind=behind, unit="releases", advisories=advisories)


def _resolve_github_compare(up, token) -> Latest:
    """How far the branch head has moved past our pinned commit, with both dates, in one call."""
    repo = _github_repo(up)
    if repo is None:
        return Latest(error=f"not a github repo: {up.repo!r}")
    data = _get_json(f"{GITHUB_API}/repos/{repo[0]}/{repo[1]}/compare/{up.pin_hash}...HEAD", token)
    ahead = data.get("ahead_by")
    commits = data.get("commits") or []
    head = commits[-1] if commits else data.get("base_commit", {})

    # These upstreams publish no release notes, but the compare response already carries every commit message
    # between our pin and the head — the same information, at no extra request.
    advisories = []
    for entry in commits:
        hit = scan_security(entry.get("commit", {}).get("message", ""))
        if hit:
            advisories.append(
                {
                    "version": (entry.get("sha", "") or "")[:12],
                    "keyword": hit[0],
                    "snippet": hit[1],
                    "url": entry.get("html_url", ""),
                }
            )

    return Latest(
        version=(head.get("sha", "") or "")[:12],
        date=_day(head.get("commit", {}).get("committer", {}).get("date", "")),
        pinned_date=_day(data.get("base_commit", {}).get("commit", {}).get("committer", {}).get("date", "")),
        behind=ahead,
        unit="commits",
        advisories=advisories,
    )


def _dotted_version(packed: str) -> tuple[int, ...]:
    """SQLite's download version is packed as X*1000000 + Y*10000 + Z*100 — 3530300 is 3.53.3."""
    n = int(packed)
    return (n // 1000000, (n // 10000) % 100, (n // 100) % 100)


def _strip_html(fragment: str) -> str:
    """The text of a changes.html section, one release note per line.

    The line split is what lets the security scan report the bullet it matched, rather than the whole section.
    """
    text = re.sub(r"<li>|<p>|<br\s*/?>", "\n", fragment, flags=re.IGNORECASE)
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"&[a-z]+;", " ", text)
    lines = (re.sub(r"[ \t]+", " ", line).strip() for line in text.splitlines())
    return "\n".join(line for line in lines if line)


def _resolve_sqlite(up) -> Latest:
    """sqlite.org has no API, but changes.html is one `<h3>DATE (VERSION)</h3>` per release, newest first.

    That is version, date and release notes in a single request, which download.html's PRODUCT block cannot give.
    """
    page = _get_text("https://sqlite.org/changes.html")
    sections = list(re.finditer(r"<h3>(\d{4}-\d{2}-\d{2}) \(([\d.]+)\)</h3>", page))
    if not sections:
        return Latest(error="no release sections in changes.html")

    ours = _dotted_version(up.version)
    newest = sections[0]

    advisories = []
    behind = 0
    for index, section in enumerate(sections):
        version = tuple(int(p) for p in section.group(2).split("."))
        if version <= ours:
            break
        behind += 1
        end = sections[index + 1].start() if index + 1 < len(sections) else len(page)
        hit = scan_security(_strip_html(page[section.end():end]))
        if hit:
            advisories.append(
                {
                    "version": section.group(2),
                    "keyword": hit[0],
                    "snippet": hit[1],
                    "url": f"https://sqlite.org/changes.html#version_{section.group(2).replace('.', '_')}",
                }
            )

    return Latest(
        version=newest.group(2),
        date=newest.group(1),
        behind=behind,
        unit="releases",
        advisories=advisories,
    )


# --- update cache -------------------------------------------------------------------------------


def load_cache(refresh: bool) -> dict:
    """The still-fresh cached lookups, keyed by dependency and upstream name.

    Each entry carries its own `fetched_at`, so a run that re-fetches one upstream does not extend the
    lifetime of the others it merely copied forward.
    """
    if refresh or not CACHE_FILE.is_file():
        return {}
    try:
        data = json.loads(CACHE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if data.get("schema") != CACHE_SCHEMA:
        return {}

    now = time.time()
    entries = data.get("upstreams", {})
    return {k: v for k, v in entries.items() if now - v.get("fetched_at", 0) <= CACHE_TTL_SECONDS}


def save_cache(entries: dict) -> None:
    CACHE_FILE.parent.mkdir(parents=True, exist_ok=True)
    payload = {"schema": CACHE_SCHEMA, "generated_at": time.time(), "upstreams": entries}
    CACHE_FILE.write_text(json.dumps(payload, indent=2), encoding="utf-8")


# Cache bookkeeping, not part of what upstream told us.
_CACHE_ONLY_KEYS = ("pin_hash", "fetched_at")


# --- list -----------------------------------------------------------------------------------------


def _display_version(up: "deps_manifest.Upstream") -> str:
    """SQLite's `version` is the download site's packed form, which nobody reads as a version."""
    if up.track == "sqlite" and up.version.isdigit():
        return ".".join(str(part) for part in _dotted_version(up.version))
    return up.version


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
    ours = up.tag or _display_version(up)
    return "up to date" if latest.version == ours else f"{latest.version} available"


def pinned_label(up: "deps_manifest.Upstream") -> str:
    version = up.tag or _display_version(up)
    short = up.pin_hash[:12]
    return f"{version} · {short}" if version else short


def run_list(*, offline: bool, refresh: bool, as_json: bool) -> int:
    upstreams = deps_manifest.load_all(EXTERN)

    cache = {} if offline else load_cache(refresh)
    resolved: dict[str, Latest] = {}
    token = github_token()

    if not offline:
        fresh: dict[str, dict] = {}
        for up in upstreams:
            key = f"{up.directory.name}/{up.name}"
            hit = cache.get(key)
            if hit is not None and hit.get("pin_hash") == up.pin_hash:
                resolved[key] = Latest(**{k: v for k, v in hit.items() if k not in _CACHE_ONLY_KEYS})
                fresh[key] = hit
                continue
            print(f"  resolving {up.name} ...", file=sys.stderr)
            latest = resolve(up, token)
            resolved[key] = latest
            # A failed lookup is never cached: one rate-limited or offline run would otherwise answer
            # "unknown" for the next 24 hours, which reads exactly like a dependency nobody can resolve.
            if not latest.error:
                fresh[key] = {**latest.__dict__, "pin_hash": up.pin_hash, "fetched_at": time.time()}
        save_cache(fresh)

    rows = []
    for up in upstreams:
        key = f"{up.directory.name}/{up.name}"
        latest = resolved.get(key, Latest())
        rows.append(
            {
                "name": up.name,
                "directory": up.directory.name,
                "pinned": up.tag or _display_version(up),
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
                "advisories": latest.advisories,
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

    _print_security_banner(rows)

    fetched = [r for r in rows if r["install"] in ("current", "stale", "not fetched")]
    if fetched:
        print("\n  fetched on demand:")
        for r in fetched:
            state = r["install"]
            mark = console.green(state) if state == "current" else console.yellow(state)
            print(f"    {r['name']:<{name_w}}  {mark}")

    if offline:
        print(f"\n  {console.dim('offline — upstream not checked')}")


def _print_security_banner(rows: list[dict]) -> None:
    """Call out the releases we are behind whose notes read as security fixes.

    Deliberately loud and deliberately below the table: a bump we are sitting on for convenience is a different
    decision once one of the versions in between says "buffer overflow", and that has to be impossible to skim past.
    Every entry shows the line that matched, so a false positive costs one glance rather than a wrong call.
    """
    flagged = [(r, a) for r in rows for a in r["advisories"]]
    if not flagged:
        return

    deps = {r["name"] for r, _ in flagged}
    title = f" SECURITY: {len(flagged)} release note(s) across {len(deps)} dependenc{'y' if len(deps) == 1 else 'ies'} "
    rule = "=" * max(len(title), 78)

    print()
    print(console.red(console.bold(rule)))
    print(console.red(console.bold(title.center(len(rule), "="))))
    print(console.red(console.bold(rule)))
    print()

    for name in sorted(deps):
        entries = [a for r, a in flagged if r["name"] == name]
        row = next(r for r, _ in flagged if r["name"] == name)
        print(f"  {console.bold(name)}  {console.dim(f'pinned {row['pinned'] or row['pin_hash'][:12]}')}")
        for a in entries:
            print(f"    {console.yellow(a['version'])}  [{console.red(a['keyword'])}] {a['snippet']}")
            if a["url"]:
                print(f"      {console.dim(a['url'])}")
        print()

    print(f"  {console.dim('Read the notes before deciding to stay put. See docs/guides/dependencies.md for the bump workflow.')}")


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
    `A AND B` for the same reason from the other direction: both bind at once, so both must be acceptable.
    An operand may still carry a `WITH` exception, which stays attached — `Apache-2.0 WITH LLVM-exception` is one identifier.
    """
    return [part.strip() for part in re.split(r"\bOR\b|\bAND\b", expression) if part.strip()]


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
            # A plainly-named LICENSE leaves nothing to suffix with, and is the primary text anyway, so it keeps the bare slug —
            # libspng's BSD-2-Clause `LICENSE` next to the libpng notice its SIMD code carries is the case.
            stem = path.name.lower().removeprefix("license").strip("._-")
            suffix = "-" + stem if len(paths) > 1 and stem else ""
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
        version = up.tag or _display_version(up) or up.pin_hash[:12]
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
