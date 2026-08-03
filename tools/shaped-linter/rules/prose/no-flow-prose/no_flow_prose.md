# no-flow-prose

Prose is one semantic point per line, never a reflowed block.
A sentence that ends in the *middle* of a line means two points were packed onto one, so that is what this rule looks for.

The detector is one shape: `<word>. <more text>`.
Two short sentences on one line are fine when the line ends with the second one, and a long sentence that had to wrap carries no interior full stop at all.

The rule deliberately carries no fix, because obeying it means modelling the prose rather than splicing in a newline.
Acting on more than a couple of findings is `prose apply`'s job: it takes a plan of rewrites across many files and lands them in one pass, and the `reworking-prose` skill is the workflow around it.
Repairing a hit in isolation is the failure mode both exist to prevent.

Blocks below are annotated with `[no-flow-prose]` for "fires once here" and `~[no-flow-prose]` for "must stay quiet".
`path="…"` is what picks the language: the fence word says what the block *is*, the path says what it is linted *as*.
A block with no path is linted as C++.

## The shape it fires on

A sentence ending mid-line.

```cpp [no-flow-prose]
// One point. And a second one.
```

The same in a doc comment.

```cpp [no-flow-prose]
/// What it does. And how it does it.
```

And in a block comment, on whichever of its lines the seam sits.

```cpp [no-flow-prose]
/* One point. And a second one.
 * A third, on its own line.
 */
```

Only the first seam on a line is reported, so a badly reflowed paragraph does not produce a wall.

```cpp [no-flow-prose]
// One. Two. Three. Four.
```

Two reflowed lines are two findings, though — each line is judged on its own.

```cpp [no-flow-prose] [no-flow-prose]
// One point. And a second one.
// A third point. And a fourth.
```

## The shape it stays quiet on

A line that ends where its sentence does.

```cpp ~[no-flow-prose]
// One point.
// And a second one.
```

Trailing whitespace does not change that.

```cpp ~[no-flow-prose]
// One point.   
```

A sentence long enough that it plainly had to wrap.

```cpp ~[no-flow-prose]
// a sentence long enough that it had to break somewhere,
// and it carries no interior full stop to give it away
```

A line with no sentence end at all.

```cpp ~[no-flow-prose]
// front-loaded, no full stop anywhere
```

Code is not prose, however many sentences it appears to hold.

```cpp ~[no-flow-prose]
auto const s = "One point. And a second one.";
```

## The exclusions

`e.g.` and `i.e.` are the commonest false positive, and are excluded by the rule that a sentence-ending word is at least two characters.

```cpp ~[no-flow-prose]
// a list of things, e.g. this one and that one
```

```cpp ~[no-flow-prose]
// the other way round, i.e. backwards
```

A known abbreviation likewise.

```cpp ~[no-flow-prose]
// vectors, strings, etc. are all in clean-core
```

```cpp ~[no-flow-prose]
// every integer type (incl. `signed char`) is fully featured
```

A member access is a dot with no space behind it, so it never reads as a sentence end.

```cpp ~[no-flow-prose]
// call ctx.report to emit a finding
```

Nor does a decimal number.

```cpp ~[no-flow-prose]
// requires CMake 3.28 or newer
```

Inline code is skipped: an odd number of backticks before the dot means we are inside a code span.

````md ~[no-flow-prose] path="x.md"
a call like `f(x). g(y)` in the middle of a line
````

A stop *after* a code span still fires, which is the point of counting rather than ignoring backticks.

````md [no-flow-prose] path="x.md"
this is `cc::vector`, done. And here is the next point.
````

## Markdown

Body text is prose.

````md [no-flow-prose] path="x.md"
One point. And a second one.
````

A heading is too.

````md [no-flow-prose] path="x.md"
## One point. And a second one.
````

An ordered-list marker is not a sentence end — its word is all digits.

````md ~[no-flow-prose] path="x.md"
1. the first step, written out
12. the twelfth step, written out
````

A fenced code block is never prose, which is what lets this very file hold bad examples.

````md ~[no-flow-prose] path="x.md"
```py
x = 1  # One point. And a second one.
```
````

A pipe table is laid out in columns rather than lines, so it is not read as prose either.

````md ~[no-flow-prose] path="x.md"
| what | when |
|---|---|
| one. two | now. later |
````

## Python

A comment.

```py [no-flow-prose] path="x.py"
# One point. And a second one.
```

A docstring, line by line.

```py [no-flow-prose] path="x.py"
def f():
    """One point. And a second one."""
```

An interior line of a multi-line docstring just as much.

```py [no-flow-prose] path="x.py"
def f():
    """Summary.

    One point. And a second one.
    """
```

A triple-quoted string that does not open a logical line is data, not documentation.

```py ~[no-flow-prose] path="x.py"
x = """One point. And a second one."""
```

An ordinary string is never prose.

```py ~[no-flow-prose] path="x.py"
x = 'One point. And a second one.'
```

## Corner-cuts, pinned as they behave

Recorded so the boundary cannot move silently — not because the behavior is right.

An abbreviation outside the small known list reads as a sentence end.
Growing the list is the fix, and the list is deliberately short until a real false positive earns an entry.

```cpp [no-flow-prose]
// see Prof. Knuth on the subject
```

A sentence ending on a quoted or parenthesized word puts the closing punctuation between the word and the dot, so the word run stops early and the line is judged by whatever precedes it.

```cpp ~[no-flow-prose]
// the state is now "done". And the next point follows
```

An abbreviation at the *end* of a line is quiet for the same reason every line-final stop is — the rule only ever looks at interior ones.

```cpp ~[no-flow-prose]
// vectors, strings, etc.
```
