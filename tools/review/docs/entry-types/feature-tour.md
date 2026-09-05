# feature-tour

**The feature, taught — the smallest real example of using it, what it produces, and what it replaced.**

*Applies: whenever the change adds a feature a reader could use.
Skip it for a change that is entirely internal — a refactor, a bug fix, a doc pass — where there is nothing new to
teach.*

## What it is for

A review is read by someone who did not design the change and is not going to read 10k lines of it.
The fastest possible explanation of what a branch *is* is one worked example of using it, and a diff never contains
that example — the diff contains the machinery.

So this entry is the tutorial the branch would have if it shipped with one.
It sits early, right after orientation, because everything below it reads better once the reader knows what the
feature looks like in use.

**Distinct from orientation.**
Orientation is the bird's-eye view: what the change does, what it claims, what the reviewer went looking for.
This is ground level, and it is made of examples rather than of description.
A reader should be able to write the feature's "hello world" from this entry alone.

## How many

**One per feature cluster**, and deciding the clusters is the judgement the entry costs.

- One or two entries is the common case.
- Several small features that share one story take one entry between them.
- Four is a lot.
  Past that, the clusters are probably one cluster and the tour is being written as a listing.

A change that adds one thing gets one tour, however large the change is.
Size is not what splits a tour; separate stories are.

## What goes in it

- **The smallest real example**, as committed.
  Not a simplified retelling of one — the file that is actually in the tree, so the reader can go and open it.
- **What it produces.**
  The generated code, the rewritten source, the emitted asset, the rendered output.
  This is the half a reader cannot get from the diff at all, and it is usually the half that explains the feature.
- **What it replaced**, where something did.
  Two lines of the old call site against two lines of the new one says more about a feature than any paragraph.
- **The seams and the refusals.**
  What the feature deliberately does not cover, in one line each.
  A reader who knows where the edges are trusts the middle.

Structure it as a walk rather than as a reference.
A reference is what the cheat sheet is for, and duplicating one here wastes the entry's one advantage: order.

## Produce the examples, never describe them

**Run the generator, run the example, and paste what came back.**

A tour whose code samples the reviewer wrote out by hand is the same class of claim as a `///` quoted from memory,
and it fails the same way — a plausible sample that the tool does not actually emit, in the one entry a reader
trusts most because it looks like evidence.

Where the artifact needs a build the review box cannot do, say so in the entry and quote whatever the branch's own
tests assert about it, which is ground truth that did run somewhere.

Say which parts you produced and how.
"The shaders are the branch's; the C++ is what its generator emitted when I ran it over them; only the package name
is mine" costs one line and tells the reader exactly how far to trust each block.

## What it is not

- **Not a findings entry.**
  A tour that turns into a critique halfway through stops teaching.
  What you noticed while writing it goes in a finding entry, or in the design critique.
- **Not the PR body.**
  The author already wrote what the change does.
  This shows what using it looks like, which the body rarely does.
- **Not exhaustive.**
  Every attribute, every overload, every flag is the cheat sheet's job.
  Pick the path a first user walks.

## The worked case

pr-164 added a preprocessing pass that owns every HLSL binding address, plus the C++ codegen that matches it.
It was reviewed across nineteen entries — the grammar, both parsers, the backends, the docs, the ports — and not one
of them showed a shader written in the new style.

The tour that fixed it was five sections, one per annotation, each carrying the committed shader, the address the
pass writes into it, and the C++ the generator emits.
Every C++ block in it came from running the branch's own generator over the branch's own test shaders, so nothing in
it was a reviewer's reconstruction.
It landed as `012`, before the glossary, and the maintainer's note was that it showed what the PR was really about
faster than anything else in the review.
