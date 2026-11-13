---
created: 2026-11-13
tags: [routine, async]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/routine/render_routine.cc
  - test: sg - a routine cancelled at its own generation is not restarted
  - test: sg - a routine whose init ends in an error runs once, and only a reload runs it again
---

# Routine tests leave async work running in a full run, since the reworked routine verdicts

Since the merge that brought 750eb160a's routine verdicts (a cancelled phase now fails, at the generation it ran at), `shaped-graphics-test` fails on every full run here.
One or two routine tests end with `left async work running — 1 async item(s) still carry its context`, on dx12 and vulkan alike.
Seen so far, a different pick each run:

- `sg - a routine cancelled at its own generation is not restarted`
- `sg - a routine whose init ends in an error runs once, and only a reload runs it again`
- `sg - a routine failing in init_once stays failed, and a reload runs init_once again`
- `sg - an init_once failing after a reload is the old generation's verdict, not the new one's`

Measured on an Arc B580, relwithdebinfo, 2026-11-13: the first iteration of every `dev.py test shaped-graphics-test --repeat` failed, 4 runs of 4.
The first of them alone, `--repeat 30`, never failed, so it needs the full mix.
It is not the drivers' new `finish_each` step: with it switched off the same tests fail the same way.

The hypothesis, not traced: a phase the reload drops before it is collected still runs, carrying the test's context.
That is the case the rework now calls a failure; [bug-sg-cancelled-phase-crashes-ticking-frame](bug-sg-cancelled-phase-crashes-ticking-frame.md) suspects the same inheritance of the caller's context.
