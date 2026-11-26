---
created: 2026-11-26
refs:
  - test: sv::annotation - a box that would leave the view is pushed back inside its margins
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-26T15:29Z
---

# The annotation test of a box pushed back inside its margins failed once, then passed on a rerun

`dev.py ship`'s gate reported it on 2026-11-26 on the Arc B580: red, then green on the rerun it makes.
The ship carried only known-issue skips and docs, so the change was not near it.
Not yet known: what it checked when it failed, which the gate's rerun overwrote, or how often it fails.
Next: `uv run dev.py test "sv::annotation - a box that would leave the view is pushed back inside its margins" --repeat 100`.
