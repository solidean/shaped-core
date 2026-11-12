---
created: 2026-11-11
refs:
  - file: libs/base/clean-core/src/clean-core/record/event_view.hh
---

# A recorded pointer value is 4 bytes on wasm32, but field_as_u64 reads 8

recording-formats.md says a payload slot holding a pointer is always eight bytes, whatever the writer's pointer width.
It lists desc_ref, cstring and pinned_bytes; a `type_code::pointer` value is not covered.
Such a value is stored as `void const*`, so its field is four bytes on wasm32, while `field_as_u64` still reads eight.
On wasm32 that reads past the field, or fails the bounds check.

Found while writing the Python `.ccrec` reader, which mirrors the C++ and reads eight bytes; not reproduced on a wasm32 build yet.
The fix is likely the rule's: widen a pointer value to eight bytes on the way in, as the other address-holding slots do, with a format version bump and the reader following.
