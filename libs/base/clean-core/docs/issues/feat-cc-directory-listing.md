---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/module_dirs.cc
---

# A directory listing in clean-core

clean-core has no way to list a directory's entries.
The sgl tool lists its module directories through `std::filesystem` meanwhile, `debt-sgl-module-dirs-std-filesystem`, which goes once this lands.
A consumer checking a folder of assets takes its files from the shell's glob for the same reason.
