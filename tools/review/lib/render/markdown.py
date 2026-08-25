"""Markdown for entry prose, plus the one thing plain markdown will not do: make a code reference clickable.

A finding that names `libs/foo/bar.cc:63` should open that line, not make the reader search for it.
So a backticked reference to a file that actually exists in the repo under review becomes a `vscode://` link,
and one that does not is left as code — a dead link would be worse than plain text.
"""

from __future__ import annotations

import re
from pathlib import Path

from markdown_it import MarkdownIt

from .highlight import highlight_code

# A backticked code span holding a repo path, optionally with a line or a line range.
_REF_RE = re.compile(
    r"<code>([\w./+-]+\.[A-Za-z0-9]{1,6})(?::(\d+)(?:-(\d+))?)?</code>"
)

_MAX_PROBE = 4096


def _renderer() -> MarkdownIt:
    md = MarkdownIt("commonmark", {"linkify": False, "html": True})
    md.enable("table")
    md.enable("strikethrough")
    return md


_MD = _renderer()


def _fence(tokens, index, options, env):
    token = tokens[index]
    info = (token.info or "").strip()
    lang, _, rest = info.partition(":")
    body = highlight_code(token.content.rstrip("\n"), path=rest.strip(), lang=lang.strip())
    label = f'<div class="code-label">{rest.strip()}</div>' if rest.strip() else ""
    return f'{label}<pre class="pg"><code>{body}</code></pre>\n'


_MD.add_render_rule("fence", _fence)


def _linkify_refs(html: str, repo: Path) -> str:
    """Turn code spans naming a real file into editor links."""

    def replace(m: re.Match) -> str:
        rel, start, end = m.group(1), m.group(2), m.group(3)
        target = repo / rel
        if not target.is_file():
            return m.group(0)
        anchor = f"{target.as_posix()}:{start}" if start else target.as_posix()
        shown = m.group(0)[len("<code>"):-len("</code>")]
        title = f"open {rel}" + (f" at line {start}" + (f"-{end}" if end else "") if start else "")
        return f'<a class="coderef" href="vscode://file/{anchor}" title="{title}"><code>{shown}</code></a>'

    return _REF_RE.sub(replace, html)


def render(text: str, *, repo: Path | None = None) -> str:
    """Entry prose as HTML, with code references linked where the file is really there."""
    if not text.strip():
        return ""
    html = _MD.render(text)
    if repo is not None and len(html) < _MAX_PROBE * 64:
        html = _linkify_refs(html, repo)
    return html


def render_inline(text: str, *, repo: Path | None = None) -> str:
    """The same, for a single line that should not become a paragraph."""
    html = render(text, repo=repo).strip()
    if html.startswith("<p>") and html.endswith("</p>") and html.count("<p>") == 1:
        return html[3:-4]
    return html
