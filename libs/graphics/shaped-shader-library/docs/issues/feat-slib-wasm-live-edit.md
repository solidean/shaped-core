---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/impl/sgl_emit_cache.cc
  - file: libs/graphics/shaped-shader-library/docs/structure.md
---

# Live shader editing into a page on wasm

The SGL compiler runs in the page, so a wasm build can compile an edited shader without any other tool.
What is missing is the edit reaching the page: no channel carries a changed `.sgl` from an editor or the file system into a running wasm build, and nothing on wasm keeps a persistent cache.

A dev server that watches the sources and pushes changes over a WebSocket is one shape; `cnet`'s loopback dev server is a candidate host.
It should end in a demonstrated loop: edit a file, see the page change, a broken edit kept out as it is natively.
