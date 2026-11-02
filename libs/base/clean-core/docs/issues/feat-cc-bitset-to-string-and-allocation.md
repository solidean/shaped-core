---
created: 2026-10-17
tags: [container]
parent: epic-cc-container-api-gaps
refs:
  - file: libs/base/clean-core/src/clean-core/container/bitset.hh
  - file: libs/base/clean-core/src/clean-core/container/fixed_bitset.hh
  - symbol: cc::bitset
  - symbol: cc::fixed_bitset
---

# `bitset` printing and allocation interop

Neither bit set has a `to_string`: it would drag `cc::string` into a container header, the way `variant`'s missing one does.
The bit order is already decided — **index 0 leftmost**, since a bit set is an indexed bit array rather than a number.

`cc::bitset` also cannot yet adopt or extract its `cc::allocation<u64>`.
