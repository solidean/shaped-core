"""Markdown for entry prose.

Making a reference clickable used to live here, as a regex over the rendered HTML looking for a `<code>` span
holding nothing but a path.
It worked by an accident — highlighting wraps a fence's body in spans, so the regex never matched inside one —
and that accident does not extend to a term mid-sentence, a sha, or a path inside a code comment.

So it moved: `lib/annotate/` finds and resolves references over the entry source, and the page wraps them.
What is left here is markdown, the fence rule, and the two marks the block grammar adds: `==new==` spans and
`pro:` / `con:` / `verdict:` list items.
"""

from __future__ import annotations

import re
from html import escape
from pathlib import Path

from markdown_it import MarkdownIt

from .highlight import highlight_code


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


# A list item opening `pro:`, `con:` or `verdict:` becomes a marked one: a green tick, a red cross, an arrow.
# The case is a design-critique pricing several alternatives, where the shape of the argument has to be visible before
# a word of it is read — a wall of flowing prose with the pros and cons buried in it is the thing this replaces.
#
# Matched before inline parsing, so the content is still the raw source and the prefix comes off cleanly.
# The marker goes on the enclosing list item rather than into the text, so the words the author wrote are all that is
# left in the item and the mark is the page's.
_VERDICT_RE = re.compile(r"^(pro|con|verdict)[ \t]*:[ \t]+", re.IGNORECASE)


def _verdict_rule(state) -> None:
    tokens = state.tokens
    for i, token in enumerate(tokens):
        if token.type != "inline":
            continue
        match = _VERDICT_RE.match(token.content)
        if match is None:
            continue
        # The enclosing item is the nearest `list_item_open` above, reached through the item's own paragraph and
        # nothing else: a paragraph that merely starts with the word is not a priced bullet.
        opener = None
        for j in range(i - 1, -1, -1):
            if tokens[j].type == "list_item_open":
                opener = tokens[j]
                break
            if tokens[j].type != "paragraph_open":
                break
        if opener is None:
            continue
        opener.attrJoin("class", f"verdict v-{match.group(1).lower()}")
        token.content = token.content[match.end():]


def _renderer() -> MarkdownIt:
    md = MarkdownIt("commonmark", {"linkify": False, "html": True})
    md.enable("table")
    md.enable("strikethrough")
    md.inline.ruler.before("emphasis", "mark", _mark_rule)
    md.core.ruler.before("inline", "verdict", _verdict_rule)
    return md


_MD = _renderer()


# `add_render_rule` binds the function onto the renderer, so a rule takes `self` first even though it uses nothing from it.
def _fence(self, tokens, index, options, env):
    token = tokens[index]
    info = (token.info or "").strip()
    # `raw` opts the whole fence out, the way `raw:` does one code span.
    # A fence holds more of what needs it than a span does — a quoted diff, a shell transcript, a config sample
    # — because it holds more, and every line of it is scanned.
    # `raw` alone, or `raw:` in front of the usual `lang:path`.
    raw = info == "raw" or info.startswith("raw:")
    if raw:
        info = info[len("raw:"):].strip() if info.startswith("raw:") else ""
    lang, _, rest = info.partition(":")
    body = highlight_code(token.content.rstrip("\n"), path=rest.strip(), lang=lang.strip())
    label = f'<div class="code-label">{rest.strip()}</div>' if rest.strip() else ""
    opened = '<code class="raw">' if raw else "<code>"
    return f'{label}<pre class="pg">{opened}{body}</code></pre>\n'


_MD.add_render_rule("fence", _fence)


# `raw:` on a code span opts it out of every annotation provider.
#
# The matchers are deliberately eager — a path is decorated wherever it appears, and unresolved is an error rather
# than a shrug, because that strictness is what catches a half-remembered path.
# The cost is that a code span which merely looks like a reference has no way to say it is not one, and narrowing
# the matcher to fix that would trade a loud false positive for a silent false negative — a typo'd path quietly
# staying plain, which is the failure the strictness exists to prevent.
#
# So the escape is per span and explicit.
# The prefix is dropped from what the reader sees, and the `raw` class is what the page's walk skips.
def _code_inline(self, tokens, index, options, env):
    content = tokens[index].content
    if content.startswith("raw:"):
        return f'<code class="raw">{escape(content[len("raw:"):])}</code>'
    return f"<code>{escape(content)}</code>"


_MD.add_render_rule("code_inline", _code_inline)


# The same escape on a link destination, which is the third place a reference can be written.
#
# The case is a relative link quoted verbatim out of another file, written `[the guide]` followed by a `raw:`-prefixed
# target: it resolves against that file rather than against the repository root.
# The prefix is dropped from the href, so the link the reader clicks is the one the author wrote.
def _link_open(self, tokens, index, options, env):
    token = tokens[index]
    href = token.attrGet("href") or ""
    if href.startswith("raw:"):
        token.attrSet("href", href[len("raw:"):])
    return self.renderToken(tokens, index, options, env)


_MD.add_render_rule("link_open", _link_open)


def render(text: str, *, repo: Path | None = None) -> str:
    """Entry prose as HTML.

    References are decorated afterwards, by the annotation pass.
    """
    _ = repo
    if not text.strip():
        return ""
    return _MD.render(text)


def render_inline(text: str, *, repo: Path | None = None) -> str:
    """The same, for a single line that should not become a paragraph."""
    html = render(text, repo=repo).strip()
    if html.startswith("<p>") and html.endswith("</p>") and html.count("<p>") == 1:
        return html[3:-4]
    return html
