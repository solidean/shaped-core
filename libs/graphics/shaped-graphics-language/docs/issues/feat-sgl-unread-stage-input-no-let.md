---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T18:51Z
---

# A stage input nothing reads is still converted into a let

dispatch's `size_dispatch` writes `const int3 id = int3(id_in);`, which nothing reads, and so does every entry point that takes `@vertex_index i` and reads none of it.
L2 does not reach the conversion, which the emitter writes from the signature, so the conversion could be written only for an input the core tree reads.
