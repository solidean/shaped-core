# no-whole-point

Prose says what a design achieves, rather than declaring a property to be its point.
`which is the whole point` asserts importance; `so a caller never waits` shows it, in the same number of words.

The detector is a list of phrase patterns in `no_whole_point.cc`, matched over a whole prose block, so a phrase wrapped across lines is found too.
In geometry code most points are points, so nearly every pattern demands a clause end after `point` or a declaring subject before it.
It carries no fix: the rewrite is what the property buys, and only the writer knows it.

The block annotations are specified in [docs/coding-guidelines.md](../../../docs/coding-guidelines.md#the-corpus-format).

## The shapes it fires on

The intensified declaration, wherever it sits.

```cpp [no-whole-point]
// Reserved now rather than when something goes wrong, which is the whole point.
```

````md [no-whole-point] path="x.md"
The whole point of the seam is that the core knows no GPU.
````

````md [no-whole-point] path="x.md"
Opacity to the caller's translation unit is the entire point.
````

A property declared the point, closing its clause.

```cpp [no-whole-point]
/// Allocation-free, which is the point: a route handler should not build a vector.
```

````md [no-whole-point] path="x.md"
The asymmetry is the point: emptying and filling are both narrow.
````

```cpp [no-whole-point]
// That is the point, and it is also why the first check reports more.
```

The point of doing something.

```cpp [no-whole-point]
// The indexes are partial, and that is the point of having them.
```

The contraction, with either apostrophe.

```cpp [no-whole-point]
// cache hits replay the same numbers — that's the point
```

```cpp [no-whole-point]
// cache hits replay the same numbers — that’s the point
```

`the point is` introducing a purpose.

```cpp [no-whole-point]
// Runs after a code-changed error: the point is to report everything in one pass.
```

```cpp [no-whole-point]
// Serialized on purpose: the point is that the consumer drives the node last.
```

```cpp [no-whole-point]
// Driven by hand because the point here is the store's message order.
```

`the crux`, which has no literal reading in code.

````md [no-whole-point] path="x.md"
The widening multiply is the crux of multi-word arithmetic.
````

A declaration wrapped across lines.

```cpp [no-whole-point]
// Reserved now rather than when something goes wrong, which is the
// whole point of it.
```

## The shapes it stays quiet on

A literal point as a subject.

````md ~[no-whole-point] path="x.md"
Wherever the point is off the surface, they match the segment predicates.
The point is the center of the box, moved to its face.
Fails if the point is not a valid key.
````

A literal point to one side.

````md ~[no-whole-point] path="x.md"
The point is to the left of the plane.
````

A literal point, intensified.

````md ~[no-whole-point] path="x.md"
The point tested is the very point the traces trace to.
Sort the whole point set first.
````

A literal point in parentheses, after code.

````md ~[no-whole-point] path="x.md"
A hit is a parameter of the ray (`r.at(t)` is the point).
````

A threshold.

````md ~[no-whole-point] path="x.md"
Wrong for ten thousand connections, which is the point at which it falls over.
````

Code is never prose.

```cpp ~[no-whole-point]
auto const s = "which is the whole point";
```

## Corner-cuts, pinned as they behave

`the point of` after a subject other than a pronoun stays quiet: a literal point of a segment reads the same.

```cpp ~[no-whole-point]
// Seeing the output IS the point of this command.
```
