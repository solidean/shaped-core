---
created: 2026-11-22
tags: [porting-feedback]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_local_indices.cc
---

# CHK-538 judges an array a struct local holds, and an element of a temporary array

CHK-538 asks a local array indexed from data to state `@indexed(.select)` or `@indexed(.memory)`, and judges only a local whose own type is an array.
Two shapes a target keeps in memory just the same are not judged yet:

- An array inside a struct local, `s.arr[i]` with `i` from data: the annotation would stand on the struct's `let` and name the field, or on the field of the struct.
- An element of a temporary array that no `let` holds, `f()[i]` or `[a, b, c][i]` with `i` from data: no local roots it, so there is nothing to annotate but the expression.

Each is today what `.memory` would state, so nothing is wrong, only unflagged.
