# design-critique

**The alternatives, priced — whether this is the right solution, not whether it is a correct one.**

*Applies: whenever the change makes a design choice that will outlive it — a new mechanism, a new API, a new
dependency, a new place where something lives.
Skip it for a change with no choice in it: a bug fix, a doc pass, a mechanical port.*

## What it is for

A correctness review answers "does this work".
It never answers "should this exist in this shape", and that is the more expensive question — a bug is fixed in an
hour, and a mechanism carved at the wrong joint outlives several rewrites of its body.

So this entry names what else could have been built, prices each one, and then says where the chosen point sits.
It is the entry that lets a maintainer *check a judgement* rather than accept it.

**It goes immediately after the feature tour, at `014`.**
The tour gives the reader a feel for the feature; the critique then anchors their top-level understanding of whether
it should be shaped this way.
Both land before any finding, because a finding read without either is a detail about something the reader has not
yet been given a reason to care about.

**Distinct from verdict.**
The verdict is the reviewer's own assessment, written last, and it is allowed to be a judgement.
This is the working behind it.
When a review has both, the verdict should point here rather than asserting "this is the right answer" on its own
authority — that sentence is exactly the one a reader cannot check.

## What goes in it

- **The alternatives, including the boring ones.**
  Doing nothing structural is always one of them, and it is often the most informative: on a large branch it reveals
  which slice carries which part of the value, and therefore what could be kept if the rest were reverted.
- **Pro, con and verdict for each**, short.
  The maintainer is checking your reasoning, not reading an essay.
- **Which ones the branch already recorded**, marked and cited.
  For those the question is whether the recorded reasoning *holds*, never whether the option was seen.
  Raising a recorded alternative as a discovery says the reviewer did not read the docs in the diff they are
  reviewing — see [the guide](../../../../docs/guides/reviewing-prs.md#a-finding-the-diff-already-documents-is-not-a-finding).
- **The chosen point against what the repo values**, one short paragraph each: API elegance, efficiency, KISS,
  moving forward, and keeping designs deliberately open to refine later.
  Name the dimension where the change is weakest and say whether it was traded knowingly.
- **Where it is a bet rather than settled.**
  A design resting on a discipline — "keep these two in step" — is a bet.
  A design resting on a mechanism is not.
  Saying which is which is most of this entry's value.

## The goal is pareto-optimal, not perfect

Perfectionism is inefficient, and a review that pushes toward a completeness nobody asked for costs more than it
returns.

What is being checked is that the solution is good, that the shape is right, and that the avenues for later growth
are open.
Not that nothing was left undone.
An acknowledged gap is frequently deliberate, and shipping something incomplete on purpose is a legitimate
engineering decision — see
[A gap the author names](../../../../docs/guides/reviewing-prs.md#a-gap-the-author-names-is-where-to-look-and-often-where-to-defer).

So an alternative that is *better on one axis and worse on another* is not a finding.
An alternative that dominates — better on some axis and worse on none — is.

## The most valuable thing it produces

**An alternative that is not closed in writing anywhere.**

An option ruled out in someone's head is one a future session re-proposes, re-argues and sometimes re-implements.
Finding one costs a paragraph and saves a rewrite, and the recommendation is usually to write it into the design doc
rather than to change any code.

This is the entry's highest-yield output, and it is worth going looking for deliberately: for each alternative, ask
not only "was it considered" but "would a reader find out why it was rejected".

## What it is not

- **Not a redesign.**
  Where a redesign is already the author's planned next step, the job is to keep this change from cementing the old
  shape, not to specify the new one.
- **Not a re-litigation.**
  An alternative the branch recorded, argued and rejected is closed unless the argument is actually wrong.
- **Not scope creep.**
  "This could also do X" is a feature request.
  This entry is about the shape of what is here.

## The worked case

pr-164 moved every HLSL binding address from the shader author to a preprocessing pass, and implemented the same
grammar twice — once in C++ for the runtime rewriter, once in Python for the build-time generator.

The critique named six alternatives.
Three the branch had already recorded, and citing them was the point: DXC's `-fvk-*-shift` family, a macro prelude,
and a C++ host tool in place of the second parser.
Three it had not.
Doing nothing structural, which revealed that the 111-line conflict check delivers a distinct slice of the value and
should survive any revert of the rest.
Rewriting at build time only, which would collapse the two grammars into one and is refuted by hot reload reading the
author's file from disk.
Generating from reflection, which is refuted by the same DXC behaviour that motivates the whole branch.

The last two were the yield: both are options a future session would re-propose, neither was written down anywhere,
and the recommendation was two paragraphs in the design doc rather than any code change.
