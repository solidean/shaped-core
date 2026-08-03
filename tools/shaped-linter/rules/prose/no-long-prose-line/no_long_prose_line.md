# no-long-prose-line

Line length in prose is free — typically 20-150 characters, whatever the point needs — but 200 is the hard ceiling.
A point that long almost always holds two, and wants splitting at the seam rather than wrapping.

This is the companion to `no-flow-prose`, and it exists because of the shape that one cannot see.
`no-flow-prose` finds a reflowed block by its tell, a sentence ending mid-line.
A single enormous sentence carries no interior full stop anywhere, so nothing about it looks wrong to that rule.

The measurement is characters, not bytes.
This tree's prose is full of em dashes and ellipses, three UTF-8 bytes each, and a byte count would report a comfortable 190-character line as over.

The exact boundary — 200 passes, 201 fires — is pinned in `no_long_prose_line-test.cc`, where the line can be generated to a precise width.
This file covers the shapes instead.

Blocks below are annotated with `[no-long-prose-line]` for "fires once here" and `~[no-long-prose-line]` for "must stay quiet".
`path="…"` is what picks the language: the fence word says what the block *is*, the path says what it is linted *as*.
A block with no path is linted as C++.

## The shape it fires on

One point that runs on well past the ceiling.

```cpp [no-long-prose-line]
// this single point runs on and on without ever ending a sentence in the middle of the line, which is exactly why no-flow-prose cannot see it, and it keeps going well past where a reader wanted the seam, and then further still
```

The same in a doc comment.

```cpp [no-long-prose-line]
/// what it does, described at such length that the description itself becomes the problem, running past two hundred characters without a single interior full stop to give no-flow-prose anything at all to catch it by
```

Markdown body text is prose like any other.

````md [no-long-prose-line] path="a.md"
A paragraph line in a document can run just as long as a comment can, and when it does it is just as hard to skim, because the eye has no idea where the next point begins or whether one ever does at all
````

So is a Python comment.

```py [no-long-prose-line] path="a.py"
# the same rule binds every language a human writes sentences in, so a Python comment that runs past the ceiling is a finding exactly as a C++ one is, with no special casing anywhere in the rule at all
```

A line can break both prose rules at once, and then both fire.

```cpp [no-flow-prose] [no-long-prose-line]
// the first point ends here. the second one then runs on and on, well past the two hundred character ceiling that this rule enforces, so this one single line manages to be both a reflowed block and an over-long one at the very same time
```

## The shape it stays quiet on

A line comfortably inside the ceiling.

```cpp ~[no-long-prose-line]
// one point, written out plainly
```

A long line whose longest unbreakable run is itself over the ceiling.
No way of breaking it brings it under, so it is not a wrapping mistake and there is nothing to report.

```cpp ~[no-long-prose-line]
// see https://example.com/a/very/long/generated/path/that/nobody/would/ever/want/to/break/across/two/lines/because/it/is/one/single/unbreakable/token/from/its/start/all/the/way/through/to/its/finish/right/here
```

Code is not prose, however long the line.

```cpp ~[no-long-prose-line]
auto const s = "this is a string literal that runs well past two hundred characters, and it is code rather than prose, so no prose rule reads it at all, whatever it happens to contain along its length";
```

A fenced code block inside markdown is not prose either.

````md ~[no-long-prose-line] path="a.md"
```py
x = "a very long line of sample code that runs past the ceiling, sitting inside a fence where it is content rather than something anybody is being asked to read as a sentence, so it is left alone"
```
````
