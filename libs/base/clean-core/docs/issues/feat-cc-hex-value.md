---
created: 2026-10-17
refs:
  - symbol: cc::is_hex_digit
  - file: libs/base/clean-core/src/clean-core/string/char_predicates.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/uri_resolver.cc
---

# `cc::hex_value(char)`, the value a hex digit names

Wanted: `cc::hex_value(char)` beside `cc::is_hex_digit`, returning 0..15 for a digit and something the caller can reject for anything else.

Classifying a hex digit and decoding one are the same operation split in two, and clean-core ships only the first half.
So every caller that tests with `cc::is_hex_digit` hand-rolls the other half beside it.
Percent-decoding a uri raised it; a `\xNN` escape in a text format, a hex colour literal and a hex dump reader all want the same four lines.

Today sv's `uri_resolver.cc` hand-rolls `hex_value` next to its `cc::is_hex_digit` call.
