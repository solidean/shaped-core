---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/container/pinned_data.hh
  - symbol: cc::pinned_data::subdata
  - file: libs/data/babel-serializer/src/babel-serializer/geometry/gltf.cc
---

# A fallible `pinned_data::try_subdata`

Wanted: `try_subdata(cc::offset_size) -> cc::optional<pinned_data>`, and the `start_end` / `isize` twins.

Slicing a pin from file-supplied offsets is the normal case for a binary format, and a bad offset is input to reject, not a bug to assert on.
`pinned_data.hh` gives two choices and neither is a validation channel.
`subdata` asserts, and the assert is compiled out in `release-*`, which is the worst possible outcome for attacker-supplied bytes.
`subdata_clamped` silently truncates.

Today every slice site in babel's `gltf.cc` is preceded by a hand-written bounds check producing a `cc::error`.
That covers the GLB chunk walk, the buffer trim, and the bufferView-in-buffer and accessor-in-bufferView validation pass.
It covers `view_of` too, which goes through `span::is_subspan` purely to get a fallible answer.
A `try_subdata` would delete that whole class of boilerplate, there and in every future binary format.
