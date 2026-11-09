---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/impl/front_end.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md
---

# Modules are a tracer

A process parses a library once and checks each module once, as a chain of layers every compile continues from.
That is the [compilation model](../spec/incubator/compilation-model.md)'s, and [epic-sgl-separate-compilation](epic-sgl-separate-compilation.md) is the work that builds it.
The front end parses a library file only where its first line may be a `module` line, which keeps a module directory of programs cheap for the tool and slib.
What a module cannot carry yet is a dotted path, `module a.b` and `use a.b`, which is `unsupported-yet`.

The children are what the tracer cannot carry yet, and the tooling around module directories.
