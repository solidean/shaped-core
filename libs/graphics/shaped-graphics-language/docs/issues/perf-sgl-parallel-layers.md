---
created: 2026-11-09
priority: someday
parent: epic-sgl-separate-compilation
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/shared_library.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
---

# A library's modules are checked one after another, those with no dependency between them too

`check::library_chain` orders a library's modules into one chain, and each link is checked over the one before, on one thread.
Two modules with no dependency between them could be checked at once, each over the links it needs, and joined after.
With positional ids a join is a relocation: the second module's part moved behind the first's, which `relocate_link` already does for a chain of the same modules.

Measured on 2026-11-09, release: shaped-viewer's 10 modules check in about 25 ms, once per process.
An edit checks only its module and moves the ones after it, so parallel layers would cut the first check of a library in a process and little else.

**When to take it up:** when a process's first compile behind a library is dominated by checking it, many modules on a cold start, and the host keeps no artifacts that would load them instead.
