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
A module's name may be dotted since 2026-11-09, `module a.b`, as one flat name that `use a.b` binds as `b` (CHK-346, CHK-347).
slib exports one as `module:a.b`, into `<sgl_modules/a/b.hh>` and `sgl_modules::a::b`.

The children are what the tracer cannot carry yet, and the tooling around module directories.
