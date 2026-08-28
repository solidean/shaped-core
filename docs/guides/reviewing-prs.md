# Reviewing PRs

What we look for in a shaped-core review, and how we weigh the calls that have no right answer.
The mechanics — fetching the branch, accounting for every change, the entry structure — belong to the `reviewing-a-pr` skill and to [the review tool](../../tools/review/readme.md);
this is the taste behind them.

**This document is alive.**
It is written from actual reviews, and every entry that reads like a rule earned that status by being said out loud in one.
An entry with no example under it is a hypothesis, not a rule.
Reviews here happen in chat, point by point, precisely so the reviewer's answers can land back in this file.

## Who the review is for

The reviewer is in contact with the author, and a review speaks for both of them.
So the artifact is **a todo list for the author's next agent session**, not a politely framed request.

- **Write instructions, not suggestions.** "Delete the copy constructor" beats "you might consider making this non-copyable".
- **No softening, no hedging, no thanks-for-the-PR preamble.** The author expects this register and reads it faster.
- **Praise only where it carries information** — "this is the right shape, keep it" tells the next session not to churn it.
  "Great work overall" tells it nothing.
- The review lands as **one comment on the PR**, so it has to stand alone: someone reading only that comment must be able to act on every point.

## What is not a review finding

Cut these before writing.
They cost attention and return nothing.

- **"Does it build" and "do the tests pass."** CI runs ~14 configurations and `dev.py check --fix` runs four presets locally in full.
  Build the branch when you need to *verify a specific claim* — that two layouts really do hash differently, that a symbol really is gone — and then say what you learned, not that it compiled.
- **PR size and splitting.** PRs are ephemeral bookkeeping; only the git repo counts.
  Size matters only as "can this be reviewed", and with agent help that bar is high.
- **Compatibility, migration, deprecation.** The repo is beta through 2026 and likely most of 2027.
  Breaking things is cheap; the wrong shape is expensive.
- **Public API docs written in the intended tense.** See "Docs are for users first" below.
- **Restating the PR body.** The author wrote it.

## Ranked by what it costs to get wrong

### 1. API shape

Equal-first with correctness, and the thing most likely to be *permanent*.
A bug gets fixed in an hour; a type that carves the problem at the wrong joint outlives several rewrites of its body.

- **Is the API hard to hold wrong?** Copyability, move semantics, RAII, whether the mistake is even expressible.
  A value type that is silently copyable when copying duplicates authoritative state is a **bug**, not a nit.
- **Are the lifetimes in the type?** A bare `u32` whose validity is scoped to an epoch is a raw pointer with extra steps.
  Prefer a typed handle — `enum class : u32` when it needs no methods, a refcounted class when it owns a slot.
- **Is the guard on the right class?** A lock that protects a lifecycle belongs on whatever owns that lifecycle, not on one participant in it.
- **Does the abstraction pay for itself?** A "manager" that fixes a layout and hides what its consumer needs is the anti-pattern.
  A small helper over one thing the caller still owns is the pattern.
- **Which library does this belong in?** Dependency direction is a hard rule; "could live lower" is the more common finding.

Report API shape **in symbols**: signatures, the actual type names, and a few lines of call-site code.
Prose about an API is much harder to judge than the API.

### 2. Correctness the type system does not catch

- **Silent wrongness over loud failure.** A misuse that asserts is a footnote; a misuse that renders the wrong thing, or corrupts a table, is the finding.
- **Load-dependent failure is worse than deterministic failure.** A stale index that works until the table fills ships, then breaks in production under load.
- **Lifetime and aliasing.** Who keeps what alive, and can an address be reused while something still keys on it.
- **Ordering and epoch discipline.** GPU-side especially: what is still in flight when this mutates.
- **Assert vs `cc::result`** — see below.
  Picking the wrong one is a real finding in both directions.

### 3. Docs that have gone out of true

This is the **highest-value-per-effort** category, because it is exactly what the GitHub review UI cannot show.
A doc line is only wrong relative to the *final tree*, and the diff view shows it against the author's intent.
Hunt it deliberately.

**A doc claiming something the branch does not do is a defect, not a nit.**

Where they rot, in practice:

- **Leftovers from an earlier state of the branch.** A branch that added a thing and then removed it leaves the cheat-sheet line behind, still describing the removed thing.
  The diff shows that line as an *addition*, so it reads as intentional.
  Always diff the docs against the final tree, never against the PR body.
- **Stale identifiers after a rename** — a parameter renamed in the signature and still named by its old name in the `///` above it.
- **Structure and status tables** (`[done]` / `[in progress]` / `[planned]`) the change should have moved.
- **Cheat sheets** whose field lists no longer match the struct.
- **A count in prose, once the set grows.** "Two backends", "both codecs", "`frame` framing only" — each was true when written and false the moment a third member landed.
  None of them shows up in the diff, because the change that falsified them never touched the line.
  So when a change adds the Nth member of a set, grep the old cardinality across the subsystem before reading anything else.
  pr-151 is the worked case: adding deflate beside zstd and lz4 left four such sentences wrong, three of them in files the branch itself edited.

Verify each doc claim against the branch by looking up the symbol, not by reading the sentence.

### 4. Prose style and nits

`dev.py lint shaped` already gates the one-semantic-point-per-line rule, so a review rarely needs to.
Drive-by nits are welcome — collect them into a single trailing point so they never crowd out the above.

## Settled calls

Each of these was decided in a review; do not re-litigate them, and apply them as rules.

### Docs are for users first, implementors second

A public API doc may state the **intent** in the present tense even where one backend has not caught up.
"Every backend's `bind_group` asserts the slot matches" is correct writing when one of the backends is a non-recording stub.
The reader is a user of the API, the implication is "every usable backend", and whoever implements the stub has to match the sentence.
Do not file this.

The inverse still holds: a doc claiming a capability *no* backend has is wrong.

### Picking the wrong error mechanism is a real finding

[error-handling.md](../error-handling.md) is the authority, and it splits three ways rather than two.
**Exceptions have a genuine place here** — this is not a "no exceptions ever" codebase, and never review as if it were.

- `CC_ASSERT` is for **contract violations only** — the programmer used the API wrong.
  Assertions being off in `release-*`, with UB past the failed contract, is accepted: we work in `relwithdebinfo` most of the time and test heavily, so violations surface.
  Do not file "this is UB in release" for a plain contract assert.
- `cc::result` / `cc::optional` is for **expected failures the immediate caller can act on**, and anywhere throwing is unwanted.
- **Exceptions are for infrequent failures that must bubble past frames that cannot help, to a handler that exists further up** — a device reset, an allocation the subsystem above can recover from.
  A failure being recoverable but *not locally* is exactly what makes it an exception rather than an assert.

Two things to actually look for:

- **An assert on anything from outside the program** — a file, a shader, a device, an allocation.
  That is the clearest defect in this area, and it is common where a check was written backend-local and never lifted.
- **A fallible operation offering only one of the two surfaces.**
  The house pattern is a `try_*` fallible core plus a thin throwing façade, so a caller can go exception-free without ceremony everywhere else.
  A new `create_*` that only throws, or only returns a `result`, is worth naming.

### A 64-bit hash is not an identity

Using a `u64` hash *as* the identity — storing only the hash and skipping the comparison — needs extraordinary evidence.
The house sizes are `cc::hash128` for identity in non-adversarial settings, and `cc::hash256` where cryptographic guarantees are wanted.

A hash map keyed on the real value is the default, and it is not a cost worth avoiding.
`cc::map` already stores each node's finalized hash and short-circuits chain compares on it, so keying on the value costs one `operator==` per hash match.
When a type has a `hash()` hidden friend and no `operator==`, adding the operator over exactly the fields the hash folds is usually the whole fix.
Those two staying in agreement is then the invariant to state.

### Out-of-order execution invalidates every collapsed maximum

When a change lets work complete out of the order it was submitted in, go looking for every place that folds a set of values into a single number and waits on it.
A shared counter signaled to "the highest value finished so far" is correct only while completion order matches submission order.
That premise usually lives in a comment nobody rechecks when the ordering changes.

**The fix is to split the signal, never to serialize it.**
A watermark — signal only the contiguous prefix that has actually finished — restores correctness and reintroduces the head-of-line blocking the change existed to remove, one queue further down.
Splitting means one timeline per *ordering family*, so values stay comparable within a family and unrelated work never speaks for anything but itself.

The worked example is sg's transfer completion.
`ctx.stream` made the copy actors select jobs out of order, and both actors still signaled one per-system fence to the highest value each window finished.
A stream to one buffer finishing therefore reported an older upload to a different buffer complete, and a reader stopped waiting for a copy that had not run.
`dx12_completion_group` is the split: one fence per resource per direction, pooled and recycled.

### A named owner is a claim to verify, not a fact to accept

When a change introduces one owner for an invariant, check that **every participant actually routes through it**.
A manager holding the lock, a single object taking the snapshot, one type minting every id — each is a claim about call sites, not about types.
The header says who owns it; only the call sites say whether anyone bypassed them.
A participant handed a *copy* of the owned thing rather than a reference to the owner is the shape to look for, because it type-checks and reads as sharing.

The worked example is sv's bindless tables.
`gpu_resource_manager` documents itself as the sole owner of the arrays, the epoch tick and the access-declaration list, and its `_record` is what makes a dispatch's declaration complete.
Four resource managers were each handed their own `sg::bindless_array` over the same binding and pinned through it.
`_record` was therefore never reached for any buffer, and every trace declared its bindless buffer table as empty.
The bug was invisible in the diff and invisible in the docs; it was only visible by listing the callers of `_record`.

### A derived artifact's cache key must cover everything that varies it

Whenever a change caches something *generated* — a shader, a layout, a packed buffer — enumerate every input to the generator and check each one is in the key.
The inputs that get missed are the ones passed as options rather than as data: a config struct, an entry-point name, an include path.
Two callers generating from the same data under different options then collide on one entry, and the second one silently gets the first one's artifact.

The worked example is sv's `generate_material_shader`.
Its key was `resolved_material::permutation_key`, which covers the resolution's shape and nothing about how it was spelled.
The emitted text also depends on the bindless table counts, the entry point and both include paths.
A `gpu_resource_manager` configured with non-default budgets generated a shader declaring the *default* array sizes against a group layout of a different size.

### Adding a member behind a seam means re-reading the seam's callers

A vtable, a trait, an enum with a switch — the written contract covers the members that exist, and a caller is free to lean on a property all of them happen to share.
The Nth member then arrives satisfying the written contract and not the unwritten one.
Nothing in the diff shows it, because the caller's line did not change.

So when a change plugs a new implementation into an existing seam, list that seam's callers and read each for an assumption only the old members satisfied.
The tell is a comment at the call site explaining why the call is safe.
That sentence is the unwritten contract, and it is exactly what nobody rechecks when a member is added.

The worked example is clean-core's `declared_size`.
It reports the uncompressed size a compressed blob declares, and for zstd and lz4 that number sits in the frame header.
`decompressing_read_stream_adapter::impl_refill` therefore probed it from the first window of bytes the inner stream had buffered.
The comment above that probe said so: "the declared size comes off the frame header".
gzip declares its size in the *trailer*, so on a stream not fully buffered up front the probe read four bytes of compressed payload as a length, and `read_all` reserved on it — up to 4 GB.
The seam's own contract was never violated, and both halves are correct read on their own.

The generalization the maintainer drew is worth keeping beside it: **trailer metadata is a design smell for anything that cannot assume bounded frames.**
"Seek to the end and read it properly" works only where the frame ends where the stream ends, which a blob embedded in a container never does.
So the answer was that deflate has no streaming size hint at all, rather than a cleverer way to find one.

### "No callers in the repo" is not evidence of dead code

It is evidence only for something the repo alone can use.
A symbol in a library's exported `FILE_SET` — a backend header included — is reachable by consumers this tree does not contain.
An unused-looking member there wants its *correctness* checked rather than its existence questioned.
Say which of the two you are claiming, because they get answered differently.

### Drive-by cleanups are welcome where the PR already is

The libraries are in flux, so cleanups get postponed by prioritization rather than by policy — fringe and niche APIs carry debt on purpose.
That is a schedule, not ossification.

So **asking to down-pay debt in code the PR is already touching is a good finding**, and worth making concrete: name the call sites and the replacement.
Asking to clean up code the PR does not touch is not.

### Portability floor: reject the non-portable thing on the dev box

Where a feature exists on the backend we develop against but not on a backend we intend to ship, we **fail everywhere, loudly, on the dev box** rather than let it surface later on the weaker target.
[concepts/views.md](../../libs/graphics/shaped-graphics/docs/concepts/views.md) is the worked precedent.
Storage-buffer offsets take WebGPU's 256-byte floor as a hardcoded portable rule, which "fails loudly on a dx12 dev box rather than surfacing later on WebGPU".
It carries a documented escape hatch for callers who knowingly target only the looser backends.

Two corollaries a review should check:

- **Keep the code paths.** Rejecting the feature at the API door is not the same as deleting the plumbing; the point is that conditional or full support later needs no redesign.
- **Say why, and where.** The rejection must point at the portability reason in a doc, not just assert "not supported yet".

**A known issue recorded in a TODO is not an accepted failure mode**, and finding it already written down does not close the question.
What the entry settles is that the *capability* is missing; what it usually leaves open is what happens when someone hits it.
Silent wrong output is the wrong answer whether or not it was foreseen.
So read a TODO entry for the failure shape, and file the assert when the recorded behavior degrades quietly.

The worked example is sv's per-permutation samplers.
`pathtrace_routine::collect_samplers` lets the first permutation to claim `sv_sampler_0` decide that register for the whole pipeline.
The viewer's TODO records it honestly: two materials sampling with different filters silently share the first one's sampler.
The missing capability is a per-hit-group local root signature, and that genuinely waits for sg.
Asserting on a *conflicting* state for an already-claimed register does not, costs nothing, and turns an unexplainable image into a message.

### A change that touches an example is reviewed by looking at the example

**Put the example's source and every image it produces in front of the maintainer, as an entry.**
The [example-showcase](../../tools/review/docs/entry-types/example-showcase.md) type is the shape; this is why it is not optional.

An example is a thing someone will read and a reference image is a thing someone will look at, and the diff shows neither.
Approving a hunk in an example is approving a demonstration nobody demonstrated, and `Bin 0 -> 31695 bytes` is a picture nobody saw.
Small examples go in whole, larger ones as a summary plus the code that carries the point, and every committed image goes in inline.

**This applies to every changeset touching an example, a capture sidecar or a reference image.**
The only exemption is a touch that could not change what the example shows — a rename, a formatting sweep, a bulk include fix.

**Open the image.
Do not infer it from the code.**
This is where the findings are, because a run that neither crashes nor asserts routinely shows nothing worth looking at.
That is the argument [examples.md](examples.md) itself makes for capturing while authoring.
A review that reads the hunks and not the picture inherits exactly that blind spot.

pr-150 is the worked case, on the branch that added headless capture.
`vdoc/cube-viewer`'s committed reference image showed its imgui panel about 110 pixels wide, every sentence wrapping to two or three words a line and one breaking mid-word.
The cause was one missing `SetNextWindowSize` beside a `SetNextWindowPos`, next to a sibling example that has both.
It had been invisible for as long as the example existed, because both examples restore a saved layout.
`ImGuiCond_FirstUseEver` then means the developer's own window — dragged wide once, months ago — is what they had seen ever since.
Nothing in the diff could have shown it, and the image showed it immediately.
The same pass found an em-dash the imgui font cannot draw, rendering as `?`, and `hello-cube`'s six declared face colours reduced to one legible face by an overhead light three stops into clipping.

**Findings from an image live in that entry**, beside the picture that is their evidence, rather than in a finding entry of their own.
And **offer the deferral**: an imperfect example is not a reason to hold a change.

### A gap the author names is where to look, and often where to defer

**The layer a PR body flags as unexercised is where the defects are**, and it is worth going there first.
The tool review (pr-147) is the worked case: the body said the local page had never been driven by a human, and every UI defect was there —
a help overlay that could not be dismissed, a crash on every fenced code block, a save loop that scrolled the reader to the top, two servers sharing a port.
Meanwhile the coverage engine the body argued for at length held up under adversarial testing.

**But an acknowledged gap is frequently deliberate, and closing it is not automatically the right call.**
Perfectionism kills velocity, and shipping something incomplete *on purpose* is a legitimate engineering decision.
The trade is genuinely hard, and it is the author's to make rather than the reviewer's.

So the finding is never "this is untested".
It is **what is actually broken there**, found by going and looking, and then an ask whose options include **document and defer** —
a line in a "Not yet" section, a TODO with the shape of the fix, an issue — alongside fixing it now.

That option is what lets a maintainer ship incompletely *with intent* rather than by omission, and offering it costs nothing when the answer is "fix it".
A review that only ever offers "fix it" pushes toward a completeness nobody asked for, and the deferral then happens silently instead.

### A finding the diff already documents is not a finding

**Search the branch's own docs for your finding before you raise it.**
`docs/TODO.md`, a "Not yet" section, the doc comment on the function itself — a good author writes the gap down, and the reviewer who missed that hands it back as a discovery.

pr-146 is the worked case, twice over.
"An attribute-less `material_type` generates a shader that does not compile" was raised as a blocking correctness bug.
The branch's `docs/TODO.md` already carried the same diagnosis, the same prescribed fix, and the author's judgement that nothing in the tree reaches it.
"`compile_source` drops the dependency list, so a generated permutation does not hot-reload" was raised the same way.
`shader_library.hh` says it three lines above the sentence the review asked to reword.

Both readings damage the review twice.
They say the reviewer did not read the docs *in the diff they are reviewing*, which is the one place a reader assumes they looked.
And they silently overrule a recorded decision to defer, without arguing against it — see [A gap the author names](#a-gap-the-author-names-is-where-to-look-and-often-where-to-defer).

**A test counts as recording it.**
A test asserting the current behaviour is the author saying "this is deliberate" as loudly as a TODO does, and it is easier to miss because it sits nowhere near the code it pins.
pr-146 again: the acquire hooks' "setting a provider after the first acquire is silently ignored" was raised as a defect.
`material-resolution-test.cc` already carried a CHECK under the comment "Clearing the hook does not un-cache what it already answered with."
Three of that review's nine findings turned out to be already recorded — one TODO, one doc comment, one test.

When the branch already records it, there are only two honest moves.
**Drop it**, if the author's call stands.
Or **raise it as a disagreement with the recorded judgement**, quoting what they wrote and saying why this branch should not ship with it.
That is a different finding, and a much harder one to write.

### A mechanism claim needs the line that proves it

**When a finding turns on "X is derived from Y", read the line that derives it.**
A plausible mechanism assembled from two things that look related is the most expensive kind of wrong: it survives review, it gets agreed to, and it produces a fix for a bug that was never there.

pr-146: the accumulation-hash finding claimed `instance_id` came from `lru_pool`'s `Id(_next++)`.
Eviction would then re-mint it for unchanged content and restart a converged image.
`instance_id` is `instance_id(u32(_instances.size()))` into a plain append-only vector that nothing evicts — the branch's own TODO says "Nothing evicts a parameter block".
Two content-addressed pools sat next to each other and only one of them minted the id in question.
The maintainer had already approved the fix before the error was found.

The check is cheap and specific: grep the constructor of the value, not the type that looks like it owns it.

**Beware two mechanisms with similar names.**
The same review asserted a cache key moved on an include edit, against a header saying it does not.
Both were true — of the DXC compile key and of the slib asset key — and the finding named neither, so it read as contradicting the document it was asking to correct.

### Do not hand back work you already did

A review that says "grep for the readers first" was written by someone who already ran that grep — it is the sentence before it.
Say what the grep found.

The same shape, in three variants seen in one comment:
telling the author to check what their own header says, quoting their own doc comment back at them to establish a point they wrote, and restating their TODO as news.
None of it is rudeness in the wording; it is the *stance*, and it reads as lecturing however politely the sentence is built.

### Commit messages are not evidence

Commit messages are agent-written and unreliable, especially about **provenance**.
A message saying "review feedback" is not proof a human decided anything.
Never let one close a question — raise the question anyway, and say where you saw it.

## Feed the adversarial pass back into this document

The `reviewing-a-pr` skill has a subagent read the drafted comment against the branch with none of the review's context, asking per item whether it could be implemented as written.
**Its findings are not all about that comment.**

Sort what comes back into two piles.
What is wrong with *this* comment goes into the next round.
What names a habit belongs **here**, as a rule with the worked example attached, in the same session.
A class of claim that went unverified, a stance that reads as lecturing, a kind of detail that keeps getting dropped on the way out of a review.

The three rules above about documented findings, mechanism claims and handing back work all came from one such pass.
A pass that produces only per-item corrections has probably been read too narrowly.

## Asking good questions

Questions are how this document grows, so they have to be answerable without the reviewer re-deriving the context.

- **Always name where you saw it** — file, line, the actual code.
- **Give the tradeoff, both sides**, and what it would cost to change.
- **Say what you would do**, so a one-word answer is possible.
- **Keep them few and separate** from the findings, at the end.

## Open questions

Things not yet ruled on.
Each becomes a rule above, an example, or a deletion.

- Where is the line between a design question worth raising and second-guessing a design the author already thought through?
  A partial answer so far: when a redesign is already the author's planned next step, the review's job is to keep this PR from cementing the old shape, not to specify the new one.
- How much of a proposed redesign belongs in the review comment itself versus a linked issue, once a finding turns into a multi-type API change?

## Reference

- [CLAUDE.md](../../CLAUDE.md) — hard rules, layering, and the style preferences a review assumes.
- [docs/coding-guidelines.md](../coding-guidelines.md) — design conventions and the prose rule.
- [docs/error-handling.md](../error-handling.md) — the assert / `cc::result` / exception split a review applies.
- [docs/guides/prose.md](prose.md) — the lint workflow behind the prose findings.
- the `reviewing-a-pr` skill — the flow and the entry structure.
- [tools/review/readme.md](../../tools/review/readme.md) — the tool the flow drives: the change ledger, the entries, and the rounds.
