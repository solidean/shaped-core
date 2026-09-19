# Ranges and Iteration

*Incubator: not normative.*

## The idea

`range` is a **builtin type** with a few simple operations, written `a..<b` (exclusive) or `a..=b` (inclusive).
A plain `..` is never a range: the reader must always see which end is meant.

A range is a value like any other:

```sgl
let r = 0..<10
let inside = x in r
let also = x in 0..<1
```

`in` is an operator that tests membership, and it is the same `in` a loop uses.

**Loops come in one form only:**

```sgl
for i in 0..<count:
    total += weight i
```

`for <var> in <range>:` — nothing else, for now.

**Later: custom iterators.**
The loop form may open up to user-defined iteration, which is what voxel tracing and grid tracing want.
A DDA walk through a grid reads far better as a `for` over an iterator than as a hand-rolled `loop`.
Because functions inline completely ([function-model.md](function-model.md)), an iterator protocol costs nothing at runtime.

## What it touches

* The builtin type set: `range`, and which element types it admits (integers certainly, floats for membership only).
* The AST phase: `for` accepts exactly one `in` expression.
* A future iterator protocol, and how a loop over one is inlined.

## Already fixed by the syntax

* `..<` and `..=` are single operator tokens, at their own non-associative level between the bit-like operators and the ascription-like operators.
* `in` is a word operator sharing one left-associative level with `:`, `->` and `as`, so `x as int in 0..=10 : bool` reads left to right.
* A keyword form takes whole expressions, so `for i in 0..<n:` is `for` plus the single expression `i in 0..<n`.
* A range operator is exempt from the rule that infix operators need spaces, so `0..<4` and `0 ..< 4` are both accepted.

## Open

* Stepped and reversed ranges.
* Whether `in` also tests membership in arrays or sets, or only in ranges.
* The shape of the iterator protocol.
