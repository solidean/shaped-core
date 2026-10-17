---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/threading.md
  - symbol: sg::thread_model
---

# Should `sg::thread_model` say more?

`sg::thread_model` says which thread may make the bound calls.
It can grow as a caller needs: whether concurrent command-list recording is allowed, for example, or per-queue guarantees.
Nothing asks for either yet.
