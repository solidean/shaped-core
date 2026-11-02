---
created: 2026-10-17
parent: epic-sg-transfer-gaps
refs:
  - file: libs/graphics/shaped-graphics/tests/transfer/download-async-test.cc
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_download_async.hh
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc
---

# An async download could cancel as soon as nobody can observe it

A readback's job owns its source, so dropping every handle to the resource never cancels a download the caller still holds a future for.
That is settled semantics, and [download-async-test.cc](../../tests/transfer/download-async-test.cc) pins it.

Dropping the *future* does cancel, and today that is noticed when the actor next picks the job up.
Finer would be to notice it per window and stop mid-copy, releasing the source with it.
Pure quality of implementation: the bytes are unobservable either way, and what it buys is releasing a large source sooner.
