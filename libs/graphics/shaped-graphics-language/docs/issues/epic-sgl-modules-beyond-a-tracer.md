---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/impl/front_end.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/compilation-model.md
---

# Modules are a tracer

Every compile parses the library and checks the modules the program reaches, in one pass with it, and keeps nothing between compiles.
Interfaces, separate compilation and a parsed library shared across compiles are the [compilation model](../spec/incubator/compilation-model.md)'s.
The front end parses a library file only where its first line may be a `module` line, which keeps a module directory of programs cheap for the tool and slib.
The language server still parses every file of its module directories.

The children are what the tracer cannot carry yet, and the tooling around module directories.
