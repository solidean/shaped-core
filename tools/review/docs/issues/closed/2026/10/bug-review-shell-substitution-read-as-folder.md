---
created: 2026-10-17
refs:
  - file: tools/review/lib/annotate/providers.py
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-18T05:58Z
closed: 2026-10-18
resolution: fixed
---

# A bash substitution in a fence reads as a folder reference

`raw:${mod//\//.}` inside a shell fence is matched as a folder reference and fails `validate`.
The folder matcher should not start inside `${`.

Found by running the tool on another repository at scale.
