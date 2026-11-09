---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/prelude.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - test: sgl profile - *
---

# What a compile and a process pay for the prelude

The prelude is checked once per process, and every compile then continues from that checked state.
Both halves still carry work that is not the program's own.
The children name each cut, with what it costs today and the test that measures it.

The checked prelude can be written as an artifact and read back, but no process keeps one for another, so each still checks it once.

## Measured, 2026-11-06

Release build, Windows, a Ryzen 9 9950X3D2, on the 443 KB prelude.

`sgl profile - parsing and checking the prelude itself`, fastest of 20 rounds: parse 4.2 ms, AST build 1.8 ms, check 5.2 ms.
A process pays about 20 ms in all for the prelude, the registry's own parse of `builtins.sgl` included.

`sgl bench`, fastest of 300 rounds: an empty compute shader compiles in 10 µs on every target, the sgl-cube's vertex shader in 130 µs.
Over shader-rosetta's 368 bgfx shaders, the smallest takes 16 µs and the median 148 µs.

What a compile paid for the prelude, cut on 2026-11-06:

- every emit reserved each of the target's reserved words in its name mint, hundreds of hash inserts; they are a set built once per process;
- the checker's passes for recursion, redeclarations, generic instances, unregistered tests and metal traversals walked the whole prelude; they start at the program's layer.

Together that took an empty compile from 66 µs to 10 µs on dx12, and from 93 µs to 10 µs on metal.
The form parser climbs precedence in one loop instead of a frame per level, which took the prelude's parse from 5.6 ms to 4.2 ms.

The test-only builtins (`test_texel` and its siblings) are about 16 KB of the prelude, some 4% of what a process pays for it.
A compile pays nothing for them, since no pass of a program's check walks the prelude any more.

Saving checked layers removes the check from a process that reads the prelude back: 5.8 ms became a 0.68 ms load on 2026-11-08.
It belongs to [epic-sgl-separate-compilation](epic-sgl-separate-compilation.md).

## Measured in a fresh process, 2026-11-08

Release `sgl emit` of the sgl-cube's vertex shader, with the checked prelude read back from the tool's file.

- **The builtin registry: 11 ms.** Registering the records takes 2.2 ms, and `finalize` 8.6 ms, which is the parse of `builtins.sgl`, 387 KB of the prelude's 467 KB.
  `finalize` reads each record's name and parameters back from that parse, and the prelude's first file is the same parse.
- **The two hand-written files' parse: about 1 ms.**
- **Reading the checked layer back: 2.3 ms,** the 1.6 MB file's read included.

Putting the parses into the artifact was tried, and lost: the artifact grew to 9.6 MB, and reading it back cost what parsing does.
So what is left is the parse itself: a faster parse, or a `builtins.sgl` with less in it, are the cuts.
