---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/types.hh
---

# Exportable memory and shared fences

OIDN's GPU devices run on their own API (CUDA, HIP, SYCL, Metal) and share memory with ours through an OS handle.
That wants an "exportable" usage on buffer and texture creation, a way to read the handle, and a fence shared both ways.
Not needed for OIDN on the CPU, which goes through the existing download and upload.
Built with the OIDN GPU member.
