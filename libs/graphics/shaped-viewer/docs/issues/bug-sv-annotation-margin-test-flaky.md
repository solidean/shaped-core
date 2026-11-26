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

## Log

**2026-11-26: the test cannot fail by itself.**
It calls `sv::impl::place_annotation` on constants and compares the result exactly: no GPU, no font, no clock, no thread.
A hundred repeats on the Arc pass.
So the red the gate saw is likely another test's, reported under this name, as bug-nexus-blocking-get-misattributes-checks describes; not confirmed, since the gate's rerun overwrote the log.
Next time the gate files it, keep the failing run's log (`build/<preset>/crashes/*-failed`) before anything reruns.
