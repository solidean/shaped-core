---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/lsp/tests/language-server-test.cc
  - test: sgl lsp - an edit of a module that leaves what it declares alone moves the modules using it rather than checking them again
closed: 2026-11-09
resolution: fixed
---

# An LSP relocation test failed once under the full gate

On 2026-11-09 `dev.py ship`'s test gate on an M4 failed this test at `language-server-test.cc:575`: `edited->layer->layer->is_relocated()` was false.
It passed alone and on 20 repeats right after, so it depends on the gate's parallel load or on timing; the cause is not traced.

## Resolution

Two causes, both in `driver/shared_library.cc`, found with a probe in `chain_of` on a failing run.
The flake: a library found again in the library cache was never made the last one under its names.
So once other tests' name lists had cleared `g_last_libraries`, an edit had no previous chain and checked its later modules afresh.
A correctness bug beside it: `chain_of` relocated the previous chain's link whatever its files, so after another edit of `shading` an edit of `noise` moved that other text into place as `shading`.
A found library is now remembered as the last one, and a link relocates only where its files are the module's own.
`sgl lsp - an edit moves the modules after it from the library last used, though another edit was made since` fails on the old code at its run of the document's test.
The binary passed 60 repeats after.
What remains: relocation leans on process-wide caches, so a flood of other libraries between two analyses still falls back to a fresh check, which is correct and slower.
