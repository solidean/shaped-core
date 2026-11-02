---
created: 2026-10-17
tags: [container]
parent: epic-cc-container-api-gaps
refs:
  - file: libs/base/clean-core/src/clean-core/container/tuple.hh
  - file: libs/base/clean-core/src/clean-core/container/variant.hh
  - symbol: cc::tuple
  - symbol: cc::variant
---

# Grow `tuple` and `variant`

The first version deliberately left out converting construction from another `tuple<Us...>`, `tuple_cat`, `variant`'s `operator<=>` and multi-variant visitation.

A `to_string` hidden friend for `variant` is missing too.
It would drag `to_debug_string.hh` into a container header, so a `variant` currently debug-prints as a raw byte dump.
