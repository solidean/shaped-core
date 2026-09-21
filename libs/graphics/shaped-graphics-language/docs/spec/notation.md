# Notation

SGL supports configurable replacements inside symbols.
The main intended use is convenient input of mathematical identifiers that are heavy in Unicode.
Back to the [specification](_index.md).

## When notation applies

* Notation applies as if during name lookup, together with symbol resolution.
* Parsing never needs it: the [form tree](syntax/forms.md#atoms) keeps each spelling as written.
* A notation is importable from a module, like any other name.
* Notation applies to the names of types, functions and variables, and never to the names of modules.
* A declaration is `notation <id> => <id>`.
* A keyword is not an identifier, so a keyword is never the target of a notation.
* The quick fix does not apply when its result spells a keyword.

## Replacements

For example:

```sgl
notation \phi => φ
notation \theta => θ
notation \Delta => Δ
```

means that:

```sgl sketch
\phi
\phi_1
d\theta
\Delta_pos
```

are canonically written as:

```sgl sketch
φ
φ_1
dθ
Δ_pos
```

These replacements only operate inside symbol tokens.
They can therefore never introduce or remove syntactic structure such as operators, parentheses, comments or indentation.
The replacement system is deliberately not a general recursive string rewrite system.

## Resolving the table

The replacement definitions themselves are resolved recursively first.

For example:

```sgl
notation a => b
notation b => c
notation dc => e
```

is resolved to:

```text
a => c
b => c
dc => e
```

Cycles during this resolution are an error.

So:

```sgl sketch
notation a => b
notation b => a
```

is invalid.

## Replacing a symbol

After the replacement table has been resolved, source symbols are replaced in exactly one non-recursive pass.
The original symbol is scanned from left to right.
At each position the longest matching left side is taken.
If no replacement matches, the original code point is kept and the scan continues.
All matched segments are then replaced independently, each by its fully resolved right side.
Importantly, replacement output is not scanned again.

For example, given:

```text
a => c
b => c
dc => e
```

the symbol `da` is segmented as `d | a`, and therefore becomes `dc`.
It does NOT become `e`.
This is intentional.
Replacement canonicalizes pieces of the spelling that were present in the original source.
It does not recursively interpret strings that replacement created.

## Diagnostics and formatting

Name lookup operates on the canonical symbol.
The original source spelling is preserved for diagnostics, formatting and source spans.
Using a replaceable spelling is a normal error.

For example:

```sgl sketch
let x = \phi
```

is interpreted exactly as:

```sgl sketch
let x = φ
```

but produces a diagnostic with a quick fix that replaces `\phi` with `φ`.
The formatter applies these replacements automatically.
ASCII-friendly spellings can therefore be used for input, while formatted source converges towards the canonical notation.
Replacement rules may map arbitrary strings to arbitrary strings as long as they stay within symbol tokens.
The Unicode aliases are merely the main intended use case, not a special language feature.

## Open

* The name of the diagnostic for a replaceable spelling.
* Whether a notation may produce a spelling that starts with a digit, `@` or `#`, which would change the kind of form.
