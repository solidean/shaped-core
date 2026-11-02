---
created: 2026-10-17
refs:
  - file: tools/review/cmd/round.py
---

# `round --no-wait` hands back every discharge list

On a large review its text was 27 KB, mostly `discharges:` ids the agent wrote itself and does not need read back.
It should print a count, and the ids only with a flag.

Found by running the tool on another repository at scale.
