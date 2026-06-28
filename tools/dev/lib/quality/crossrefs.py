"""Cross-reference validator: verify links between docs and code still resolve.

This backs `dev.py check crossrefs`. It scans tracked (and untracked-but-not-
ignored) markdown and `.cc`/`.hh` sources under `libs/`, `docs/`, `.claude/`,
plus the repo-root meta docs (CLAUDE.md, README.md), and verifies that:

  - Markdown links in `.md` files: `[text](relative/path.cc#L42-L51)`. The path
    resolves relative to the `.md` file, except a target that starts with a
    repo-root segment (`docs/`, `libs/`, `.claude/`, ...) resolves
    repo-root-relative — that is how the meta docs link. Line anchors
    (`#L42` / `#L42-L51`) are checked against the target's line count; section
    anchors (`#some-heading`) against the target `.md`'s headings. In-page
    anchors (`#foo` with no path) resolve against the same file.
  - Doc references in `//` and `///` comments inside `.cc`/`.hh` files:
    repo-root paths like `docs/guides/building-and-testing.md`.

These rot easily: rename or move a file and links in *other*, untouched files
silently break. The scan is full-repo so the breakage is caught wherever it
lands. `/* ... */` block comments are not scanned (discouraged here).

`check_crossrefs(root)` returns a CrossRefResult; orchestration/printing lives
in `dev.py`.
"""

from __future__ import annotations

import os
import re
import subprocess
import urllib.parse
from dataclasses import dataclass
from pathlib import Path

# Top-level repo dirs a repo-root-relative link/comment token may start with. A
# token starting with one of these is resolved repo-root-relative; anything else
# is treated as relative to the referencing file (then the repo root) as a
# fallback — which is how bare root files (CLAUDE.md, dev.py) resolve.
REPO_ROOT_SEGMENTS = (
    "docs/",
    "libs/",
    "tools/",
    ".claude/",
)

# Dirs scanned as link *sources* (their markdown / sources are walked). A link
# *target* anywhere in the repo is still validated regardless of this list.
_SCAN_DIRS = ("libs", "docs", ".claude")

# Markdown inline link / image: [text](target) or ![text](target). We capture
# the target and recover its line via offset; nested brackets in the text are
# rare enough in our docs that the non-greedy text match is fine.
MD_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]*)\)")

# A path-like token ending in .md, used to find doc references inside comments.
DOC_TOKEN_RE = re.compile(r"[\w.\-/]+\.md")

# #L42 or #L42-L51 or #L42-51 line anchors.
LINE_ANCHOR_RE = re.compile(r"^L(\d+)(?:-L?(\d+))?$")

# ATX markdown heading, e.g. "## Some Heading".
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")


@dataclass
class CrossRefResult:
    """Outcome of a cross-reference scan: offending references plus the counts
    that went into the verdict. `offenders` are preformatted `file:line: msg`
    lines, ready to print one per line."""

    offenders: list[str]
    md_links: int
    src_refs: int
    md_files: int
    src_files: int

    @property
    def ok(self) -> bool:
        return not self.offenders


def list_files(root: Path) -> list[Path]:
    # Tracked + untracked-but-not-ignored, so a freshly added doc is seen before
    # it is committed. Scoped to the dirs we own cross-references for (libs/,
    # docs/, .claude/) plus the repo-root meta docs (CLAUDE.md, README.md). The
    # "*.md" pathspec also matches markdown deeper in the tree (tools/, ...), so
    # we keep only the root-level ones — those subtrees are not scanned as
    # sources, only as link targets.
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard",
         "--", *_SCAN_DIRS, "*.md"],
        check=True,
        capture_output=True,
        text=True,
    )
    scan_prefixes = tuple(f"{d}/" for d in _SCAN_DIRS)
    files: list[Path] = []
    for line in result.stdout.splitlines():
        rel = line.strip()
        if not rel:
            continue
        # Root-level markdown only from the "*.md" spec; dir specs keep their depth.
        if rel.endswith(".md") and "/" in rel and not rel.startswith(scan_prefixes):
            continue
        files.append(root / rel)
    return files


def slugify_heading(text: str) -> str:
    # Approximate GitHub's anchor slugs: strip inline markdown emphasis/code
    # markers, lowercase, drop punctuation other than word chars / space / hyphen,
    # then spaces -> hyphens. Good enough for our prose headings.
    text = text.replace("`", "")
    text = re.sub(r"[*_]+", "", text)
    text = text.lower()
    text = re.sub(r"[^\w\s-]", "", text)
    text = text.strip().replace(" ", "-")
    return text


def heading_slugs(path: Path) -> set[str]:
    # All heading anchors for a .md file, with GitHub-style -1/-2 disambiguation
    # for duplicate slugs. Headings inside fenced code blocks are ignored.
    slugs: set[str] = set()
    counts: dict[str, int] = {}
    in_fence = False
    for line in read_lines(path):
        stripped = line.strip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = HEADING_RE.match(line)
        if not m:
            continue
        base = slugify_heading(m.group(2))
        n = counts.get(base, 0)
        slug = base if n == 0 else f"{base}-{n}"
        counts[base] = n + 1
        slugs.add(slug)
    return slugs


_lines_cache: dict[Path, list[str]] = {}


def read_lines(path: Path) -> list[str]:
    cached = _lines_cache.get(path)
    if cached is not None:
        return cached
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        lines = []
    _lines_cache[path] = lines
    return lines


def check_anchor(target: Path, fragment: str) -> str | None:
    # Returns an error description if the fragment does not resolve, else None.
    m = LINE_ANCHOR_RE.match(fragment)
    if m:
        line_count = len(read_lines(target))
        start = int(m.group(1))
        end = int(m.group(2)) if m.group(2) else start
        if start < 1 or start > end or end > line_count:
            return f"line anchor out of range (#{fragment}, file has {line_count} lines)"
        return None

    # Section-heading anchor: only meaningful for markdown targets.
    if target.suffix != ".md":
        return f"anchor on non-markdown target (#{fragment})"
    if slugify_heading(fragment) in heading_slugs(target) or fragment in heading_slugs(target):
        return None
    return f"missing heading anchor (#{fragment})"


def is_external(target: str) -> bool:
    return "://" in target or target.startswith(("mailto:", "tel:"))


def check_md_links(md_path: Path, root: Path, offenders: list[str]) -> int:
    checked = 0
    rel_md = md_path.relative_to(root).as_posix()
    in_fence = False
    for lineno, line in enumerate(read_lines(md_path), start=1):
        stripped = line.strip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        # Drop inline code spans so C++ snippets like `[](auto& x)` are not
        # mistaken for markdown links.
        line = re.sub(r"`+[^`]*`+", "", line)
        for m in MD_LINK_RE.finditer(line):
            raw = m.group(1).strip()
            if not raw or is_external(raw):
                continue
            # Drop an optional link title: [x](path "title").
            target = raw.split(" ", 1)[0].split("\t", 1)[0]
            if not target:
                continue
            path_part, _, fragment = target.partition("#")
            path_part = urllib.parse.unquote(path_part)

            checked += 1
            loc = f"{rel_md}:{lineno}"

            if not path_part:
                # In-page anchor: resolve against this very file.
                if fragment:
                    err = check_anchor(md_path, fragment)
                    if err:
                        offenders.append(f"{loc}: {err}: {raw}")
                continue

            # A repo-root-segment target (docs/..., libs/..., .claude/..., ...)
            # may be repo-root-relative — the style CLAUDE.md / README.md / skills
            # use — or relative to the linking file, which is how a per-library
            # cheat sheet references its own docs/ subdir. Try repo-root first,
            # then fall back to file-relative; a non-segment target is always
            # file-relative. A trailing slash means a directory is expected.
            wants_dir = path_part.endswith("/")
            candidates: list[Path] = []
            if path_part.startswith(REPO_ROOT_SEGMENTS):
                candidates.append(Path(os.path.normpath(root / path_part)))
            candidates.append(Path(os.path.normpath(md_path.parent / path_part)))
            resolved = next(
                (c for c in candidates if (c.is_dir() if wants_dir else c.is_file())), None
            )
            if resolved is None:
                detail = " (directory not found)" if wants_dir else ""
                offenders.append(f"{loc}: broken link{detail}: {raw}")
                continue
            if fragment:
                err = check_anchor(resolved, fragment)
                if err:
                    offenders.append(f"{loc}: {err}: {raw}")
    return checked


def resolve_doc_token(token: str, src_path: Path, root: Path) -> Path | None:
    # Returns the resolved existing path, or None if it does not resolve. A
    # docs/... token may be repo-root-relative or, in a per-library source file,
    # relative to that file's own docs/ subdir — so try repo-root for segment
    # tokens, then next to the referencing file, then the repo root.
    bases = [root] if token.startswith(REPO_ROOT_SEGMENTS) else []
    bases += [src_path.parent, root]
    for base in bases:
        candidate = Path(os.path.normpath(base / token))
        if candidate.is_file():
            return candidate
    return None


def check_source_refs(src_path: Path, root: Path, offenders: list[str]) -> int:
    checked = 0
    rel_src = src_path.relative_to(root).as_posix()
    for lineno, line in enumerate(read_lines(src_path), start=1):
        idx = line.find("//")
        if idx == -1:
            continue
        comment = line[idx + 2 :]
        for m in DOC_TOKEN_RE.finditer(comment):
            token = m.group(0)
            checked += 1
            if resolve_doc_token(token, src_path, root) is None:
                offenders.append(f"{rel_src}:{lineno}: broken doc ref: {token}")
    return checked


def check_crossrefs(root: Path) -> CrossRefResult:
    """Scan the repo's docs and sources and validate every cross-reference.

    Markdown links (and their line/heading anchors) and `//`-comment doc tokens
    are resolved against the on-disk tree; a link *target* anywhere in the repo
    counts, even in subtrees not scanned as sources. The lines cache is cleared
    on entry so repeated calls in one process see current file contents.
    """
    _lines_cache.clear()

    md_files: list[Path] = []
    src_files: list[Path] = []
    for path in list_files(root):
        if path.suffix == ".md":
            md_files.append(path)
        elif path.suffix in (".cc", ".hh"):
            src_files.append(path)

    offenders: list[str] = []
    md_refs = 0
    src_refs = 0
    for md in sorted(md_files):
        md_refs += check_md_links(md, root, offenders)
    for src in sorted(src_files):
        src_refs += check_source_refs(src, root, offenders)

    return CrossRefResult(
        offenders=offenders,
        md_links=md_refs,
        src_refs=src_refs,
        md_files=len(md_files),
        src_files=len(src_files),
    )
