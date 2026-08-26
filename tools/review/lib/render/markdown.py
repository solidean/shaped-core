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


def _mark_rule(state, silent: bool) -> bool:
    """`==new==` as a highlighted span, for a block re-stated in full with only its changed points drawn.

    The case this exists for is a rephrase where three words moved: unreadable as a diff, dishonest as a silent
    replacement, and invisible to a maintainer who does not re-read an entry end to end before sending.

    Registered after `backticks`, so a code span holding `==` is left alone.
    The content is taken as plain text: this marks changed words, and nesting emphasis inside it would be a second
    thing to reason about for no case anyone has.
    """
    src, pos = state.src, state.pos
    if not src.startswith("==", pos):
        return False
    end = src.find("==", pos + 2)
    if end < 0 or not src[pos + 2:end].strip():
        return False
    if not silent:
        opened = state.push("mark_open", "mark", 1)
        opened.attrs = {"class": "new"}
        state.push("text", "", 0).content = src[pos + 2:end]
        state.push("mark_close", "mark", -1)
    state.pos = end + 2
    return True


def _renderer() -> MarkdownIt:
    md = MarkdownIt("commonmark", {"linkify": False, "html": True})
    md.enable("table")
    md.enable("strikethrough")
    md.inline.ruler.before("emphasis", "mark", _mark_rule)
    return md


_MD = _renderer()


# `add_render_rule` binds the function onto the renderer, so a rule takes `self` first even though it uses nothing from it.
def _fence(self, tokens, index, options, env):
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
