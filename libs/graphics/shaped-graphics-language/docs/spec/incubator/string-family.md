# The String Family

*Incubator: not normative.*

## The idea

The simple default is `"`, with interpolation as its convenience.
Around it sits a small family, arranged so that the common case needs the least ceremony:

| spelling | escapes | interpolation | status |
|---|---|---|---|
| `"text"` on one line | backslash escapes | `$` | specified |
| `"` … `"` multi-line | none | `$` | specified |
| `"""` … `"""` multi-line | none | none — fully raw | reserved |
| `"lang` … `"` | none | `$` | reserved |
| `"""lang` … `"""` | none | none | reserved |

**A multi-line string needs no escapes** because indentation delimits it: no character inside it can close it.
So a line holding a single `"` is the string `"`, and the only special character is the `$` of interpolation.
`"""` removes even that.

**A language tag** names what the content is:

```sgl sketch
let s = "json
    { "a": $a }
"
```

A tag lets an editor highlight the content, lets a linter check it, and could let the compiler validate it.
It is not expected to matter much in a shading language.
It is here because SGL is also a test run for a general-purpose language ([beyond-shaders.md](beyond-shaders.md)), where it would.

Comments may get the same treatment: embedded markdown, and embedded languages inside it.

## What it touches

* The tokenizer: two more opener shapes, and a mode without interpolation.
* The highlighters, which can delegate tagged content to another grammar.

## Already fixed by the syntax

* `"""` and a tagged opener — a quote followed by exactly one symbol and the end of the line — tokenize as openers today and draw a "reserved" normal error.
  No existing source changes meaning on the day they are given one.
* Interpolation applies to double quotes only; single quotes and backquotes are tokenized and have no meaning yet.

## Open

* What single quotes and backquotes are for: characters, raw one-line strings, symbols, embedded source.
* Whether a tag is a free symbol or must name something declared.
* Whether `$` interpolation inside a tagged string is escaped for the tagged language, for example JSON-quoted.
