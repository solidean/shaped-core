---
created: 2026-10-17
refs:
  - file: tools/review/cmd/validate.py
---

# A mistyped change id gets no did-you-mean

`validate` says the id is not in the ledger and stops there.
Naming the nearest live id would make the typo a one-line fix.

Found by running the tool on another repository at scale.
