---
created: 2026-11-09
priority: someday
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

## The lists, landed on 2026-11-09

A frozen list is now a few shared segments: a freeze merges the newest while the last is no more than twice the size of the next.
So a list holds about log2 of its size of them, a freeze copies the newest entries, and a link shares every segment below its own.
A lookup below the first segment reads a directory of one entry per 256 entries, and the first segment, the prelude's, is held in the list itself.
A link no longer makes the set of every name below it either, which a program behind a link replaces with the names of the files it reaches.

Measured in release, shaped-viewer's 10 modules:

| | before | after |
|---|---|---|
| the 10 links loaded from artifacts | 8.6 ms | 4.1 ms |
| the 9 links after `scene` relocated | 4.5 ms | 3.0 ms |
| the 10 links checked | 28.1 ms | 25.3 ms |
| a keystroke in the chain's first module | 9.6 ms | 7.4 ms |
| a keystroke in a program | 3.70 ms | 3.81 ms |
| the language server benchmark's peak | 369 MiB | 246 MiB |

The keystroke in a program pays the lookup: about 3% more, measured against the old lists on the same tree.

What is left: every `layered_map` freeze still copies the map below it, and the checker's plain vectors and maps are copied whole by every link.

**When to take up the rest:** when memory is what a host runs short of, a library much larger than shaped-viewer's or a host keeping many option permutations alive.
A link loaded from an artifact costs about 0.4 ms now, so the copies matter more for memory than for time.
