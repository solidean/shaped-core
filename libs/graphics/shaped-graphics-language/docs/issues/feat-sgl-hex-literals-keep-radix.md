---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
---

# A hex literal is written in decimal

`modules/slug.sgl` writes `0xffff'fffd`, `0x2e74` and `0x0101`, and the targets read `4294967293u`, `11892u` and `257u`.
A bit pattern reads as one only in hex, so the literal's radix could reach the emitter, without its `'` separators.
