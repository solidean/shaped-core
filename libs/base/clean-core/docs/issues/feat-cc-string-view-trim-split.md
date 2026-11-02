---
created: 2026-10-17
refs:
  - symbol: cc::string_view
  - file: libs/data/babel-serializer/src/babel-serializer/image/hdr.cc
  - file: libs/data/babel-serializer/src/babel-serializer/image/pfm.cc
---

# `trimmed()` and a whitespace `split` on `cc::string_view`

Wanted: `cc::string_view::trimmed()`, leading and trailing `cc::is_space` removed.
And a whitespace `split` yielding the tokens of a view, either as a range or into a caller's `cc::vector<cc::string_view>`.

Every ASCII-headered format walks the same two operations.
Radiance HDR splits its `-Y h +X w` resolution line and trims both halves of each `KEY=value` line.
PFM walks its five-token header one token at a time.
A future PPM / PGM or `.mtl` reader wants exactly this; none of it is format knowledge.

Today babel's `hdr.cc` hand-rolls `trimmed` and `split_tokens`, and `pfm.cc` hand-rolls `next_token`.
`cc::is_space` already does the classification, so only the iteration is missing.
A line iterator on `cc::string_view` would retire `hdr.cc`'s `next_line` too, though that one also strips a `\r`, which is arguably the caller's business.
