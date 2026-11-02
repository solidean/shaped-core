---
created: 2026-10-17
refs:
  - symbol: sv::texture_manager
  - symbol: sg::bindless_array
---

# A texture the LRU evicts stays resident until the bindless table fills

Its element holds the view and with it the texture, as a replaced one did before `sg::bindless_array` gained `transient.release`.
Eviction calling release, through a hook from `texture_manager` to the table's owner, is the fix.
