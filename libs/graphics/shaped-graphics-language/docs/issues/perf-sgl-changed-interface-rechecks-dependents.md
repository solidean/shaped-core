---
created: 2026-11-09
priority: someday
parent: epic-sgl-separate-compilation
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/relocate.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/own_part.cc
---

# An edit that changes what a module declares checks every later module naming it, its unchanged part included

A link moves past an edit only where the edited module's interface hashes the same (`check::relocate_link`).
An edit that changes it, a function added or a signature typed, refuses every later link holding an id into that module's part, so each of them is checked again.
Most of what such a link names did not change, but ids are positions, and an entry added early in the part moves every one after it.

Measured on 2026-11-09, release, shaped-viewer's 10 modules: a function added to a module moved 3 of the 24 links after the edits the test makes, and checked 21 again.
A body edit of `scene`, the chain's first module, is a 7.4 ms keystroke in the language server; one that changes its interface checks the whole chain, about 25 ms.

**What would cut it:** keep the part of a changed interface that did not change, matching declarations by name and kind across the two checks rather than by position.
A later link then moves where everything it names is matched and hashes the same, and is checked again only where it names what changed.
It is the hardest of the epic's remaining cuts: the match has to cover every list an id names, entries a declaration made but holds no name for among them.

**When to take it up:** when a library is large enough that typing in a signature feels slow in an editor, a keystroke past some 50 ms.
