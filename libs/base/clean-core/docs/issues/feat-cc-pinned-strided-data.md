---
created: 2026-10-17
priority: someday
refs:
  - symbol: cc::strided_span
  - symbol: babel::gltf::accessor_view
---

# A pinned strided view

Wanted: `pinned_data::as_strided<T>(stride)`, or a `cc::pinned_strided_data<T>` pairing a `strided_span` with a pin.

`cc::strided_span` is exactly the right view over an interleaved vertex buffer, but it is non-owning.
Handing one out drops the pin, and with it the guarantee that made the zero-copy read worth doing.

Today `babel::gltf::accessor_view` hand-rolls `pinned_data + stride + count + element_size` and exposes `as_strided<T>()` as a non-owning view tied to its own lifetime.
Low priority: the hand-rolled struct also carries the accessor's `component` / `type` / `normalized`, so it is more expressive than a generic pinned strided span would be.
It is not obvious a second caller wants the generic form.
