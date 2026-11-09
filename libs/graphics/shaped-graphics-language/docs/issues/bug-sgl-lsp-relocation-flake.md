---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/lsp/tests/language-server-test.cc
  - test: sgl lsp - an edit of a module that leaves what it declares alone moves the modules using it rather than checking them again
---

# An LSP relocation test failed once under the full gate

On 2026-11-09 `dev.py ship`'s test gate on an M4 failed this test at `language-server-test.cc:575`: `edited->layer->layer->is_relocated()` was false.
It passed alone and on 20 repeats right after, so it depends on the gate's parallel load or on timing; the cause is not traced.
