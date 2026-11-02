---
created: 2026-10-17
tags: [rec]
refs:
  - file: libs/base/clean-core/src/clean-core/record/recording.hh
  - file: libs/base/clean-core/docs/systems/recording.md
---

# A query index over a recording

The queries are linear scans today, which is fine to somewhere around a million events and no test comes close.
An index would be a hash from descriptor to event offsets plus a sorted timestamp array, built on first query and cached on the recording.
