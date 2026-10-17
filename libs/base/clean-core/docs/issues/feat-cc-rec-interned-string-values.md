---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/src/clean-core/record/value.hh
  - file: libs/base/clean-core/src/clean-core/string/interned_string.hh
  - symbol: cc::interned_string
  - symbol: CC_RECORD_NAMED
---

# An interned tier for recorded text

The static tier landed: a literal VALUE is stored as its address (`type_code::cstring`), and only a runtime string is copied.
`cc::interned_string` has landed since too, but recording does not use it yet.

What is left is recording an `interned_string`, whose 8-byte handle would give a runtime string the same cost as a literal with no lifetime rule attached.
It is also what would let `CC_RECORD_NAMED` take a runtime string value, since the handle is fixed-size.
Its identity is process-local, so a serialized recording has to resolve the handle to its bytes on the way out.
