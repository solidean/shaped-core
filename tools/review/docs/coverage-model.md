# The coverage model

What "every change is accounted for" means precisely, and why a claim from any provenance can be compared with a claim from any other.

## Atoms

A review's obligation is a set of **atoms** against a pinned base.
There are three kinds, and each is addressable:

- an **added line**, keyed by its post-image number under the head path
- a **removed line**, keyed by its pre-image number under the base path
- a **file atom** — a rename, a mode change, a binary file — for what no line can express

Keying removed lines on the base side is what makes a deletion addressable at all.
Under a rename the two sides carry different paths, which is correct rather than a wrinkle: the removed lines really were in the old file.

Both line numbers fall out of one walk of a hunk body.
Two counters seeded from `@@ -a,b +c,d @@`; a context line advances both, a `-` advances and emits on the pre-image, a `+` advances and emits on the post-image.

## Two diffs, and why

Atoms come from a `--unified=0` diff and **only** from there.
A wider context is what a human reads, but a context line is not a change, so letting it into the atom set would inflate every denominator.

Display hunks come from a `--unified=<context>` diff, default 8.
A display hunk strictly contains the zero-context hunks inside it, so:

> a display hunk's claim = its span, intersected with net space

That intersection drops the context lines automatically.
It also means **the default sweep covers the whole obligation by construction** — there is no arithmetic that could be off by one, and gate 1 goes green on the first `ingest`.

## Coalescing does nothing below twice the context

Git already merges hunks whose context windows touch, so at `--unified=8` any two hunks less than 17 lines apart are already one hunk.
The `coalesce_gap` default is therefore 20, above that floor, so the knob has an effect.
Setting it below 17 is not wrong; it just does nothing.

## The pinned base

`init` resolves the merge base once and stores it as a **sha**.
Every diff afterwards is two-dot from that sha.

A three-dot range re-resolves its merge base on every invocation.
Once the integration branch moves, that silently changes net space underneath every change id already handed out.
A review would report different coverage on Tuesday than on Monday, with no commit to blame.
`sync --rebase-base` is the deliberate way to move it.

## The pinned flag set

Every diff runs with text conversion, external diff drivers and autocrlf disabled, and rename detection uncapped.

A `.gitattributes` `diff=` driver or a CRLF round-trip changes the bytes a hunk is made of, and a change id is a hash of those bytes.
Without the pin, the same branch would produce different ids on a different machine.
`-l0` matters for the same reason: rename detection gives up on a large diff by default, so a rename would appear as a delete plus an add depending only on how big the change happened to be.

## Provenance

Three ways a change comes to exist.
All three claim a subset of the same net space, so overlaps are legal and the sum is comparable.

**Net-diff hunks** — the default, claiming as above.

**Commit-local hunks** (`--commits`) — the escape hatch for a net hunk that blends two concerns.
Hunks are taken from one commit's own diff, which is numbered in *that commit's* tree, so they are carried before they claim anything:
added lines forward to head, removed lines back to base, then intersected with net space.

Both directions are one diff each, not a walk over the commits between.
`git diff -U0 C head` already states which regions of C differ from head, and the regions it omits are unchanged — which is the composed line mapping, for free.
A line inside a region a later commit rewrote is **killed** rather than mapped, because that commit owns the line now.
`--stats` reports the shrinkage per commit, so the mapping is never a black box.

A merge counts as everything it brought in, which is this repo's convention for naming a single commit.
Its first-parent diff is what gets carried, so bulking a merge means accepting the branch it merged.

**Bulk** (`--bulk SEL` or `--bulk-commits A..B`, with `--reason WHY`) — one id over a whole set of atoms, with no hunk bodies on disk.
A path selector scopes it by where the change landed; a commit spec scopes it by who made it, which is the shape a formatting sweep or a vendored drop actually has.
Both together intersect, so "the sweep, but only under `libs/`" is one claim rather than two.
A commit's contribution is carried exactly the same way a commit-local hunk is, so a bulk claim is exact rather than approximate.

The reason is mandatory, and that is the point.
It forces the agent to say why it is not reading them, instead of quietly not reading them.

## What carries no obligation

A line added in one commit and deleted in another is not in net space, so nothing has to account for it.
That is what makes the net-diff default safe, and it follows the same principle the review guide states for docs: a claim is judged against the final tree, never against an intermediate state.

## The two gates

**Gate 1 is hard.** Every atom belongs to at least one live change.
An atom with no id is invisible rather than merely unreviewed, which is a different and worse failure.
`ingest --rest` is the verb that closes it.

**Gate 2 is progress.** Every change is discharged, by an ask that names it or by a bulk reason.
It is reported, never enforced: a review is allowed to be unfinished, and rounds are how it finishes.

## Superseding

`sync` recomputes net space against the moved head.
A change whose claim no longer meets net space is marked **superseded** and never deleted, so an entry that discussed it stays readable.
In a `land-changes` review that mark is the evidence: the fix landed, so the hunk it was about is gone.
