---
created: 2026-11-03
refs:
  - file: libs/base/typed-geometry/tests/scalar/bigint-test.cc
---

# bigint-test's size assertion fails on wasm32

What is wrong or wanted, and what is already known.

Building the `emscripten-webgpu-relwithdebinfo` preset (2026-11-03), `bigint-test.cc:11` fails `static_assert(sizeof(tg::bigint) == 8)`.
A pointer is 4 bytes on wasm32, so a `bigint` holding one is smaller there; the assertion states a 64-bit layout as if it held everywhere.
It stops `dev.py test` on that preset from building at all, though each other test binary still builds and runs alone.
