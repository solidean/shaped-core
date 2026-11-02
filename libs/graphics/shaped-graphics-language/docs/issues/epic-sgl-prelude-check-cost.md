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

The persistent shader cache of slib does not skip the prelude check, so this cost is paid once per process wherever a pipeline is acquired (shaped-shader-library's `docs/structure.md`).

## Measured, 2026-11-01

`sgl profile - parsing and checking the prelude itself`, release, fastest of 20 rounds, on the 354 KB prelude:
parse 6.2 ms, AST build 2.3 ms, check 6.4 ms — about 15 ms per process.
A compile behind the shared prelude pays 0.10 ms for a small program, 0.05 ms for an empty one.

The parse's remaining time is spread evenly down the form parser's precedence chain, one `parse_level` frame per level and operand.
Single-loop precedence climbing is the next cut there, worth an estimated 1–2 ms.

Step 3 of the compilation model, saving checked layers (`docs/spec/incubator/compilation-model.md`), would remove the 15 ms.
It is shelved: a cache with its invalidation does not pay for 15 ms per process yet.
What would reopen it: many short-lived processes compiling, or a prelude that grows well past today's size.
