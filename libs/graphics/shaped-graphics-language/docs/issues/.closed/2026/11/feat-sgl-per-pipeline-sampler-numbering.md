---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/emit.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
closed: 2026-11-27
resolution: fixed
---

# Compact file-sampler numbering per `pipeline` declaration

A file-scope sampler's index is its position among all the file's samplers (EMIT-133).
So a file past 16 is `too-many-samplers` even where each stage reaches few.

The way out not taken yet: a `pipeline` numbers only the samplers its stages reach, and emits those stages with its numbers.
It costs an entry point's text depending on its pipeline, and a stage shared by two pipelines compiling twice.
A hand-assembled pipeline keeps declaration order.

## Resolution (branch `jn/sgl-link`, under review)

Generalized by `sgl::link` ([linking.md](../../../../spec/semantics/linking.md), LINK-1 to LINK-9).
A host's group of entries, from any files, numbers the samplers its stages reach once, for the pipeline.
The first stage's file keeps its whole table, so its output is the one compiled alone; the others append what they reach, dense.
EMIT-133 gained the sentence that a link's numbering replaces the program's.
Julius and Philip review the branch before it merges; a `pipeline` declaration naming a group is linking.md's open item.
