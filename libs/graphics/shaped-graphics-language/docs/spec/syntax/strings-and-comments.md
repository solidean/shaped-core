# Strings and comments

Comments and quoted literals are the two token kinds that can own lines below them.
Both follow the [line tree](line-tree.md): they own children, and never anything else.
Back to the [phases](_index.md); the reasons are in [why/strings-and-comments.md](why/strings-and-comments.md).

## Comments

* **CMT-1** `//` starts a comment, and the comment ends with its line ([why](why/strings-and-comments.md#cmt-1)).
* **CMT-2** `///` starts a documentation comment ([why](why/strings-and-comments.md#cmt-2)).
* **CMT-3** A **comment line** is a code line whose only token is a comment.
* **CMT-4** A comment line owns every line below it that is indented deeper: they are comment lines, whatever they contain ([why](why/strings-and-comments.md#cmt-4)).
* **CMT-5** The lines a documentation comment owns are part of that documentation comment.
* **CMT-6** A comment that trails code ends with its line, and the children of that line are read as if the comment were not there.
* **CMT-7** Inside a quoted literal, `//` is content.

```sgl
// one marker comments a paragraph
   because every deeper line belongs to it
      whatever its indentation
    or its content: let x = "

let x = 0 // a trailing comment

/// Documents `scale`.
    The continuation aligns with the text above it.
let scale = 2
```

Commenting out a header comments out its body.

```sgl
// if ready:
    let a = 1
let b = 2
```

| line | kind |
|---|---|
| `// if ready:` | comment |
| `let a = 1` | comment |
| `let b = 2` | code |

## One-line strings

* **STR-1** A quoted literal opens with `"`, `'` or a backquote, and closes with the same character.
* **STR-2** A **one-line string** opens and closes on one line.
* **STR-3** In a one-line string a backslash starts an escape.

| escape | stands for |
|---|---|
| `\\` | a backslash |
| `\"` `\'` and backslash backquote | that quote character |
| `\n` `\r` `\t` `\0` | line feed, carriage return, tab, the zero byte |
| `\u{…}` | the code point with that hexadecimal number |

* **STR-4** Any other escape is the normal error `unknown-escape`, and it stands for the character after the backslash.
* **STR-5** There is no escape for `$`: a literal dollar is `$$` ([interpolation](#interpolation)).
* **STR-6** A quoted literal with content and no closing quote on its line is the normal error `undelimited-string`, and it closes at the end of the line ([why](why/strings-and-comments.md#str-6)).

```sgl
print "hello world"
print "a tab:\t and a quote: \""
```

The first line below reports `undelimited-string`, and the second line is read as code.

```sgl error
print "hello
let x = 1
```

## Multi-line strings

* **STR-7** A quote that is the last token of its line opens a **multi-line string** ([why](why/strings-and-comments.md#str-7)).
* **STR-8** Whitespace after the opening quote does not change STR-7, and a lint reports it.
* **STR-9** A quote that a comment follows does not open a multi-line string: the comment marker is content, and STR-6 applies.
* **STR-10** The children of the opening line are the content, and they are tokenized in string content mode.
* **STR-11** The next sibling of the opening line must start with the closing quote, and it continues as code after that quote.
* **STR-12** When the next sibling does not start with the closing quote, or there is no next sibling, that is the normal error `missing-string-end`.
* **STR-13** After `missing-string-end` the string closes where its children end, and the sibling is read as if nothing were open.
* **STR-14** This is the only form: the opening line holds no content.

```sgl
print "
    hello world
    second row
"
```

### Content

* **STR-15** The value is the content lines joined by `\n`, with no `\n` after the last one.
* **STR-16** Each content line loses the indentation width of the opening line plus 4 columns.
* **STR-17** A content line keeps the indentation it has beyond that.
* **STR-18** A content line with less indentation than that loses what it has, and it is the normal error `underindented-string-content`.
* **STR-19** A blank line between two content lines is an empty content line.
* **STR-20** A blank line after the last content line is not content ([LINE-18](line-tree.md#blank-lines)).
* **STR-21** A multi-line string has no backslash escapes: a backslash is content ([why](why/strings-and-comments.md#str-21)).
* **STR-22** A quote character in the content is content.

```sgl
let text = "
    first

        indented by four
    say "hi" \ no escape here

"
```

| content line | value line |
|---|---|
| `    first` | `first` |
| blank | empty |
| `        indented by four` | four spaces, then `indented by four` |
| `    say "hi" \ no escape here` | `say "hi" \ no escape here` |
| blank, before the closing quote | not content |

A content line that holds a single `"` makes the string `"`.

```sgl
let quote = "
    "
"
```

### Closing and going on

The closing quote is the first token of the sibling, and what follows it is ordinary code.
That code may end with an open quote again ([GRP-19](groups.md#closing)).

```sgl
log("
    first part
" + "
    second part
")
```

## Interpolation

* **STR-23** Interpolation exists in double-quoted strings only, one-line and multi-line alike ([why](why/strings-and-comments.md#str-23)).
* **STR-24** `$name` interpolates a symbol.
* **STR-25** `$name.member.member` interpolates a symbol and the run of fused `.symbol` pairs after it.
* **STR-26** `$(expr)` interpolates an expression: the content of the parentheses is tokenized as code, and the `)` must be on the same line.
* **STR-27** `$$` is a literal dollar.
* **STR-28** A `$` followed by anything else is the normal error `stray-dollar`, and it stands for itself.
* **STR-29** The token phase and the form phase accept interpolation; the AST rejects it until its semantics exist.

```sgl
print "total: $total of $stats.count, that is $(100 * total / stats.count)%"
print "price: $$5"
```

The `$` before the space reports `stray-dollar`.

```sgl error
print "cost in $ is unknown"
```

## Reserved openers

* **STR-30** `"""` as the last token of its line is tokenized as an opening quote, and it is the normal error `reserved-string-opener`.
* **STR-31** A quote followed by exactly one symbol and then the end of the line is a **tagged opener**: it is tokenized as an opening quote, and it is the normal error `reserved-string-opener`.

```sgl sketch
let shader = """
    raw text, $no interpolation
"""

let config = "json
    { "width": 4 }
"
```

## Other quotes

* **STR-32** `'…'` and backquoted literals tokenize as double-quoted literals do, without interpolation.
* **STR-33** They have no meaning yet, and the AST rejects them.

## Open

* How a `$(` without a `)` on its line recovers.
* Whether the sibling after a `"""` opener must start with `"""` or with `"`.
* Whether a tagged opener allows whitespace between the quote and the symbol, and whether it exists for all three quote characters.
* Whether a `\u{…}` escape with no digits, too many digits or a surrogate value is `unknown-escape` or a kind of its own.
