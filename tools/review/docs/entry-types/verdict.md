# verdict

**The big picture, written last.**

*Applies: always, when there is a changeset.*

## What it is for

A review made of findings answers a hundred small questions and never asks the one that matters:
**is this a good thing to have in the repository?**

That judgement is exactly what a detail-oriented pass loses.
Twelve entries about line maps and CSS say nothing about whether the tool should exist, whether it earns its dependencies,
or whether a year from now this will read as a good decision.
The maintainer can reconstruct it, but only by re-reading everything — which is the work the review was supposed to do.

So the last entry says it plainly, and it is allowed to be long.

## What goes in it

- **Is the shape right?** Not "are there bugs" — whether the central abstraction carves the problem where it should.
  Name the one or two decisions that are hardest to undo, and say whether you would make them the same way.
- **Does it fit the repository?** Its philosophy, its layering, the way its other libraries are built.
  A change can be correct and still be foreign.
- **What does it cost?** Dependencies, build time, a surface that has to be maintained, a concept every future reader must learn.
  Weigh that against what it buys.
- **What would you regret in a year?** The part most likely to be rewritten, and whether this change makes that rewrite harder.
- **The overall call**, in one sentence, without hedging.

Write it after the findings, because it should be informed by them — but it is not their sum.
A change with six real bugs can still be the right shape, and a change with none can still be the wrong one.
Saying both is the point.

## What to leave out

- **Restating the findings.** They are above and they have their own entries.
  Reference them by id where the verdict turns on one.
- **Praise that carries no information.** "Great work" tells the next session nothing.
  "This is the right shape, do not churn it" tells it what to leave alone.
- **Hedging.** If the answer is "yes with two conditions", say the two conditions.

## Shape

Numbered `980`, immediately before the generated coverage entry.

The `ask` is the overall call, and its options should be the real ones —
usually some form of *land it*, *land it once these land*, and *this wants a different shape first*.
Mark the one you would pick.

`show: collapsed` on any `changes` block, and there should barely be any: this entry is about the whole, not about hunks.
