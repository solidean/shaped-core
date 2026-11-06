---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/impl/sgl_emit_cache.cc
  - file: libs/graphics/shaped-shader-library/docs/structure.md
closed: 2026-11-06
resolution: fixed
---

# Live shader editing into a page on wasm

The SGL compiler runs in the page, so a wasm build can compile an edited shader without any other tool.
What is missing is the edit reaching the page: no channel carries a changed `.sgl` from an editor or the file system into a running wasm build, and nothing on wasm keeps a persistent cache.

A dev server that watches the sources and pushes changes over a WebSocket is one shape; `cnet`'s loopback dev server is a candidate host.
It should end in a demonstrated loop: edit a file, see the page change, a broken edit kept out as it is natively.

## Done

The channel is the page itself, which needs no server: sgl-cube's page holds `cube.sgl` in an editor beside the canvas.
An edit goes through two exported functions into a `memory_filesystem` mounted over the package's embedded copy, and an unthreaded poll each frame builds it.
The pipeline swaps once the new stages build; a broken edit keeps the last one drawing, and its error is shown under the editor.
Where the browser lets a page read a file (Chrome and Edge), the page follows a `cube.sgl` on disk instead, so a save in an editor reaches it.
`?selftest` makes one broken edit and one good one, and passed in headless Edge on WebGPU (2026-11-06).
slib's test of the overlay is "a memory filesystem mounted over a package's embedded files is how a page edits them".

Not done here, and not needed for the loop: a persistent cache on wasm, which the SGL front end's speed in the page has not asked for yet.
