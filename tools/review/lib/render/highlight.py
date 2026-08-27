"""Syntax highlighting, and the diff rendering built on top of it.

A diff is not highlighted as a diff.
Each line's marker decides its background, and the payload after the marker is highlighted as the language the file is in,
so a hunk reads like the code it changes rather than like a wall of red and green.

Everything here is a pure function of its input bytes, so the results are memoized by content hash.
That is not state the server has to keep consistent — it is the same answer, computed once.
"""

from __future__ import annotations

import hashlib
from functools import lru_cache

from pygments import highlight as _pygments_highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import get_lexer_by_name, guess_lexer_for_filename
from pygments.style import Style
from pygments.token import (
    Comment,
    Error,
    Generic,
    Keyword,
    Name,
    Number,
    Operator,
    Punctuation,
    String,
    Text,
    Whitespace,
)
from pygments.util import ClassNotFound


class DarkModern(Style):
    """VS Code's Dark Modern palette, mapped onto Pygments' token tree.

    Pygments ships no dark default, so without this the page emits the light `default` style over a dark background.
    The tree is coarser than a TextMate grammar, so a few tokens are a judgement call rather than a translation:
    Pygments has no `keyword.control`, so `Keyword.Namespace` carries the magenta that VS Code gives `import` and `#include`,
    and every other keyword takes the blue.

    `background_color` is empty on purpose.
    The diff table paints its own per-line tint, and a style background would sit on top of it.
    """

    background_color = ""
    line_number_color = "#6e7681"

    styles = {
        Text: "#cccccc",
        Whitespace: "#cccccc",
        Error: "#f44747",

        Comment: "#6a9955",
        Comment.Preproc: "#c586c0",
        Comment.PreprocFile: "#ce9178",

        Keyword: "#569cd6",
        Keyword.Namespace: "#c586c0",
        Keyword.Constant: "#569cd6",
        Keyword.Type: "#569cd6",

        Operator: "#d4d4d4",
        Operator.Word: "#c586c0",
        Punctuation: "#d4d4d4",

        Name: "#9cdcfe",
        Name.Attribute: "#9cdcfe",
        Name.Builtin: "#569cd6",
        Name.Builtin.Pseudo: "#569cd6",
        Name.Class: "#4ec9b0",
        Name.Constant: "#4fc1ff",
        Name.Decorator: "#dcdcaa",
        Name.Entity: "#569cd6",
        Name.Exception: "#4ec9b0",
        Name.Function: "#dcdcaa",
        Name.Function.Magic: "#dcdcaa",
        Name.Label: "#c8c8c8",
        Name.Namespace: "#4ec9b0",
        Name.Property: "#9cdcfe",
        Name.Tag: "#569cd6",
        Name.Variable: "#9cdcfe",

        String: "#ce9178",
        String.Doc: "#6a9955",
        String.Escape: "#d7ba7d",
        String.Interpol: "#569cd6",
        String.Regex: "#d16969",
        String.Symbol: "#ce9178",

        Number: "#b5cea8",

        Generic.Deleted: "#f44747",
        Generic.Inserted: "#6a9955",
        Generic.Emph: "italic",
        Generic.Strong: "bold",
        Generic.Heading: "bold #569cd6",
        Generic.Subheading: "bold #4ec9b0",
    }


_FORMATTER = HtmlFormatter(nowrap=True, classprefix="pg-", style=DarkModern)

# Extensions the repo actually carries, mapped to what Pygments calls them.
_BY_SUFFIX = {
    "hh": "cpp", "hpp": "cpp", "h": "cpp", "cc": "cpp", "cpp": "cpp", "cxx": "cpp", "inl": "cpp",
    "py": "python", "md": "markdown", "json": "json", "toml": "toml", "yml": "yaml", "yaml": "yaml",
    "cmake": "cmake", "txt": "text", "hlsl": "hlsl", "glsl": "glsl", "sh": "bash", "ps1": "powershell",
    "js": "javascript", "css": "css", "html": "html", "sql": "sql", "rs": "rust",
}


def css() -> str:
    """The highlighting palette, emitted once into the page.

    An empty `background_color` still makes Pygments emit `background: ;`, which is an invalid declaration rather than no
    declaration, so it is dropped here — the diff table's own per-line tint is what should show through.
    """
    defs = HtmlFormatter(classprefix="pg-", style=DarkModern).get_style_defs(".pg")
    return defs.replace("background: ; ", "").replace("{ background: ; }", "{ }")


def _lexer_for(path: str, lang: str):
    name = lang or _BY_SUFFIX.get(path.rsplit(".", 1)[-1].lower() if "." in path else "", "")
    if name:
        try:
            return get_lexer_by_name(name, stripnl=False)
        except ClassNotFound:
            pass
    if path:
        try:
            return guess_lexer_for_filename(path, "", stripnl=False)
        except ClassNotFound:
            pass
    return None


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


@lru_cache(maxsize=2048)
def _highlight_cached(text: str, path: str, lang: str) -> str:
    lexer = _lexer_for(path, lang)
    if lexer is None:
        return _escape(text)
    try:
        return _pygments_highlight(text, lexer, _FORMATTER).rstrip("\n")
    except Exception:
        # Highlighting is decoration, so a lexer that chokes must never cost the reader the content.
        return _escape(text)


def highlight_code(text: str, *, path: str = "", lang: str = "") -> str:
    """Highlighted HTML for a block of code, with no wrapping element of its own."""
    return _highlight_cached(text, path, lang)


_MARKER_CLASS = {"+": "add", "-": "del", " ": "ctx", "\\": "note"}


def highlight_diff(text: str, *, path: str = "") -> str:
    """A unified diff as a table of marked lines, each payload highlighted as its own language.

    Every row carries its offset into the diff body, which is what a comment on one line anchors on.
    That offset is stable for exactly as long as the change id is, so a `sync` that supersedes the change
    correctly leaves the comment on a hunk that no longer exists.
    """
    rows = []
    old_no = new_no = 0
    for offset, raw in enumerate(text.splitlines()):
        if raw.startswith("@@"):
            rows.append(f'<tr class="dl-hunk" data-off="{offset}"><td class="dl-no"></td><td class="dl-no"></td>'
                        f'<td class="dl-src">{_escape(raw)}</td></tr>')
            header = raw.split("@@")[1].strip() if raw.count("@@") >= 2 else ""
            try:
                old_part, new_part = header.split(" ")
                old_no = int(old_part.lstrip("-").split(",")[0])
                new_no = int(new_part.lstrip("+").split(",")[0])
            except (ValueError, IndexError):
                old_no = new_no = 0
            continue

        marker = raw[:1] if raw else " "
        payload = raw[1:] if raw else ""
        css_class = _MARKER_CLASS.get(marker, "ctx")

        left = right = ""
        if marker == "+":
            right, new_no = str(new_no), new_no + 1
        elif marker == "-":
            left, old_no = str(old_no), old_no + 1
        elif marker == " ":
            left, right = str(old_no), str(new_no)
            old_no, new_no = old_no + 1, new_no + 1

        body = highlight_code(payload, path=path) if payload.strip() else _escape(payload)
        rows.append(
            f'<tr class="dl-{css_class}" data-off="{offset}"><td class="dl-no">{left}</td><td class="dl-no">{right}</td>'
            f'<td class="dl-src"><span class="dl-mark">{_escape(marker)}</span>{body}</td></tr>'
        )
    return '<table class="difflines pg">' + "".join(rows) + "</table>"


def digest(text: str) -> str:
    return hashlib.blake2b(text.encode("utf-8", "replace"), digest_size=8).hexdigest()
