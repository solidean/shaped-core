---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/emit.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
---

# Compact file-sampler numbering per `pipeline` declaration

A file-scope sampler's index is its position among all the file's samplers (EMIT-133).
So a file past 16 is `too-many-samplers` even where each stage reaches few.

The way out not taken yet: a `pipeline` numbers only the samplers its stages reach, and emits those stages with its numbers.
It costs an entry point's text depending on its pipeline, and a stage shared by two pipelines compiling twice.
A hand-assembled pipeline keeps declaration order.
