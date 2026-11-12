---
created: 2026-11-09
refs:
  - file: libs/base/clean-core/src/clean-core/streams/file_stream.hh
---

# Opening a file past Windows' 260-character path limit fails

What is wrong or wanted, and what is already known.

`cc::file_read_stream_adapter::open` fails on a path longer than `MAX_PATH`, which `std::filesystem` iterates past without complaint.
A path built as `root / ".." / ".." / ".."` reaches it in the shader-rosetta companion's deep bgfx tree, around 250 characters.
The slib sweeps normalize their root, which is right on its own terms and keeps them under it; a longer tree would still fail.
The fix is a `\?\` prefix on an absolute path in the Windows open, or the long-path-aware manifest.
