"""A Pygments lexer for SGL, the Shaped Graphics Language.

SGL cannot be highlighted by a regex state machine, because what a line *is* depends on the line tree above it.
A line that is only a comment owns every deeper-indented line below it, and a line ending in an open `"` turns its children into string content.
So this lexer walks lines with an indentation stack, the way the real tokenizer does, and only tokenizes a line as code once the tree says it is code.

The rules mirror `libs/graphics/shaped-graphics-language/docs/spec/`, which is the authority.
Highlighting is decoration: nothing here reports an error, it only draws one.
"""

from __future__ import annotations

import re

from pygments.lexer import Lexer
from pygments.token import Comment, Error, Keyword, Name, Number, Operator, Punctuation, String, Whitespace

_DECLARATION_KEYWORDS = frozenset({
    "fun", "let", "mut", "out", "struct", "enum", "binding", "sampler", "const", "use", "module", "type", "notation", "assert", "print",
})
# `mut buffer[float]`, `out image2d[rgba8unorm]`: an access word at the top of a type position leaves the position a type.
_TYPE_QUALIFIERS = frozenset({"mut", "out"})
_CONTROL_KEYWORDS = frozenset({"if", "else", "for", "while", "loop", "return", "yield", "continue", "break", "case"})
_WORD_OPERATORS = frozenset({"and", "or", "not", "in", "as"})
_CONSTANTS = frozenset({"true", "false"})
# A symbol directly after one of these names a function or a type.
_NAMES_FUNCTION = frozenset({"fun"})
_NAMES_TYPE = frozenset({"struct", "enum", "binding", "sampler", "type", "module"})

# Everything past ASCII counts as a symbol character for now; the spec will narrow it to identifier ranges.
_NON_ASCII = "\x80-\U0010ffff"
_SYMBOL_START = "0-9A-Za-z_@#\\\\" + _NON_ASCII
# `-` is never a symbol character: `a-b` is a subtraction, so names stay spellable in host languages.
_SYMBOL = re.compile(f"[{_SYMBOL_START}][{_SYMBOL_START}']*")
_OPERATOR = re.compile(r"\.\.[!+\-*/%=<>?&^|~]*|[!+\-*/%=<>?&^|~]+")
_SPACE = re.compile(r"[ \t]+")
_ESCAPE = re.compile(r"\\.")
# What may follow a digit-led symbol to keep it one number: a fraction (never the `..` of a range), and a
# signed exponent once the part before it ends in `e` or `p`. The real tokenizer leaves these as separate fused
# tokens for the form phase to assemble; a highlighter has no later phase, so it assembles them here.
_FRACTION = re.compile(r"[.](?![.])[0-9][0-9A-Za-z_']*")
_EXPONENT = re.compile(r"(?<=[eEpP])[+-][0-9][0-9A-Za-z_']*")
# `$name`, `$name.member`, `$$`, and `$(…)` without nested parentheses, which is as far as a highlighter needs to see.
_INTERPOLATION = re.compile(r"\$\$|\$[A-Za-z_][0-9A-Za-z_]*(?:\.[A-Za-z_][0-9A-Za-z_]*)*|\$\([^()]*(?:\([^()]*\)[^()]*)*\)")
_QUOTE_TOKEN = {'"': String.Double, "'": String.Single, "`": String.Backtick}


class _Line:
    """One entry of the indentation stack: where the line sits, and what it makes of the lines below it."""

    __slots__ = ("indent", "child_mode", "open_quote")

    def __init__(self, indent: int, child_mode: str, open_quote: str):
        self.indent = indent
        self.child_mode = child_mode
        self.open_quote = open_quote


class SglLexer(Lexer):
    name = "SGL"
    aliases = ["sgl"]
    filenames = ["*.sgl"]

    def get_tokens_unprocessed(self, text):
        stack: list[_Line] = []
        pos = 0
        for raw in text.splitlines(keepends=True):
            start = pos
            pos += len(raw)
            body = raw.rstrip("\r\n")
            if not body.strip():
                yield start, Whitespace, raw
                continue

            indent = len(body) - len(body.lstrip(" \t"))
            previous_sibling = None
            while stack and stack[-1].indent >= indent:
                previous_sibling = stack.pop()
            mode = stack[-1].child_mode if stack else "code"

            if indent:
                yield start, Whitespace, body[:indent]
            content = body[indent:]
            at = start + indent

            if mode == "comment":
                yield at, Comment.Single, content
                stack.append(_Line(indent, "comment", ""))
            elif mode in ("string", "raw-string"):
                if mode == "string":
                    for offset, token, value in _interpolated(content, 0, len(content), String, False):
                        yield at + offset, token, value
                else:
                    yield at, String, content
                stack.append(_Line(indent, mode, ""))
            else:
                closes = previous_sibling.open_quote if previous_sibling is not None else ""
                child_mode, open_quote = "code", ""
                for offset, token, value, state in _code_tokens(content, closes):
                    yield at + offset, token, value
                    child_mode, open_quote = state
                stack.append(_Line(indent, child_mode, open_quote))

            if len(raw) > len(body):
                yield start + len(body), Whitespace, raw[len(body):]


def _code_tokens(line: str, closes: str):
    """Tokens of one code line, each with the (child_mode, open_quote) the line would end in if it stopped there.

    `closes` is the quote a multi-line string on the previous sibling is waiting for, which only the first token may supply.
    """
    i = 0
    first = True
    previous_symbol = ""
    expects_type = False
    # Depth of `[` lists opened directly after a type: `buffer[float]`, whose arguments are types too.
    type_arguments = 0
    last_was_type = False
    code = ("code", "")
    while i < len(line):
        ch = line[i]
        space = _SPACE.match(line, i)
        if space:
            yield i, Whitespace, space.group(), code
            i = space.end()
            continue

        if line.startswith("//", i):
            yield i, Comment.Single, line[i:], (("comment", "") if first else code)
            return

        if ch in _QUOTE_TOKEN and first and ch == closes:
            yield i, _QUOTE_TOKEN[ch], ch, code
            i += 1
            first = False
            continue

        if ch in _QUOTE_TOKEN:
            token = _QUOTE_TOKEN[ch]
            end = _closing_quote(line, i + 1, ch)
            if line[i:].strip() == ch * 3:
                end = -1
            if end < 0 and (not line[i + 1:].strip() or line[i:].strip() == ch * 3):
                # An open quote with nothing after it: the children are the string, and the next sibling closes it.
                is_raw = ch != '"' or line[i:].strip() == '"""'
                yield i, token, line[i:], ("raw-string" if is_raw else "string", ch)
                return
            if end < 0:
                yield i, Error, ch, code
                yield i + 1, token, line[i + 1:], code
                return
            yield from _quoted(line, i, end + 1, token, code)
            i = end + 1
            first = False
            previous_symbol = ""
            continue

        symbol = _SYMBOL.match(line, i)
        if symbol:
            word = symbol.group()
            end = symbol.end()
            if word[0].isdigit():
                # `1.5` is a number, `1..<4` is a range and `1.x` is a member access.
                fraction = _FRACTION.match(line, end)
                if fraction:
                    end = fraction.end()
                exponent = _EXPONENT.match(line, end)
                # In a hex literal `e` is a digit, so only `p` introduces an exponent there.
                if exponent and (not word.lower().startswith("0x") or line[end - 1] in "pP"):
                    end = exponent.end()
                yield i, Number, line[i:end], code
            else:
                fused_call = line.startswith("(", end)
                token = _classify(word, previous_symbol, expects_type or type_arguments > 0, fused_call)
                yield i, token, word, code
                last_was_type = token is Name.Class
                expects_type = expects_type and word in _TYPE_QUALIFIERS
                previous_symbol = word
                i = end
                first = False
                continue
            previous_symbol = word
            expects_type = False
            last_was_type = False
            i = end
            first = False
            continue

        operator = _OPERATOR.match(line, i)
        if operator:
            yield i, Operator, operator.group(), code
            expects_type = operator.group() == "->"
            i = operator.end()
        elif line.startswith("::", i):
            yield i, Punctuation, "::", code
            i += 2
        elif ch in ".,;:()[]{}":
            yield i, Punctuation, ch, code
            if ch == "[" and (last_was_type or type_arguments > 0):
                type_arguments += 1
            elif ch == "]" and type_arguments > 0:
                type_arguments -= 1
            expects_type = ch == ":"
            i += 1
        else:
            yield i, Error, ch, code
            i += 1
        first = False
        previous_symbol = ""
        last_was_type = False


def _classify(word: str, previous_symbol: str, expects_type: bool, fused_call: bool):
    if word.startswith("@"):
        return Name.Decorator
    if word.startswith("#"):
        return Number.Hex
    if word == "_":
        return Keyword.Constant
    if word in _CONTROL_KEYWORDS:
        return Keyword.Namespace
    if word in _DECLARATION_KEYWORDS:
        return Keyword
    if word in _WORD_OPERATORS:
        return Operator.Word
    if word in _CONSTANTS:
        return Keyword.Constant
    if previous_symbol in _NAMES_FUNCTION or fused_call:
        return Name.Function
    if previous_symbol in _NAMES_TYPE or expects_type:
        return Name.Class
    return Name


def _closing_quote(line: str, start: int, quote: str) -> int:
    i = start
    while i < len(line):
        if line[i] == "\\":
            i += 2
        elif line[i] == quote:
            return i
        else:
            i += 1
    return -1


def _interpolated(line: str, start: int, end: int, token, has_escapes: bool):
    """A stretch of string text as (offset, token, value), with its escapes and interpolations picked out."""
    pattern = re.compile(_ESCAPE.pattern + "|" + _INTERPOLATION.pattern) if has_escapes else _INTERPOLATION
    at = start
    for found in pattern.finditer(line, start, end):
        if found.start() > at:
            yield at, token, line[at:found.start()]
        is_escape = found.group().startswith("\\") or found.group() == "$$"
        yield found.start(), String.Escape if is_escape else String.Interpol, found.group()
        at = found.end()
    if end > at:
        yield at, token, line[at:end]


def _quoted(line: str, start: int, end: int, token, state):
    if token is String.Double:
        yield start, token, line[start], state
        for offset, kind, value in _interpolated(line, start + 1, end - 1, token, True):
            yield offset, kind, value, state
        yield end - 1, token, line[end - 1], state
        return
    at = start
    for escape in _ESCAPE.finditer(line, start, end - 1):
        if escape.start() > at:
            yield at, token, line[at:escape.start()], state
        yield escape.start(), String.Escape, escape.group(), state
        at = escape.end()
    if end > at:
        yield at, token, line[at:end], state

