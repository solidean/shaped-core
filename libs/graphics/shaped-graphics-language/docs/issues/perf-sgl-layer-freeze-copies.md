---
created: 2026-11-09
parent: epic-sgl-separate-compilation
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/layered.hh
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/shared_library.cc
---

# Every layer of a chain holds a whole copy of the tables below it

`layered::freeze` copies the shared lower part and the layer's own entries into one new vector.
That was free with two layers, the prelude and a program, and a chain of module layers pays it once per link.
So a library of n modules keeps n copies of growing tables, each link the whole state up to its module.

Measured on 2026-11-09, release, the language server's benchmark behind shaped-viewer's 10 modules:
with links kept by a cache of their own, 742 MiB peak; with links kept only by the libraries alive, at most 4, 400 MiB.

What would cut it: a frozen table as a list of shared segments, one per layer, so a link adds its own and shares the rest.
A lookup then finds its segment, one of at most a few tens, where today it compares against one base size.
That lookup is on the checker's hottest path, so it is a measurement before it is a change.
