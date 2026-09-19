# Tokens

The token phase reads each line of the [line tree](line-tree.md) into tokens.
It never looks past the line it reads.
Back to the [phases](_index.md); the reasons are in [why/tokens.md](why/tokens.md).

## Modes

* **TOK-1** A line is tokenized in one of three modes: code, comment, or string content.
* **TOK-2** A line in comment mode is one comment token, from the end of its indentation to the end of the line.
* **TOK-3** A line in comment mode hands comment mode to its children.
* **TOK-4** A line in string content mode is string body, from the end of its indentation to the end of the line, split only by [interpolation](strings-and-comments.md#interpolation).
* **TOK-5** A line in string content mode hands string content mode to its children.
* **TOK-6** A line in code mode hands a mode to its children by how it ends ([why](why/tokens.md#tok-6)).

| the code line | hands to its children |
|---|---|
| holds only a comment | comment mode |
| ends with an open quote | string content mode |
| ends in any other way | code mode |

* **TOK-7** A code line that ends with an open quote hands its next sibling the duty to start with the closing quote ([strings](strings-and-comments.md#multi-line-strings)).
* **TOK-8** Tokenizing a line yields three things and nothing else: its tokens, the mode of its children, and the duty of its next sibling.

## Code tokens

* **TOK-9** Every byte of a code line after its indentation belongs to exactly one token or to the whitespace between two tokens.
* **TOK-10** Whitespace is a run of spaces and tabs, and it is not a token.
* **TOK-11** At each position the longest token that matches is taken ([why](why/tokens.md#tok-11)).

### Symbols

* **TOK-12** A **symbol** starts with an ASCII letter, a digit, `_`, `@`, `#`, `\`, or any code point outside ASCII.
* **TOK-13** A symbol continues with the same characters and with `'`.
* **TOK-14** `'` never starts a symbol.
* **TOK-15** There is no keyword token, number token, attribute token or identifier token: each of these is a symbol ([why](why/tokens.md#tok-15)).
* **TOK-16** `-` is not a symbol character ([why](why/tokens.md#tok-16)).
* **TOK-17** The **wildcard** is the symbol that is exactly `_`.

| source | tokens |
|---|---|
| `10a7` | one symbol |
| `@range` | one symbol |
| `#ff00bb` | one symbol |
| `\phi_1` | one symbol |
| `x'` | one symbol |
| `1'000` | one symbol |
| `a-b` | symbol, operator, symbol |
| `_` | wildcard |

Names are written in snake_case.

```sgl
let half_width = width' / 2
```

### Punctuation

* **TOK-18** `.` is DOT, `,` is COMMA, `;` is SEMICOLON and `:` is COLON.
* **TOK-19** `::` is DOUBLE_COLON, and every DOUBLE_COLON is the normal error `double-colon` ([why](why/tokens.md#tok-19)).
* **TOK-20** `->` is ARROW and `=>` is DOUBLE_ARROW.
* **TOK-21** `(`, `)`, `[`, `]`, `{` and `}` are one token each.

The line below reports `double-colon`; the member is written `color.red`.

```sgl error
let c = color::red
```

### Operators

* **TOK-22** An **operator** is a run of the operator characters `!` `+` `-` `*` `/` `%` `=` `<` `>` `?` `&` `^` `|` `~` ([why](why/tokens.md#tok-22)).
* **TOK-23** A run that is exactly `->` or `=>` is the token of TOK-20, not an operator.
* **TOK-24** Two dots open an operator that continues with operator characters, so `..<`, `..=` and `..` are one operator each.
* **TOK-25** An operator stops before `//`.

| source | tokens |
|---|---|
| `a <<= 2` | symbol, operator `<<=`, symbol |
| `0..<n` | symbol, operator `..<`, symbol |
| `x =>> y` | symbol, operator `=>>`, symbol |
| `a +// note` | symbol, operator `+`, comment |

### Quotes and comments

* **TOK-26** `"`, `'` and the backquote open a quoted literal, which is a quote open token, string body, and a quote close token ([strings](strings-and-comments.md#one-line-strings)).
* **TOK-27** `//` starts a comment token that runs to the end of the line ([comments](strings-and-comments.md#comments)).
* **TOK-28** A comment that starts with `///` is a documentation comment: the same token kind, with a flag.

### Unknown bytes

* **TOK-29** A run of bytes that starts no other token is one error token, and it is the normal error `unknown-character`.
* **TOK-30** Bytes that are not valid UTF-8 are part of an error token.

The `$` below is outside a string, so it reports `unknown-character`.

```sgl error
let x = $ + 1
```

## Fused

* **TOK-31** A token is **fused** when it touches the token before it: both are on one line and no whitespace is between them ([why](why/tokens.md#tok-31)).
* **TOK-32** The first token of a line is not fused.
* **TOK-33** Fused is derived from the byte spans, and there is no token that stands for it.
* **TOK-34** A fused colon and a colon after whitespace mean the same.

| source | the second token is |
|---|---|
| `f(x)` | fused |
| `f (x)` | not fused |
| `a.b` | fused |
| `a .b` | not fused |

## Numbers

* **TOK-35** A number is not a token ([why](why/tokens.md#tok-35)).

| source | tokens |
|---|---|
| `1.5` | symbol, DOT, symbol |
| `1e-5` | symbol, operator, symbol |
| `1..<4` | symbol, operator, symbol |

The form phase assembles them, by the rules in [numbers.md](numbers.md).

## Open

* The exact Unicode ranges a symbol accepts; today every code point outside ASCII is accepted.
* How three or more dots in a row tokenize.
* Whether a control character other than a tab is whitespace or an error token.
