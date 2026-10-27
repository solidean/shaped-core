---
created: 2026-10-17
parent: epic-sgl-modules-beyond-a-tracer
blocked-by: [feat-cc-directory-listing]
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/module_dirs.cc
  - symbol: sgl_tool::read_module_dirs
---

# The sgl tool lists a module directory through `<filesystem>`

clean-core has no directory listing yet, so `tools/sgl/module_dirs.cc` walks a module directory with `std::filesystem`.
Once clean-core has one, the file lists through it and drops `<filesystem>`.
