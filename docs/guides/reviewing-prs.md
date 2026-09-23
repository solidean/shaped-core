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

- **"Does it build" and "do the tests pass."** CI runs ~14 configurations, and `dev.py check --fix` runs every preset its platform has in full — four on Windows, five on macOS, six on Linux.
  Build the branch when you need to *verify a specific claim* — that two layouts really do hash differently, that a symbol really is gone — and then say what you learned, not that it compiled.
- **PR size and splitting.** PRs are ephemeral bookkeeping; only the git repo counts.
  Size matters only as "can this be reviewed", and with agent help that bar is high.
- **Compatibility, migration, deprecation.** The repo is beta through 2026 and likely most of 2027.
  Breaking things is cheap; the wrong shape is expensive.
- **Public API docs written in the intended tense.** See "Docs are for users first" below.
- **Restating the PR body.** The author wrote it.

## What a review owes before it finds anything

Two things, and a review that skips them can be entirely correct and still leave the maintainer unable to judge the change.
Both were asked for out loud, on pr-164, against a review that had nineteen entries and neither of these.

### A tour per feature cluster, with annotated examples as the vehicle

**A review of a change that adds a feature owes an entry that teaches the feature.**
Not the bird's-eye overview the orientation entry already carries — a tour, in the shape a good tutorial takes: the smallest real example, what it produces, and what it replaced.

- **One entry per cluster**, and the count is the judgement.
  Several small features that share a story take one entry between them; a large change with genuinely separate clusters takes one each.
  **One or two is the common case and four is a lot** — past that the clusters are probably one cluster.
- **Annotated code examples are the main vehicle**, not prose about the code.
  The before, the after, and the generated or derived artifact, each as itself.
- **Produce the examples rather than describing them.**
  Run the generator, run the example, paste what came back.
  A tour whose code samples were written by the reviewer from memory is the same claim as a quoted `///` retyped from memory, and it fails the same way.

The reason this is not optional: the maintainer is reading a diff of someone else's design, and the fastest possible explanation of what a branch *is* is one worked example of using it.
pr-164 is the worked case — a branch whose entire subject is a new way to write HLSL, reviewed across nineteen entries, none of which showed a shader written the new way.
The tour that fixed it was five annotations, each with the committed shader, the address the pass writes into it, and the C++ the generator emits.
All of it was produced by running the branch's own generator over the branch's own test shaders, rather than written out by the reviewer.

### An argument that the solution is right, not just that it is correct

**Correctness review answers "does this work". It does not answer "should this exist in this shape", and that is the more expensive question.**
So a review owes a section that names the alternatives and prices them.

- **Enumerate the alternatives, including the ones nobody would pick**, because the boring ones are what establish the frontier.
  Doing nothing structural is always one of them, and it is often the one that reveals which slice of a large branch carries which part of the value.
- **Pro, con and verdict for each**, short.
  The maintainer is checking your reasoning, not reading an essay.
- **Mark which ones the branch already recorded**, and cite where.
  For those the question is whether the recorded reasoning *holds*, never whether the option was seen.
  See [A finding the diff already documents is not a finding](#a-finding-the-diff-already-documents-is-not-a-finding).
- **Then judge the chosen point against what this repo actually values**: API elegance, efficiency, KISS, moving forward, and keeping designs deliberately open to refine later.
  Name the dimension where the change is weakest, and say whether it was knowingly traded.
- **The goal is pareto-optimality, not perfection.**
  Perfectionism is inefficient, and a review that pushes toward completeness nobody asked for costs more than it returns.
  What is being checked is that the solution is good, the shape is right, and the avenues for later growth are open — not that nothing was left undone.

**The highest-value output of this pass is usually an alternative that is not closed in writing anywhere.**
An option ruled out in someone's head is one a future session re-proposes and re-implements.
On pr-164 two of six were in that state.
Rewriting at build time only is refuted by hot reload, and by a location counter that is flat per stage rather than per file.
Generating from reflection is refuted by the same DXC behaviour that motivates the whole branch.
Neither was written down, and the recommendation was to write them into the design doc rather than to change any code.

### Written for someone who has not read the diff

**Every entry that argues — the critique above all, then the verdict — is written for a reader who has not opened the branch.**
The reviewer writes it after reading everything, which is exactly when the branch's vocabulary stops feeling like vocabulary.
So introduce each mechanism before judging it: the situation as a concrete scenario, then each option as *how it works / pro / con / verdict*, in bullets.
[design-critique](../../tools/review/docs/entry-types/design-critique.md#introduce-before-you-price) has the shape, and the cold-read check that catches what the author cannot see.

pr-173 is the worked case: a correct critique the maintainer could not follow — "nothing is properly introduced" — and a verdict they found "always hard to read" as paragraphs.

#### A set named by its cardinality is a set the reader cannot check

**Never write "the four backends" or "the same two asserts" without enumerating them once.**
A count reads as precision and carries none: the writer is counting something they can see, and the reader is being told how many of something they cannot.

It is the most reliable thing the cold-read check finds, because it is invisible from the inside.
The writer re-reads "four backends" and pictures four; nothing on the page disagrees.

Two failure modes, and the second is the one that costs credibility:

- **The count is right and the set is never named**, so the reader cannot verify a word of the argument that rests on it.
  "The same two asserts" three times in one entry, with the two never enumerated, leaves every duplication complaint unjudgeable — is it two lines or two invariants?
- **The count is simply wrong**, which a reader spots instantly and the writer never does.

The metal-raster-completion review is the worked case.
Its design critique said "each of the four backends carries the same two asserts" while the entry named three — dx12, vulkan and metal — leaving the reader to invent webgpu.
The fix was one sentence naming all four at first use, plus a short block enumerating the two conditions before the options that argue about them.

The cheap habit that prevents it: **enumerate at first use, then count freely afterwards.**

### Two alternatives the maintainer wants on the table

These are not preferences that decide a case.
They are options the maintainer likes to **see beside the recommendation**, and then weighs per case — so a critique or a design entry that could offer one and does not has left out a candidate.

- **The strict rule that is obviously correct, beside the clever permissive one.**
  A rule narrow enough to be trivially right, whose later relaxation is purely additive, is a real alternative to a cleverer rule that accepts more today.
  The async-tests design review is the worked case: letting an async invocable take a lock its driver did not hold was going to need a name-ordering constraint to stay deadlock-free.
  The maintainer's counter-proposal was "the driver may hold tags, or the child may, never both".
  It is deadlock-free by the same argument top-level exclusion is, and relaxable later without breaking anything it accepted.
  **It loses on a high-level wrapper's public surface.**
  There the maintainer prefers the complete shape on day one, because widening a wrapper later is friction for every caller even when it is additive.
  The denoising design review is the worked case: denoise-only at one resolution was recommended over a reconstruct contract that admits upscaling.
  The answer, verbatim: "if we don't have api for this day 1 (in the high-level wrapper) we might have friction adding it in the future".
  The strict rule still won inside that surface, where a relaxation reaches no caller — ratios are named presets the routine resolves, with a free ratio left to add later.
- **Deleting a legacy spelling, beside accommodating it.**
  When a new design has to grow a rule only to keep an old spelling working, removing the spelling is an alternative in its own right.
  The same review spent a round designing how `main_thread` should treat an `ASYNC_TEST` body that returns a raw graph instead of being a coroutine.
  The maintainer asked whether the spelling was needed at all; it was not, and removing it deleted the question along with the rule.

## A PR arrives red, and fixing it is the review's job

**The normal flow here is: one contributor writes the branch on the one platform they have, opens the PR while CI is failing, and the review happens next.**
That is deliberate, not a lapse, and a reviewer who treats a red PR as "not ready yet" has misread the process.

The reason is economic.
Fixing CI before a review means fixing code the review may tell you to throw away, and on a branch that reshapes an API that is most of it.
So the order is: write it, open it, review it, then make it green — and the *making it green* is inside the review's scope rather than handed back.

Three things follow, and they are what a reviewer should actually do differently:

- **Build and run the branch on whatever the author could not.**
  That is the first action of the review, not a later verification step.
  On this repo it usually means: the author is on Linux and only vulkan compiles there, so a Windows box compiles dx12 — or the reverse.
  Everything the author's machine could not parse is where the defects are, and [the `#ifdef` arm rule](#the-ifdef-arm-this-machine-does-not-compile-is-where-the-defect-is) is the general form of it.
- **wasm is one of those platforms on every box.**
  The emsdk carries its own node, and `dev.py test --preset emscripten-… --emsdk-path <emsdk>` builds and runs the suite with it.
  "No browser here" is not a reason to review wasm code by reading alone.
- **Land the fixes rather than filing them.**
  A compile error is not a finding — it is work, and it is yours.
  File the *pattern* if there is one worth naming, and put the fix in the working tree.
- **Do not propose process changes to prevent a red PR.**
  Suggesting a pre-merge CI gate, or a rule that branches must be green before review, is arguing against the workflow rather than working in it.

pr-168 is the worked case, and it is worth knowing the shape.
The branch was written on Linux, where dx12 does not compile at all; the PR body said so plainly.
On a Windows box it needed five build cycles to clear: 28 diagnostics across 20 files, in eight clusters.
Clearing them is what made the other two findings visible at all — one real bug in the dx12 transfer drain, and a missing `tick()` that had every render routine declining forever.
None of that is reachable from a diff.
The review's first ask proposed adding a Windows CI gate, and the answer settled it, verbatim:

```raw
CI was red anyways. your task here is to fix the compilation as well. no process changes needed. (it is by design)
```

pr-184 is the case for the "run" half rather than the "compile" half.
The branch was green on its author's four Windows presets, and carried a Linux thread-stack reporter its own header said had never run.
Running it here, on Linux, found four defects no reading had.
The manual hang test was undefined behaviour, and the hang-path dump could wait forever.
The recording it wrote was overwritten a moment later, and every thread's stack came back empty.
The same review first read the wasm half instead of running it, and the maintainer's answer was:

```raw
but why did you not build wasm? i thought we have node and deno on this system
```

Once built, the wasm threads preset reproduced the per-worker stack limit that the review had until then only argued from a standalone probe.

**The corollary for the review artifact:** a red CI is not something to report back, because the author already knows.
What is worth reporting is what the failures turned out to *be*, which is a different and much shorter list.

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
- **Does a setter read like a getter?** A bare-noun method that changes state reads as a query at the call site.
  The async fuzz design proposed `test->inherit_home()`, and the maintainer renamed it `set_inherit_home(bool)`: the verb says it mutates, and the bool makes switching it off expressible.
- **Does a new accessor name an internal of a library still in flux?** Then it pins the internal, and the public shape should say only the outcome.
  pr-170's fix for a test drain needed to wait on sv's fallback shader compile, and first added `sv::frame::fallback_shader_compile()`.
  The maintainer rejected it: sv is alpha and will change a lot, so the accessor exposed a very internal thing for a bad reason.
  What landed was `frame::background_work() -> cc::shared_async<cc::unit>`, which covers the fallback today and grows with the internals while its signature stays put.
  The same holds for a fix a review lands: an accessor added to reach one internal is a finding against the fix.
  **It binds the doc comment as much as the signature.**
  pr-174 kept `sv::background_work(ctx)` outcome-only in its type and then listed the three backlogs it settles in the header, the cheat sheet and the guidelines.
  The maintainer's answer was that exhaustively enumerating internals in public API comments is unnecessary.
  The line they called exactly the right sentiment: "settles once the background work so far is done, including process-wide compiles and cache writes; what it covers is internal".
  What survives from such a list is only the fact a caller could be wrong about — there, that the wait is process-wide despite the `ctx` parameter.

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

### Posting is a separate instruction, and nothing else is one

**Never post a review anywhere the author can see it — a PR comment, a review, an inline note — without the maintainer saying to post it, in those words, after reading the text.**
The go-ahead is an act of its own, and no amount of upstream context substitutes for it.

Three things that are routinely mistaken for one, and are not:

- **The goal.** `--goal pr-comment` says what the artifact *is*, not that it may be published; "goal is pr comment" at the start of a session says the same thing and no more.
- **A round answer.** Approving a draft entry approves the text, which is exactly why the tool refuses to `post` before it and still needs `--confirm` after.
- **The absence of an objection.** A maintainer who never said "don't post" has not said anything.

This holds for a review conducted entirely in chat too, where there is no draft entry and no `post` gate to stop you.
The lighter the process, the more the rule is carrying: a chat review that skips the tool has skipped every mechanism that would otherwise ask.

The worked example is a review of #154 that went well and then posted itself.
The maintainer had opened with "this is a small one, so maybe in-chat is sufficient" and "goal is pr comment", read nothing, and found the comment already on the PR.
A review the maintainer has not seen is a draft whatever its quality, and publishing one spends their credibility on findings they never agreed to.

### A review that lands changes records them as a comment

**The PR description is immutable once the PR is open; what a review changed goes into a PR comment.**
The comment says what the review commits changed and why, measured against the description: which of its claims no longer hold, and what replaced them.
A PR is then a record of how the change developed, which is worth more here than a description kept clean.

This binds a `land-changes` review on our own branch as much as one on someone else's.
The comment is drafted like any other artifact and still posts only on the maintainer's go-ahead.

pr-178 is the worked case.
The review settled from DXC's source the one question the description called unsettleable, and the first instinct was to rewrite that line of the description.
The maintainer's answer, verbatim:

```raw
the pr description stays immutable and we have a record of development instead. that is more valuable for our purposes than a "clean" pr description
```

### No attribution in a review comment

**A review comment carries its content and nothing else: no "🤖 Generated with Claude Code" footer, no session link, no sign-off.**
A harness routinely asks for such a footer on PR text, and for a review comment that instruction does not apply.
For a `land-changes` review the content is a summary of what the review changed; for a `pr-comment` review it is the task list.

pr-180 is the worked case.
The drafted comment ended in the harness's attribution line, and the maintainer's answer, verbatim:

```raw
do not add "🤖 Generated with Claude Code". we keep it professional here and only want a summary of the changes in that comment.
```

### Price work in what it improves and how long an agent takes, never in human hours

**The author hands a comment to an agent, so the work in it is effectively free.**
An option phrased "not worth an afternoon" prices the wrong thing, and it tilts the maintainer toward cutting work that would have cost nothing.

What is actually being weighed is two things.
Whether the change makes the codebase better, and whether it does so at a reasonable latency.
Agents are fast at the work and still slow at wall-clock time, so a sprawling item is not free even when its effort is.

pr-164's follow-up review is the worked case.
A docs pass over eleven files was offered with the option "only the cheat sheets and shaders.md; the rest is not worth an afternoon", and the answer was:

```raw
julius will get your comment but delegates to an agent. the tradeoff is not "an afternoon" (the work is effectively free). we always want to consider if it makes the codebase better with reasonable amount of work latency
```

So when an ask offers a smaller scope, say what the smaller scope *loses*, not what the larger one costs in hours.

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

**The same enumeration binds a fix the review recommends.**
A payload's content hash covers its bytes, while the thing cached from it usually copies more.
pr-179's first draft told the author to key sv's placement slots on each `mesh_attribute::hash`.
The binding the slot holds also copies the attribute's `name`, `format` and `frequency`, so a rename over the same bytes would still have hit the stale fast path.
List what the cached value copies, field by field, and key on all of it.

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

### An amortised pass under a lock is profiled, and says what it can cost

Amortised O(1) is an average, and the pass that pays for it is O(n) in one go.
When that pass runs under a lock other threads take on a hot path, one large instance stalls all of them at once — which a frame shows as a stutter and nothing else attributes.

So a review that recommends such a pass, or finds one, asks for two things beside it.
A `CC_RECORD_SCOPE_IF` gated on the size, so a large pass shows up in a profile by name while small ones cost nothing.
And a comment saying the pass can cause stutter in pathological cases, so whoever sees the scope knows what they are looking at.
Keeping the algorithm simple is still fine; the point is that its worst case is visible rather than solved.

pr-174 is the worked case.
The review recommended that `cc::async_backlog::track_node` compact its whole ring once it doubles past what last survived, instead of pruning from the front.
The maintainer accepted it with exactly this condition: larger compactions behind a record scope, and a comment that they may cause frame stutter in pathological situations.
What landed opens `CC_RECORD_SCOPE_IF(count >= 1024, "cc.async_backlog.compact")`.

### A new link on the async ambient chain is a tax; carry state on what the path already found

The ambient chain is how state follows async work across threads, and everything pushed onto it is paid for.
Pushing a scope allocates a link, and every node started under it retains the chain.
Lookups are cheap only for as long as the tag is absent.
So a design that needs a flag, a sink or an owner to reach work on any worker first asks whether an object the path already looks up can carry it.

The async fuzz design is the worked case.
A CHECK inside an async fuzz op can fire on any worker, and the first proposal caught it with a capture tag pushed onto the chain around every step.
The maintainer declined that as a perf tax, and pointed out that the fuzz is nexus's own and may reach into the running test.
Every check already finds its test through the chain in `current_context()`.
So the design settled on one atomic pointer on `test_context`, set for each async step and read right after that lookup: a relaxed load per check, and nothing per step, spawn or poll.

The same review showed why intercepting beats undoing.
The maintainer's own first shape was a snapshot of the test's check state, restored after each step.
A restore cannot take back the log line each failure writes, nor the throws the per-test failure cap has already made.
A divert read before either happens has nothing to undo.

### A backend finding is shown as the library code that breaks

**When a backend violates its library's contract, the finding opens with the caller's code that goes wrong, not with the backend's internals.**
A few lines of `sg` calls, the sentence of the contract they rely on, and what each backend does between them.
The internals — the clamp, the stage mask, the declare — come after, as the reason.

pr-185 is the worked case.
Metal cut a fragment-stage barrier down to the vertex stage inside a render pass, and the first entry explained it in stage masks, encoder scopes and hazard-tracking modes.
The maintainer's answer, verbatim:

```raw
sorry i dont understand anything the way this entry is written. please make a sg code example where you think our metal backend does not adhere to the sg contract
```

The rewrite was seven lines — two draws sharing a read-write buffer — plus `concepts/barriers.md`'s promise that sg orders them, plus one bullet each for dx12, vulkan and metal.
The same shape also made the fix clearer, because the contract being broken is what the fix has to restore.

### The `#ifdef` arm this machine does not compile is where the defect is

A platform-guarded helper has two arms and only one is ever parsed.
The unbuilt arm is invisible to every check the author can run: the compiler, clang-tidy, the prose linter, and `dev.py check` across every preset its platform has.
So a mistake there survives all of it and lands.
It is the same blind spot as a doc claim, and it wants the same deliberate pass: when a change adds or edits an `#ifdef` arm, **read the arm this machine cannot build, symbol by symbol.**

pr-159 is the worked case, on the branch that made a Windows-only shader compiler cross-platform.
`dxc_compiler-test.cc` grew a two-arm `make_dxc_compiler`, and the `CC_OS_WINDOWS` arm returned `make_dxc_compiler()` — itself.
The function has a deduced return type, so a use before that type is deduced is ill-formed and the test binary does not build on Windows at all; the unbounded recursion is what the error prevents.
The `#else` arm beside it was correct, `check` was green on every preset it runs, and nothing local could have said otherwise.

The tell is mechanical rather than subtle, which is what makes the pass cheap.
One arm called `slib::create_dxc_spirv_compiler()`, and the other called something whose name was the enclosing function's.

The generalization worth keeping beside it: **a change that makes a single-platform library cross-platform doubles the number of arms nobody local compiles.**
That branch had two of them and its PR body named one, which is the ratio to expect.

### A review of code nobody here can run says so, and hands verification to the author

Some branches cannot be built on any machine the reviewer has — a Metal backend reviewed from Windows is the case that set this.
Everything such a review finds is read off the source, and the comment must say that plainly rather than wear the register of a verified finding.

- **Frame the whole comment as inferred from source only**, once, at the top.
- **Make each item reverify → fix → test**, so the author's agent checks the mechanism before changing code.
- **Mark the items that hinge on runtime ordering or vendor API behaviour as plausible**, to be confirmed on the author's machine first.
- **Keep the plausible ones in the comment.**
  Most of them end in a missing test, and the test is worth having even when the finding turns out wrong.

pr-177 is the worked case: four parallel readers over a whole Metal backend, with nothing runnable.
The maintainer's answer to the verdict, verbatim:

```raw
basically prepare the pr comment with what you found but keep it as "inferred from source only". lots of it are request for more tests, so always valuable. the other stuff julius' agent should just reverify and check
```

### A one-for-one migration keeps the old idiom's breadth, and that is where to look

When a change translates a pattern mechanically — blocking into awaiting, one API into its successor — each call site inherits what the old spelling had to do, not what the site needs.
The translation is correct, so it survives review, and it cements a wait or a check the new API made unnecessary.
So for each translated idiom, ask what the site actually needs and whether the new API has a narrower spelling for it.

pr-173 is the worked case, twice, and the maintainer caught both rather than the review.

- `ctx->block_until_idle(); future.try_get_data()` became `co_await ctx->idle_completion(); future.try_get_data()` in 144 places.
  A readback needs only its own bytes, which the future's own completion already signals; the idle wait held every submission, actor and epoch besides.
  The fix added `future.data()` and awaited that instead.
- A hand-rolled `await(a)` that pumped until `a->is_ready()` became `co_await cc::async_settled(a); a->value()` everywhere.
  Only one site inspected the failure; everywhere else a plain `co_await` was shorter and failed by name instead of asserting.

The tell is a new line that reads as ceremony around the value the site wanted.

### A guarantee only the old implementation gave is not a regression

When a rewrite stops doing something the old code happened to do, ask whether the API ever promised it before calling the rewrite wrong.
If the only thing that promised it is a sentence, the defects are that sentence and the callers that leaned on it — not the new behaviour.

pr-171 is the worked case.
nexus used to run every `main_thread` test in a serial phase of its own, so they never overlapped anything.
The branch made them ordinary nodes, and the review filed "a blocking `main_thread` body runs other tests nested on its stack" as a bug, recommending the runner serialize them again.
The maintainer's answer, verbatim:

```raw
a main_thread TEST runs its body on the mainthread. [...] interleaving and nesting TESTs via this mechanism is completely fair. I'd say the old way nexus did it is simply wrong. if you want one-at-a-time, you need exclusive, NOT main_thread. [...] the fix though, is simple: DO NOT BLOCK.
```

Re-read that way, the finding turned into three different ones, all real.
The docs still promised "one at a time among themselves".
Every `sr::window_system` test and every sv capture test was relying on the old serialization for exclusion it never asked for — a process-wide singleton, and process environment variables.
And a pin justified by blocking shader compiles was stale: it was written before render-routine init became coroutines, and nobody rechecked it after.

**The tell is a recommendation that restores old behaviour.**
Before writing one, name the flag or type whose contract covers it; when none does, look for the callers that need a contract they never stated.

### "No callers in the repo" is not evidence of dead code

It is evidence only for something the repo alone can use.
A symbol in a library's exported `FILE_SET` — a backend header included — is reachable by consumers this tree does not contain.
An unused-looking member there wants its *correctness* checked rather than its existence questioned.
Say which of the two you are claiming, because they get answered differently.

**It binds a design review too, where "nobody uses it" tempts a deletion that makes a new backend cheaper.**
The WebGPU backend design found `sg::bound_sampler` — a register-bound static sampler on a pipeline layout — constructed by no library or example, only by three tests.
WebGPU has no static samplers, so the review recommended deleting the spelling rather than emulating it in the reserved group.
The maintainer's answer, verbatim:

```raw
let's go with B. so we support it fully. the feature is simply not used _yet_. and we dont expect to write WGSL by hand for long.
```

A feature with no callers in a young library is usually a feature whose callers have not been written.
Deleting it is the option to offer, not the one to recommend on that evidence alone.

### Drive-by cleanups are welcome where the PR already is

The libraries are in flux, so cleanups get postponed by prioritization rather than by policy — fringe and niche APIs carry debt on purpose.
That is a schedule, not ossification.

So **asking to down-pay debt in code the PR is already touching is a good finding**, and worth making concrete: name the call sites and the replacement.
Asking to clean up code the PR does not touch is not.

### A local helper is either a duplicate or a recorded gap, and it has to say which

Every codec, parser and backend grows a handful of small private helpers — is this character whitespace, read four bytes in a stated byte order, append a string to a byte buffer.
They are three to twenty lines each, obviously correct in isolation, and written without checking whether `cc` already has them.

**Check the foundational libraries for each one.**
The cost of the duplicate is not the lines.
It is that the local copy is a *slightly different* function under the same name, and the difference surfaces later on an input nobody tested.

Three outcomes, and the review's job is to sort each helper into one:

- **It exists in `cc` (or `tg`)** — delete the local copy and call the real one.
- **It does not exist and belongs there** — record it as a lower-library gap, and leave the local version with a one-line comment saying `cc` has no equivalent yet.
  That comment is what stops the next reviewer re-raising it, and what stops the next author writing a fifth copy.
- **It is genuinely specific to this format or backend** — leave it, unexplained is fine.

The recorded-gap half is what makes the rule work in both directions.
A repo that only ever says "use the `cc` one" pushes authors toward not writing helpers at all, which is worse.

The worked example is pr-152's two native image codecs.
`cc::is_space` exists in `clean-core/string/char_predicates.hh`, and both codecs defined their own — matching space, `\t`, `\r`, `\n`, but not the `\f` and `\v` that `cc::is_space` matches.
A PFM header separated by a vertical tab therefore parsed as one glued token and returned a bogus dimensions error.
`pfm.cc` also hand-rolled 34 lines of byte-order-explicit float load and store.
`clean-core/common/endian.hh` already had `cc::load_bytes_le/be` and `cc::store_bytes_le/be`, under a header comment naming that exact use case as "the durable-format primitives".
Meanwhile `trimmed` and `split_tokens` in the same files genuinely had no `cc` equivalent, and belonged in babel's `docs/lower-library-gaps.md` rather than being deleted.

**Grep the foundational library for the helper's name and for what it does, not just its name.**
`float_from_bytes` finds nothing; `load_bytes` finds the header that made it unnecessary.

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

**A spec rule that states a limitation counts too, and so does a comment in a test generator.**
The SGL review filed "a `case` pattern that inlines a helper fails to emit" as a legalizer bug.
The branch's `legalization.md` already had LEGAL-47, "A pattern that has an effect is not carried: C1 runs behind the expression rules".
The random program generator said the same beside its pattern code.
The maintainer chose the fix anyway, which is the second honest move below; the finding should have been raised as one.

When the branch already records it, there are only two honest moves.
**Drop it**, if the author's call stands.
Or **raise it as a disagreement with the recorded judgement**, quoting what they wrote and saying why this branch should not ship with it.
That is a different finding, and a much harder one to write.

**The same rule binds a finding about the review tool**, and it bites harder there.
The reviewer is inside the tool rather than reading about it, and mistakes that familiarity for having read its docs.
pr-167 is the worked case.
The tooling entry reported that `validate` resolves paths inside fenced blocks, so pasting a build driver's output — object paths, an SDK header — is impossible without falsifying it.
It recommended a per-block opt-out.
The opt-out exists: `raw` as a fence's info string, and `` `raw:…` `` for one span.
It is implemented in `tools/review/lib/annotate/table.py:36-57`, documented under a heading of its own in `tools/review/docs/block-grammar.md`, and said again in `tools/review/readme.md`.
The reviewer had read the grammar down to the section above it and stopped.

So **read the tool's own docs end to end before filing against it**, the same way you read the branch's.
What survived in that case was a much smaller finding: the failure message named the problem and no remedy, which is the one moment an author would have found the feature.

### A test's comment is a claim about the test, checked the way a doc claim is

Read what the test asserts, then read what its comment says it asserts.
A comment that overstates is worse than no comment: it tells the next reader the property is covered, so nobody covers it.
And it survives review, because the code beneath it is correct as far as it goes.

pr-159 is the worked case.
Its headless-present test said "a swapchain that handed out the same image every frame, or presented one and rendered into another, would fail".
The loop clears the acquired view to a per-frame shade and reads back **that same view** before presenting.
So a chain returning index 0 forever passed every check in it, and so would one that presented a different image than it rendered.
What the test actually pinned was that the acquired target is renderable and readable, which is worth having and is not what the comment claimed.
The missing property was three lines away: collect the acquired texture's address per frame, and require `buffer_count` distinct ones.

This is the inverse of the rule above, and the two are worth holding together.
There, the author's prose is evidence you can trust and re-deriving it is the mistake.
Here it is a claim like any other, because nothing in the toolchain checks a comment against the code under it.

### A mechanism claim needs the line that proves it

**When a finding turns on "X is derived from Y", read the line that derives it.**
A plausible mechanism assembled from two things that look related is the most expensive kind of wrong: it survives review, it gets agreed to, and it produces a fix for a bug that was never there.

pr-146: the accumulation-hash finding claimed `instance_id` came from `lru_pool`'s `Id(_next++)`.
Eviction would then re-mint it for unchanged content and restart a converged image.
`instance_id` is `instance_id(u32(_instances.size()))` into a plain append-only vector that nothing evicts — the branch's own TODO says "Nothing evicts a parameter block".
Two content-addressed pools sat next to each other and only one of them minted the id in question.
The maintainer had already approved the fix before the error was found.

The check is cheap and specific: grep the constructor of the value, not the type that looks like it owns it.

**A recommendation is a mechanism claim too.**
pr-170 found that `exclusive(...)` on an `INVOCABLE_TEST` is silently ignored, and recommended that nexus union each child's tags onto the drivers that dispatch it.
That fix assumes a static link from driver to child.
There is none: a driver calls `nx::invoke_tests` at runtime, and which invocables it reaches is discovered then, so nothing before the schedule is built knows the edge.
The maintainer's answer was a runtime assert at dispatch instead — a child's flags must be held by whichever test invokes it.
The finding was right and the fix was unbuildable, and the review had even written "I have not checked how a driver's dispatched parameter type is known to the scheduler" beside it.
A sentence like that is the check, left undone; do it before recommending, not after.

**A proposed assert is checked against what the code declares today, not against what it should declare.**
pr-185 found metal leaving a fragment-stage write unordered against a later draw in the same pass, and first prescribed asserting when a resource "declared with a fragment-stage write" was read again.
`declare_bound_groups` declares every bound group resource as `shader_read | shader_write`, whatever its binding allows.
So every resource shared by two draws already looked like that write, and the assert would have fired on imgui's font texture in every frame.
The finding survived; the fix had to start with declaring real per-binding access, the way dx12's `hazard_views` do.
Before prescribing a check on a declared property, read the one function that declares it.

**A member of the right type is not the mechanism wired.**
A helper that notifies, retires or releases usually needs a call that connects it, and holding the helper proves nothing about that call.
pr-177's first draft said metal's streams "do notify, through `sg::impl::transfer_drain`", because `metal_stream_system` holds one.
It calls `_drain.start()` and never `_drain.notify_on_drained(&ctx)`, which every dx12 and vulkan drain calls, so none of metal's three drains notified.
Grep for the connecting call, not for the member.

**A fix names inputs, and each one has to be reachable where the fix goes.**
The same draft told the author to call `impl::notify_transfer_drained(*ctx)` from a commit feedback handler that captures no context on purpose, because it can run after shutdown.
It told them to push a cancellation into a completion hidden inside a `cc::unique_function`, and to assert against a raster pipeline layout with no accessor.
Every mechanism was right and every fix was unbuildable as written.

**So is the price of an option you did not recommend.**
The WebGPU backend design needed a wasm test to wait for a WebGPU callback, and priced "make nexus's executor resumable" as much larger than linking JSPI into test binaries.
It called the change "much larger" and one that "changes nexus on every platform to serve one", and rejected it on that.
The maintainer asked whether an async run driven by the browser loop would not simply work.
It would: every test is already an async node, and the blocking lives in three drivers, of which a no-threads build reaches only `drive_serially`.
Its "no progress" branch is exactly where a return to the host goes.
The executor had not been read when the option was priced, and the inflated price is what made the recommended option look cheap.

**"Nothing has an effect" is a claim about every write the language can make.**
The SGL review priced an evaluation-order question as invisible today, since "nothing SGL can write has an observable effect except `print`".
The maintainer answered with three lines: a helper that stores to a buffer, called from the index of a store to that buffer.
Buffer stores had landed on the same branch, so the claim was stale the day it was written.
Checking it took one more step, which also found a real bug: an element read to the left of such a helper was read after the store.
List the writes the language has, and ask of each whether a helper can make one, before calling an order unobservable.

**Beware two mechanisms with similar names.**
The same review asserted a cache key moved on an include edit, against a header saying it does not.
Both were true — of the DXC compile key and of the slib asset key — and the finding named neither, so it read as contradicting the document it was asking to correct.

**A prescribed drain, wait or guard is a mechanism claim about what is outstanding.**
Naming the remedy without naming what leaked is a guess.
It is one that looks like diligence, because the remedy is usually sound in general.

pr-179 again, and again conceded by the reviewer.
A GPU test was reported as leaking "2 async item(s)" on a cold cache, with the nodes unidentified, and the prescription was to settle `sv::background_work` the way the neighbouring test files do.
The two items were the second batch's STREAM UPLOADS: `quadrics.acquire` queues one transfer per buffer, and that test acquires again after its only drain.
So `background_work` does not cover them at all, and `wait_for_pending_uploads` does.
The leak was real and the prescription was inert.

**Look for the configuration that makes the race deterministic before writing the item.**
Here it was the `singlethreaded-*` preset: with no worker thread, work advances only when something pumps, so an undrained transfer fails every time instead of once per cold cache.
That is a two-minute run and it converts "I saw it once and could not reproduce it" into a named mechanism.
Reach for it whenever a finding rests on something that happened on one run — a leak, an ordering, a timeout.
This repo keeps presets that remove exactly the concurrency such a finding depends on.

### A doc's statement of its own limits is a claim, not a boundary

The rules above say the author's prose is evidence you can trust, and that a test's comment about itself is a claim to check.
There is a third position, and it is the one that bites hardest: **a design doc's account of what the design cannot do yet.**

That sentence is written early, by someone reasoning about a case they deliberately did not build, and nothing ever rechecks it.
Accepting it is worse than accepting a wrong finding, because it propagates into the review's own recommendations as a constraint.
The reviewer starts scoping advice around an obstacle that is not there.

So read a "what this does not address" section the way you read a claim about a mechanism: **go and look at the thing it says is not served.**

pr-164 is the worked case.
Its design doc closed with sv's bindless tables, saying they "need a register space per table, and nothing here assigns a space other than one per group".
The review repeated that and built a recommendation on it — reject hand-written addresses, but not until bindless is served.
The maintainer pushed back, and the code settled it in two greps.
Bindless in this tree is a fixed-size array in a space of its own, `sv::space_of` numbers one space per table from 1, and one space per group is exactly one space per table.
The pass already emitted precisely those bytes.
There was no obstacle, the migration was ten files rather than a blocked feature, and the doc's own sentence was the only thing standing in the way.

**The tell is a limitation stated in terms of the design's vocabulary rather than in terms of the blocked thing.**
"Nothing here assigns a space other than one per group" describes the pass.
It never says what a bindless table actually needs, which is the fact that decides it.

### A UB claim is checked against the standard the repo compiles as

C++20 redefined signed left shift as modular rather than undefined, so `255 << 24` is well-defined in this repo's C++23 and yields a negative number.
A reviewer running on a pre-C++20 reflex files "this is UB", and the author checks the standard and discards the finding.

Worse, the reflex hides the real bug.
pr-152 is the worked case: `decode_flat_scanline` was filed as signed-overflow UB, and the actual defect was what the well-defined negative value then does.
A negative `run` passes the `x + run > width` guard, drives `x` negative, and the next iteration memcpys through a pointer below the buffer — an out-of-bounds write rather than an overflow.
The wrong claim was less severe than the truth.

**Say which construct is undefined and why, and check the version.**
Shifts, signed overflow, `char` signedness and aggregate init all changed under recent standards.

### A measurement of a failing system says nothing about why it is failing

A number is evidence, and a number taken from a run that is *already* broken is usually evidence about the breakage rather than about its cause.
The failure mode is subtle because the measurement is real and the reasoning from it feels quantitative.

pr-168 is the worked case.
The viewer's routine chain declined every frame, and the question was whether it was still compiling or permanently stuck.
The review raised the frame count from 8 to 300, saw all 300 run in 267 ms, and concluded: far too fast for a shader compile, therefore stuck rather than slow.

The frames were fast **because** they declined.
A loop whose body returns immediately runs quickly whatever the reason, so the timing measured the symptom and was consistent with either answer.
The actual test was one line — a 50 ms sleep per frame — and it passed, because it had been a latency race all along.

**The check is to ask what the measurement would look like under the other hypothesis.**
Where the answer is "the same", it is not evidence.
A timing taken from a system that is doing nothing almost always fails that test.

### Measure the code, not a model of it

**A number taken from a re-implementation is a claim about the re-implementation.**
Modelling the code in numpy or in a scratch program is the natural way to check a numerical claim.
It is right up to the point where the model and the code round differently, which for a precision finding is the whole subject.

pr-179 is the worked case, and the reviewer caught it themselves.
The finding was that a thin quadric loses its silhouette at distance, because `qc = |o|² − r²` swamps the radius in float32 and the discriminant's sign becomes rounding noise.
That was right, and the fix — shifting the solve to the ray's closest approach — was right.
The reported SYMPTOM was not.
The table came from a numpy float32 model whose ray directions were built in float64 and cast down, and it showed a mix of missed hits and false hits, described as tubes "dissolving".
Run against `sv::intersect` itself, with rays built the way the code builds them, every ray aimed *outside* the sphere reports a hit from 50 units out.
Nothing dissolves; the silhouette inflates.

Which side the error lands on depends on exactly how the ray is constructed, so a model that builds them differently answers a different question.

**The remedy is usually cheaper than the model.**
`sv::intersect` is a plain CPU function with no device behind it, so a test could have called it directly.
That is what the eventual test does, and what produced the real number.
Before modelling, ask whether the thing under test is already callable.
Where it is, the model is strictly worse evidence than the call.

**A finding can be right about the cause and wrong about the symptom**, and that is worth stating rather than smoothing over.
The fix survives and the description does not, and an author who checks the description first will conclude the whole item is wrong.

### "The only X" is a count

[A count is a claim](#a-count-is-a-claim-and-it-is-checked-by-enumerating) covers numerals.
The same rule binds the words that imply one — *the only*, *the single exception*, *the one place*, *nothing else*.
Those are easier to write without noticing, because they read as emphasis rather than as arithmetic.

They are also disproportionately load-bearing, because a reader uses them to stop looking.
"`block_until_idle()` is the only sg call that waits" is an instruction to grep for one name.

pr-168 again, from the other side: the branch said it in four places, having settled on two spellings.
Each sentence was true when written.
Nothing in the diff of the change that added the second one touches the sentences that say there is one, which is what makes this the doc-rot category rather than a proofreading miss.

**Enumerate on the way past.** When a review is about to write "the only", or is reading one, that is the moment the set gets listed.

### A count is a claim, and it is checked by enumerating

A number in a review reads as evidence, which is exactly why a wrong one is expensive.
"Twenty declarations across ten files" is a stronger sentence than "several files", and a reader who checks it and finds nineteen stops trusting the rest of the item.

**The failure is counting from a grep's output rather than from the set the claim is about.**
A grep hits comments, hits files outside the population, and hits the same file twice.
Every one of those is a plausible off-by-one that survives re-reading, because re-reading the sentence does not re-count anything.

pr-164's comment carried three wrong counts in one draft, all from that reflex.
"`sg::vertex_attribute_format` has 16 members and the pass can produce 14" — it declares 14 and the pass produces 12.
"Twenty hand-written `register()` declarations across ten files" — nineteen across nine, because the twentieth was in a backend test that never reaches the pass and one more hit was inside a comment.
"Four single-binding compute test shaders" — three.
The argument each number supported was correct in all three cases, which is what makes this worth a rule rather than a proofread: the finding survives, and the credibility does not.

**So either enumerate the set in the comment, or drop the number.**
A list of nine paths is longer than "nine files" and it is checkable, self-correcting while you write it, and more useful to the person doing the work.
Where a count really is the point — a coverage claim, a before/after — produce it with a command whose output you paste, rather than by reading a list and counting.

### Check whether the code already does the thing you are asking for

A finding that asks for an assertion, a check or a guard is a claim that it is absent.
That claim is checked by grepping the code paths it would live in, and it is the check most often skipped.
The reason is that the reviewer arrived at the request by reasoning about what *should* exist rather than by reading what does.

The cost is worse than a wasted item.
The author reads an instruction to add something that is already there, and has to work out whether the reviewer means something subtler or simply did not look.
The honest answer is usually the second.

pr-164 is the worked case.
The review asked that a generated `bind<G>` "check `G::group_index` against the bound pipeline layout's groups", because the group index had disappeared from the call site.
Both backends already assert exactly that, at `dx12_command_list.cc:255` and `vulkan_command_list.compute.cc:56`, each beside a layout-identity assert.
The concern was real and the request was already satisfied.
What the item needed to say was that the existing asserts cover it, so the templated version must keep passing the index rather than reaching past it.

**The tell is an instruction phrased as "make X check Y".**
Before writing one, grep for the check in the two or three places it would have to live.

### Verbatim means pasted

A quoted `///`, doc line or comment is a claim about a file, and it is checked the same way a line number is.
Retyping one from memory truncates it, moves a comma, or silently merges two lines into a sentence the file does not contain.

pr-152 quoted `clean-core/common/endian.hh` in one sentence.
The file has it as two lines, and the second one ends `..., and reading it must never depend on the host's.`
The file has two lines, and the second ends `..., and reading it must never depend on the host's.`
The truncation landed exactly where the quote stopped supporting the point being made, which is the shape a reader notices.

Paste it, keep its line breaks, and cite the line the paste starts at.

**And cite where it is actually from.**
A quote attributed to the wrong source is wrong twice: the sentence is unfindable where the review says it is, and the authority the attribution borrowed was never lent.
pr-164's comment introduced a sentence with "Q14 measured it as portable" and then quoted `binding-preprocessor.md`.
Q14 is a test; the sentence was the author's own doc, on the branch under review.
That also made it the second failure below — quoting someone's documentation back at them to establish a point they wrote — where a pointer to the line would have carried the whole argument.

### A rename's call-site list comes from a grep of the name

Listing the sites you happened to read while forming the finding is not the same as listing the sites.
The ones missed are systematically the ones a reviewer does not visit.
A comment mentioning the symbol, a family list in a neighbouring header's `///`, a doc that recorded the symbol back when it was a gap.

pr-152 named five places to update for a `tg::pow2` rename and missed four.
The declaration's own doc comment, a family list in `scalar/traits.hh`, a prose mention in the calling `.cc`, and the retired-gap entry in babel's `lower-library-gaps.md`.
Every one of them was a plain text match on the name.

An instruction the author has to complete themselves is not an instruction.

**The same holds for any change to a symbol, not just a rename.**
An instruction that removes a table row, changes a constructor or replaces an overload owes the list of what references it: tests, test fakes, corpus cases, generated doc comments, other docs.
pr-164's follow-up comment was right about every line it named and missed the references in four items.
Removing the `Buffer` row would have broken the corpus's every-type case, which declares `Buffer<float4>` and numbers the bindings after it.
Giving `sg::binding_group_layout` a new constructor argument also reaches both backends' subclasses and `fake_group_layout` in `layout_hash-test.cc`.
Replacing `create_binding_group(G const&)` left five docs still spelling the old call.
Renaming a CMake custom target missed the `add_dependencies` naming it and the property that records it.

### An instruction to "both halves" is checked against each half separately

Where a design implements one thing twice — a runtime parser and a build-time generator, two backends — an instruction written once for both assumes the halves have the same machinery.
They rarely do, and the difference is exactly what the author discovers halfway through.

pr-164's follow-up is the worked case, twice.
"Pin vertex-input member offsets in both halves" assumed the C++ parser computes them.
It computes only source offsets for the `[[vk::location]]` edit, so the instruction was new layout code rather than a corpus change.
"Refuse two push-constant struct bodies of one name in both halves" assumed both read the same text.
The Python generator reads the raw file and the C++ pass the flattened one, so an `#ifdef __spirv__` fork is two bodies to one and one body to the other.

So for each "both halves" item, open each half and find the code the instruction would extend.

### Items in one comment are checked against each other

A long comment is written item by item, and an instruction's target wording in one section can undo another.
pr-164's follow-up told the author to let non-address `[[vk::…]]` attributes through in a dialect file.
Four items later it told them to document that a hand-written `[[vk::…]]` is valid only in a file with no attribute.
Read the draft once for exactly that before handing it over.

**A test one item asks for is checked against the fixes the other items ask for.**
pr-179's first draft asked for a trace test proving a still-streaming quadric batch draws its placeholder.
Two items later it moved the procedural stand-in's acquire earlier, and a trace declines until that stand-in compiles.
So the test would have raced both the upload and the new compile.
The instruction that survived checks the bindless index `describe_instance` returns before anything drains, which is deterministic and needs no trace at all.

### A bound is checked against every path the code dispatches to

When a finding recommends replacing a constant limit with one derived from the input, enumerate the code paths that limit has to hold for.
The derivation is usually worked out against the path the reviewer just read, and a second path with a different expansion factor makes it reject valid input.

pr-152: an allocation ceiling derived from the remaining bytes was correct for HDR's adaptive RLE, which expands at most about 16 pixels per byte.
`read` dispatches per scanline, and the flat path's `1 1 1 n` marker expands 4 bytes into up to 255 pixels — more once a marker chain is allowed.
Shipped, it would have rejected legal old-format files.

The honest conclusion was that a run-length format has no linear bound and wants a sane constant instead, which is a different recommendation from the one the maintainer had already approved.
**Catching this after approval is normal; that is what the adversarial pass is for.** Supersede the recommendation rather than shipping the approved-but-wrong one.

### Do not hand back work you already did

A review that says "grep for the readers first" was written by someone who already ran that grep — it is the sentence before it.
Say what the grep found.

The same shape, in three variants seen in one comment:
telling the author to check what their own header says, quoting their own doc comment back at them to establish a point they wrote, and restating their TODO as news.
None of it is rudeness in the wording; it is the *stance*, and it reads as lecturing however politely the sentence is built.

**The variant that survives this rule is the contract-mismatch finding**, because there the author's own doc is the evidence.
pr-161 is the worked case: a failed texture sampled an uninitialized resource rather than its placeholder.
The finding quoted `residency::failed`'s doc comment and the branch's own substitution table at length, to establish that it should have been substituted.
Both were written by the author, on that branch, and the whole force of the finding was that the code did not match them.
**Cite the contract and move on.**
A pointer says the same thing as a block quote, and the block quote is what turns "your code disagrees with your doc" into "let me read your doc to you".

### A second citation is a second claim

**A finding propped on a supporting citation is only as strong as the weaker one.**
Reaching for a second reference because the first felt thin is the tell: it doubles the surface a reader can check, and one false line invites them to discount the finding that was correct.

pr-161: a doc-rot finding said `libs/graphics/shaped-viewer/docs/_index.md` still called asset loading "planned", and added that `docs/structure.md` "marks all five phases `[done]`".
The first half was right.
The second file has no phase rows at all — the phasing lives in `asset-loading.md`, which marks phase 5 half-landed.
So the support overstated exactly the thing it was there to prop up.
The finding needed no second citation, and the one it got was the only checkable claim in the item that was false.

If a finding does not stand on one verified line, that is a finding to re-derive rather than to buttress.

### Praise is addressed to the author

Praise that carries information is worth writing — the existing rule — but **write it to the person reading it.**
A comment lands on someone's branch, and a reviewer full of three rounds of context slips easily into narrating that person's work to a third party.

pr-161's closing section opened "three decisions here are worth more than they look, and a session working from this list would not know it".
Every word of the assessment was meant and useful; addressed past the author to a hypothetical later agent, it reads as a pat on the head.
"This is the right shape, do not churn it" is the same information in the second person, and it is shorter.

The same slip in the other direction is a small jab attached to a correct observation — "which is why nothing caught it", "which suggests it was not meant to".
Both were true, and both read the same without the closing clause.

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
