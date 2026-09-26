# Reviewing PRs

What we look for in a shaped-core review, and how we weigh the calls that have no right answer.
The mechanics — fetching the branch, accounting for every change, the entry structure — belong to the `reviewing-a-pr` skill and to [the review tool](../../tools/review/readme.md);
this is the taste behind them.

**This document is alive.**
Every rule in it was said out loud in a real review, usually as a correction to a draft that had it wrong.
The worked cases stay because the rule without its case overgeneralizes; the PR numbers do not matter and are left out.
An entry with no case under it is a hypothesis, not a rule.
Reviews happen in chat, point by point, so the reviewer's answers can land back here.

## Who the review is for

The reviewer is in contact with the author, and a review speaks for both of them.
So the artifact is **a todo list for the author's next agent session**, not a politely framed request.

- **Write instructions, not suggestions.** "Delete the copy constructor" beats "you might consider making this non-copyable".
- **No softening, no hedging, no thanks-for-the-PR preamble.** The author expects this register and reads it faster.
- **Praise only where it carries information**, and write it to the author.
  "This is the right shape, keep it" tells the next session not to churn it; "great work overall" tells it nothing.
  Assessment addressed past the author to a hypothetical later agent reads as a pat on the head.
  A correct observation with a small jab appended — "which is why nothing caught it" — reads the same with the clause cut, so cut it.
- **The review lands as one comment on the PR**, so someone reading only that comment must be able to act on every point.
- **Price work in what it improves and how long an agent takes, never in human hours.**
  The author hands the comment to an agent, so effort is effectively free; what is weighed is whether the change makes the codebase better at a reasonable latency.
  "The rest is not worth an afternoon" prices the wrong thing and tilts the maintainer toward cutting work that would have cost nothing.
  When an ask offers a smaller scope, say what the smaller scope *loses*.

## What is not a review finding

Cut these before writing.

- **"Does it build" and "do the tests pass."** CI runs ~14 configurations and `dev.py check` runs every preset its platform has.
  Build the branch to *verify a specific claim*, then say what you learned, not that it compiled.
- **PR size and splitting.** PRs are ephemeral bookkeeping; only the git repo counts.
- **Compatibility, migration, deprecation.** The repo is beta through 2026 and likely most of 2027.
  Breaking things is cheap; the wrong shape is expensive.
- **Public API docs written in the intended tense.**
  "Every backend's `bind_group` asserts the slot matches" is correct writing when one backend is a non-recording stub: the reader is a user, and whoever implements the stub has to match the sentence.
  A doc claiming a capability *no* backend has is still wrong.
- **Restating the PR body.** The author wrote it.
- **Process changes.** See [A PR arrives red](#a-pr-arrives-red-and-fixing-it-is-the-reviews-job).

## What a review owes before it finds anything

A review can be entirely correct in its findings and still leave the maintainer unable to judge the change.
Both of these were asked for out loud, against a review that had nineteen entries and neither.

### A tour per feature cluster, with annotated examples as the vehicle

**A review of a change that adds a feature owes an entry that teaches the feature**: the smallest real example, what it produces, and what it replaced.
Not the bird's-eye overview the orientation entry already carries — a tour, in the shape a good tutorial takes.

- **One entry per cluster, and the count is the judgement.** One or two is the common case and four is a lot.
- **Annotated code is the vehicle, not prose about the code.** The before, the after, and the generated artifact, each as itself.
- **Produce the examples rather than describing them.** Run the generator, paste what came back.
  A sample written from memory is the same claim as a quoted `///` retyped from memory, and fails the same way.

The maintainer is reading a diff of someone else's design, and the fastest explanation of what a branch *is* is one worked example of using it.
A branch whose whole subject was a new way to write HLSL was once reviewed across nineteen entries, none of which showed a shader written the new way.

### An argument that the solution is right, not just that it is correct

**Correctness review answers "does this work"; it does not answer "should this exist in this shape", and that is the more expensive question.**

- **Enumerate the alternatives, including the ones nobody would pick** — the boring ones establish the frontier, and "do nothing structural" is always one.
- **Pro, con and verdict for each, short.** The maintainer is checking your reasoning.
- **Mark which ones the branch already recorded**, and cite where; for those, the question is whether the recorded reasoning *holds*.
- **Judge the chosen point against what this repo values**: API elegance, efficiency, KISS, moving forward, and designs kept deliberately open to refine later.
  Name the dimension where the change is weakest, and say whether it was knowingly traded.
- **The goal is pareto-optimality, not perfection.**
  What is checked is that the shape is right and the avenues for later growth are open, not that nothing was left undone.

**The highest-value output of this pass is usually an alternative that is not closed in writing anywhere.**
An option ruled out in someone's head is one a future session re-proposes; the recommendation is then to write it into the design doc, not to change code.

### Written for someone who has not read the diff

**Every entry that argues — the critique above all, then the verdict — is written for a reader who has not opened the branch.**
The reviewer writes it after reading everything, which is exactly when the branch's vocabulary stops feeling like vocabulary.
Introduce each mechanism before judging it: the situation as a concrete scenario, then each option as *how it works / pro / con / verdict*, in bullets — never as paragraphs.
[design-critique](../../tools/review/docs/entry-types/design-critique.md#introduce-before-you-price) has the shape and the cold-read check.

**A set named by its cardinality is a set the reader cannot check.**
"The four backends" or "the same two asserts" reads as precision and carries none.
Either the count is right and the set is never named, so nothing that rests on it can be verified, or the count is wrong and the reader spots it instantly.
A critique once said "each of the four backends carries the same two asserts" while naming three and never listing the two conditions.
**Enumerate at first use, then count freely afterwards.**

### Two alternatives the maintainer wants on the table

Not preferences that decide a case — options the maintainer wants **beside the recommendation**, weighed per case.

- **The strict rule that is obviously correct, beside the clever permissive one.**
  A rule narrow enough to be trivially right, whose later relaxation is purely additive, is a real alternative to a cleverer rule that accepts more today.
  Letting an async invocable take a lock its driver did not hold was going to need a name-ordering constraint to stay deadlock-free.
  The counter-proposal was "the driver may hold tags, or the child may, never both": deadlock-free by the same argument top-level exclusion is, and relaxable later.
  **It loses on a high-level wrapper's public surface**, where the maintainer wants the complete shape on day one: "if we don't have api for this day 1 we might have friction adding it in the future".
  Inside that surface, where a relaxation reaches no caller, the strict rule was still the one chosen — ratios as named presets, a free ratio left to add later.
- **Deleting a legacy spelling, beside accommodating it.**
  When a new design has to grow a rule only to keep an old spelling working, ask whether the spelling is needed at all; removing it deletes the question along with the rule.

## A PR arrives red, and fixing it is the review's job

**The normal flow: one contributor writes the branch on the one platform they have, opens the PR while CI is failing, and the review happens next.**
Fixing CI before a review means fixing code the review may throw away, so the order is write, open, review, then make it green — and making it green is inside the review.

- **Build and run the branch on whatever the author could not.** That is the first action, not a later verification step.
  The author is on Linux and only vulkan compiles there, so a Windows box compiles dx12, or the reverse.
- **wasm is one of those platforms on every box.** The emsdk carries its own node, and `dev.py test --preset emscripten-… --emsdk-path <emsdk>` builds and runs the suite with it.
  "No browser here" is not a reason to review wasm code by reading.
  Once actually built, a wasm threads preset reproduced a per-worker stack limit a review had until then only argued from a standalone probe.
- **Run what the author's machine could not run, not just compile it.**
  A Linux thread-stack reporter whose own header said it had never run turned up four defects on its first run that no reading had.
- **Land the fixes rather than filing them.** A compile error is work, and it is yours; file the *pattern* if there is one worth naming.
- **Do not propose process changes to prevent a red PR.** A pre-merge CI gate argues against the workflow rather than working in it.

```raw
CI was red anyways. your task here is to fix the compilation as well. no process changes needed. (it is by design)
```

Clearing the compile errors is usually what makes the real findings visible at all.
Five build cycles on one branch exposed a real transfer-drain bug and a missing `tick()` that had every render routine declining forever.
What is worth reporting is what the failures turned out to *be*, which is a much shorter list than "CI is red".

## Ranked by what it costs to get wrong

### 1. API shape

Equal-first with correctness, and the thing most likely to be *permanent*: a bug is fixed in an hour, a type that carves the problem at the wrong joint outlives several rewrites.

- **Is the API hard to hold wrong?** A value type silently copyable when copying duplicates authoritative state is a **bug**, not a nit.
- **Are the lifetimes in the type?** A bare `u32` scoped to an epoch is a raw pointer with extra steps; prefer a typed handle.
- **Is the guard on the right class?** A lock that protects a lifecycle belongs on whatever owns that lifecycle.
- **Does the abstraction pay for itself?** A "manager" that fixes a layout and hides what its consumer needs is the anti-pattern; a small helper over one thing the caller still owns is the pattern.
- **Which library does this belong in?** "Could live lower" is the common finding.
- **Does a setter read like a getter?** `inherit_home()` became `set_inherit_home(bool)`: the verb says it mutates, the bool makes switching it off expressible.
- **Does a new accessor name an internal of a library still in flux?** Then the public shape should say only the outcome.
  A test drain that needed to wait on the viewer's fallback shader compile first added `frame::fallback_shader_compile()`.
  What landed was `frame::background_work()`, which grows with the internals while its signature stays put.
  **This binds the doc comment as much as the signature.**
  The header, the cheat sheet and the guidelines then listed the three backlogs it settles, and the answer was that exhaustively enumerating internals in public comments is unnecessary.
  What survives from such a list is only the fact a caller could be wrong about — there, that the wait is process-wide despite the `ctx` parameter.
  The same holds for a fix a review lands: an accessor added to reach one internal is a finding against the fix.

Report API shape **in symbols**: signatures, type names, a few lines of call-site code.
Prose about an API is much harder to judge than the API.

### 2. Correctness the type system does not catch

- **Silent wrongness over loud failure.** A misuse that asserts is a footnote; one that renders the wrong thing or corrupts a table is the finding.
- **Load-dependent failure is worse than deterministic failure.** A stale index that works until the table fills ships, then breaks under load.
- **Lifetime and aliasing.** Who keeps what alive, and can an address be reused while something still keys on it.
- **Ordering and epoch discipline**, GPU-side especially: what is still in flight when this mutates.
- **Assert vs `cc::result` vs exception.** Picking the wrong one is a real finding in both directions — see [the error mechanism](#picking-the-wrong-error-mechanism-is-a-real-finding).

### 3. Docs that have gone out of true

The **highest-value-per-effort** category, because it is what the GitHub diff view cannot show: a doc line is only wrong relative to the *final tree*.
**A doc claiming something the branch does not do is a defect, not a nit.**

- **Leftovers from an earlier state of the branch.** A thing added and then removed leaves its cheat-sheet line behind, and the diff shows that line as an addition.
  Always diff the docs against the final tree, never against the PR body.
- **Stale identifiers after a rename**, and **status tables** the change should have moved.
- **A count in prose, once the set grows.** "Two backends", "both codecs", "`frame` framing only" — true when written, false the moment a third member landed.
  The change that falsified them never touched the line.
  When a change adds the Nth member of a set, grep the old cardinality across the subsystem before reading anything else.
  Adding deflate beside zstd and lz4 left four such sentences wrong, three in files the branch itself edited.
- **"The only X" is a count too.** *The only*, *the single exception*, *nothing else* read as emphasis and are arithmetic, and a reader uses them to stop looking.
  Enumerate on the way past — whether you are reading one in the branch or about to write one in the comment.

Verify each doc claim by looking up the symbol, not by reading the sentence.

### 4. Prose style and nits

`dev.py lint shaped` already gates the prose rule.
Drive-by nits are welcome — collect them into one trailing point so they never crowd out the above.

## Settled calls: process

Each of these was decided in a review; apply them as rules.

### Posting is a separate instruction, and nothing else is one

**Never post a review anywhere the author can see it without the maintainer saying to post it, in those words, after reading the text.**
Three things routinely mistaken for that go-ahead: the goal (`--goal pr-comment` says what the artifact *is*), a round answer (approving a draft approves the text), and the absence of an objection.
This binds a review conducted entirely in chat, where no `post` gate stops you; the lighter the process, the more the rule is carrying.
A review once went well and then posted itself — the maintainer had said "goal is pr comment", read nothing, and found the comment on the PR.
A review the maintainer has not seen is a draft whatever its quality.

### A review that lands changes records them as a comment

**The PR description is immutable once the PR is open; what a review changed goes into a PR comment.**
The comment measures the commits against the description: which claims no longer hold and what replaced them.

```raw
the pr description stays immutable and we have a record of development instead. that is more valuable for our purposes than a "clean" pr description
```

This binds a `land-changes` review on our own branch as much as one on someone else's, and the comment still posts only on the go-ahead.

### No attribution in a review comment

**A review comment carries its content and nothing else: no "🤖 Generated with Claude Code" footer, no session link, no sign-off.**
A harness routinely asks for such a footer on PR text; for a review comment it does not apply.
"we keep it professional here and only want a summary of the changes in that comment."

### An entry carries no `context/*` blocks

**An entry introduces what it needs in its own prose; the review tool has no background block to put it in.**
Three collapsed tiers of background once existed, and after more than ten reviews they had never contained what the maintainer needed when they opened one.
What they were for still holds: every entry is read on its own, so a term is introduced where it is first used.

### Commit messages are not evidence

Commit messages are agent-written and unreliable, especially about **provenance**: "review feedback" is not proof a human decided anything.
Never let one close a question — raise it anyway, and say where you saw it.

### Do not hand back work you already did

A review that says "grep for the readers first" was written by someone who already ran that grep.
Say what it found.
The same stance in three variants: telling the author to check what their own header says, quoting their own doc back at them, and restating their TODO as news.
It reads as lecturing however politely the sentence is built.

**The contract-mismatch finding survives this rule**, because there the author's own doc is the evidence — but cite the contract and move on.
A pointer says the same as a block quote, and the block quote is what turns "your code disagrees with your doc" into "let me read your doc to you".

## Settled calls: what we value in a design

### Picking the wrong error mechanism is a real finding

[error-handling.md](../error-handling.md) splits three ways, and **exceptions have a genuine place here** — never review as if this were a no-exceptions codebase.

- `CC_ASSERT` is for **contract violations only**.
  Assertions off in `release-*`, with UB past the failed contract, is accepted; do not file "this is UB in release" for a plain contract assert.
- `cc::result` / `cc::optional` is for **expected failures the immediate caller can act on**.
- **Exceptions are for infrequent failures that must bubble past frames that cannot help** — a device reset, an allocation the subsystem above can recover from.

Look for **an assert on anything from outside the program** — a file, a shader, a device — and for **a fallible operation offering only one surface**.
The house pattern is a `try_*` fallible core plus a thin throwing façade.

### A 64-bit hash is not an identity

Storing only a `u64` hash and skipping the comparison needs extraordinary evidence; the house sizes are `cc::hash128` for identity and `cc::hash256` where cryptographic guarantees are wanted.
A map keyed on the real value is the default and not a cost worth avoiding: `cc::map` short-circuits on the stored hash, so it costs one `operator==` per hash match.
A type with a `hash()` and no `operator==` usually wants the operator over exactly the fields the hash folds, with their agreement stated as the invariant.

### Portability floor: reject the non-portable thing on the dev box

Where a feature exists on the backend we develop against but not on one we intend to ship, we **fail everywhere, loudly, on the dev box**.
[concepts/views.md](../../libs/graphics/shaped-graphics/docs/concepts/views.md) is the precedent.
Storage-buffer offsets take WebGPU's 256-byte floor as a hardcoded rule, with a documented escape hatch for callers who knowingly target only the looser backends.

- **Keep the code paths.** Rejecting at the API door is not deleting the plumbing; later support needs no redesign.
- **Say why, and where** — point at the portability reason in a doc.
- **Refuse by feature, never by target.** "we technically do not refuse by target _ever_, we only refuse by feature level".
  A design option shaped as a per-target refusal is not a candidate; offer the feature-gated form in its place.

**A known issue recorded in a TODO is not an accepted failure mode.**
The TODO settles that the capability is missing; it usually leaves open what happens when someone hits it, and silent wrong output is the wrong answer either way.
Per-permutation samplers let the first permutation claim a register for the whole pipeline, and the TODO recorded it honestly.
Asserting on a *conflicting* claim costs nothing and turns an unexplainable image into a message.

### A design option is priced on the design, never on what is built so far

**What an in-progress implementation happens to support is not an argument for or against a shape.**
Recommending free functions over methods because the checker had no method calls yet was called "a bad habit": "we can postpone or stub if we want to use things that are not implemented yet".
Price each option on the language or API itself, and state the build plan separately: implement, stub behind a marked temporary, or defer.

### "No callers in the repo" is not evidence of dead code

A symbol in an exported header is reachable by consumers this tree does not contain; an unused-looking member there wants its *correctness* checked, not its existence questioned.
Say which of the two you are claiming.
It binds a design review too.
A static-sampler spelling constructed by no library and three tests was recommended for deletion to make a new backend cheaper:

```raw
let's go with B. so we support it fully. the feature is simply not used _yet_. and we dont expect to write WGSL by hand for long.
```

In a young library, deletion is the option to offer, not the one to recommend on that evidence alone.

### Drive-by cleanups are welcome where the PR already is

Cleanups get postponed by prioritization, not policy — fringe APIs carry debt on purpose, and that is a schedule, not ossification.
**Asking to down-pay debt in code the PR is already touching is a good finding**, made concrete with call sites and the replacement.
Asking to clean up code the PR does not touch is not.

### A surface still being filled in is trimmed once it is complete, not before

**When follow-up PRs will add the readers of an API, fields nothing reads yet are not a finding.**
Trimming now guesses which fields the later members want, and the next PR re-adds what the guess dropped.
A denoise front carried everything the planned vendor denoisers need while the two live members read none of it, and the review recommended trimming:

```raw
there are a few other PRs in the pipeline for more denoisers. I would say we keep it this way and trim/reorg once we have the full surface. less speculation
```

The correctness half still applies: a field whose presence *changes the meaning* of another input is worth naming even when kept.

### Improving HLSL-only machinery waits for the SGL port

**SGL is where shaders are going, so growing HLSL-specific infrastructure is deferred rather than done.**
A finding whose fix is "teach the HLSL path what the SGL path already does" is recorded as a TODO beside the port, never prescribed in the PR.

### A local helper is either a duplicate or a recorded gap, and it has to say which

Every codec, parser and backend grows small private helpers written without checking whether `cc` already has them.
The cost is not the lines; it is that the local copy is a *slightly different* function under the same name, and the difference surfaces on an input nobody tested.
Two image codecs each defined an `is_space` that missed `\f` and `\v`, so a header separated by a vertical tab parsed as one glued token.
The same file hand-rolled 34 lines of byte-order float load that `cc::load_bytes_le` already covered.

Sort each helper into one of three:

- **It exists in `cc` (or `tg`)** — delete the copy.
- **It does not exist and belongs there** — record it in the library's `docs/lower-library-gaps.md`, and leave the copy with a one-line comment saying `cc` has no equivalent yet.
  That comment stops the next reviewer re-raising it and the next author writing a fifth copy.
- **It is genuinely specific to this format** — leave it.

**Grep the foundational library for what the helper does, not just its name.**
`float_from_bytes` finds nothing; `load_bytes` finds the header that made it unnecessary.

**A recorded gap's defects are recorded against the gap, not fixed in the copy.**
A hand-rolled child-process runner marked temporary until clean-core has one had a SIGPIPE, a close-on-exec race and a pipe leak.
Those became requirements on the future `cc` abstraction, not in-place fixes.

### A guarantee only the old implementation gave is not a regression

When a rewrite stops doing something the old code happened to do, ask whether the API ever promised it.
If only a sentence promised it, the defects are that sentence and the callers that leaned on it.
nexus once ran every `main_thread` test in a serial phase; when they became ordinary nodes, "a blocking `main_thread` body runs other tests nested on its stack" was filed as a bug.

```raw
interleaving and nesting TESTs via this mechanism is completely fair. I'd say the old way nexus did it is simply wrong. if you want one-at-a-time, you need exclusive, NOT main_thread. [...] the fix though, is simple: DO NOT BLOCK.
```

Re-read that way it became three real findings: a doc still promising serialization, tests relying on it for exclusion they never asked for, and a stale pin nobody rechecked.
**The tell is a recommendation that restores old behaviour.**
Name the flag or type whose contract covers it first; when none does, look for the callers that need a contract they never stated.

### A one-for-one migration keeps the old idiom's breadth, and that is where to look

A mechanical translation — blocking into awaiting, one API into its successor — makes each site inherit what the old spelling had to do, not what the site needs.
`block_until_idle(); try_get_data()` became `co_await idle_completion(); try_get_data()` in 144 places, when a readback needs only its own bytes and the future's completion already signals them.
For each translated idiom, ask what the site actually needs and whether the new API has a narrower spelling for it.
The tell is a new line that reads as ceremony around the value the site wanted.

### An amortised pass under a lock is profiled, and says what it can cost

Amortised O(1) is an average, and the pass that pays for it is O(n) in one go.
Under a lock other threads take on a hot path, one large instance stalls them all, which a frame shows as a stutter nothing attributes.
A review that recommends or finds one asks for a `CC_RECORD_SCOPE_IF` gated on size, and a comment saying the pass can stutter in pathological cases.
The algorithm stays simple; its worst case becomes visible rather than solved.

### A new link on the async ambient chain is a tax; carry state on what the path already found

Pushing a scope allocates a link, and every node under it retains the chain.
So a design that needs a flag or a sink to reach work on any worker first asks whether an object the path already looks up can carry it.
An async fuzz CHECK firing on any worker was first caught by a capture tag pushed around every step.
It settled on one atomic pointer on `test_context`, read right after the `current_context()` lookup every check already makes: a relaxed load per check, nothing per step.
The same review showed why **intercepting beats undoing**: a restore cannot take back the log line each failure writes, and a divert read before it happens has nothing to undo.

### Out-of-order execution invalidates every collapsed maximum

When a change lets work complete out of submission order, find every place that folds a set of values into one number and waits on it.
A fence signaled to "the highest value finished so far" is correct only while completion order matches submission order, a premise that lives in a comment nobody rechecks.
**The fix is to split the signal, never to serialize it** — one timeline per ordering family — since a watermark reintroduces the head-of-line blocking the change existed to remove.

## Settled calls: where defects hide

### The `#ifdef` arm this machine does not compile is where the defect is

The unbuilt arm is invisible to every check the author can run, and a mistake there survives all of it.
When a change adds or edits a platform arm, **read the arm this machine cannot build, symbol by symbol.**
A two-arm `make_dxc_compiler` had its Windows arm return `make_dxc_compiler()` — itself — beside a correct `#else`, and `check` was green on every preset it runs.
A change that makes a single-platform library cross-platform doubles the number of arms nobody local compiles, and its PR body will name about half of them.

### A gap the author names is where to look, and often where to defer

**The layer a PR body flags as unexercised is where the defects are**, so go there first.
A body that said "the local page has never been driven by a human" had every UI defect there, while the engine it argued for at length held up.

**But an acknowledged gap is frequently deliberate, and closing it is not automatically right.**
Perfectionism kills velocity, and shipping incomplete *on purpose* is the author's call.
So the finding is never "this is untested"; it is **what is actually broken there**, with an ask whose options include **document and defer** alongside fixing now.
A review that only ever offers "fix it" pushes toward a completeness nobody asked for, and the deferral then happens silently.

### A finding the diff already documents is not a finding

**Search the branch's own docs for your finding before you raise it** — `docs/TODO.md`, a "Not yet" section, the `///` on the function.
Raising a recorded gap as a discovery says the reviewer did not read the docs in the diff, and silently overrules a recorded decision to defer without arguing against it.
**A test counts as recording it**, and so does a spec rule or a comment in a test generator.
Three of one review's nine findings were already recorded — one TODO, one doc comment, one CHECK in a test nowhere near the code it pins.

Two honest moves: **drop it**, or **raise it as a disagreement with the recorded judgement**, quoting what they wrote and saying why the branch should not ship with it.

**The same binds a finding about the review tool**, harder, because being inside the tool feels like having read its docs.
A tooling entry recommended a per-block opt-out from path resolution that already existed as `raw`, documented under its own heading; the reviewer had stopped reading one section above it.
Read the tool's docs end to end before filing against it.

### A named owner is a claim to verify, not a fact to accept

When a change introduces one owner for an invariant, check that **every participant routes through it**.
The header says who owns it; only the call sites say whether anyone bypassed them, and a participant handed a *copy* of the owned thing type-checks and reads as sharing.
A resource manager documented as sole owner of the bindless arrays had four consumers each handed their own array over the same binding.
Its `_record` was never reached, and every trace declared the table empty.

### A derived artifact's cache key must cover everything that varies it

Whenever a change caches something *generated*, enumerate every input to the generator and check each is in the key.
The missed ones are passed as options rather than data: a config struct, an entry-point name, an include path.
A material-shader key covered the resolution's shape and nothing about bindless table counts, so non-default budgets generated a shader declaring the default array sizes.
**The same binds a fix the review recommends**: a payload's hash covers its bytes, while the cached thing usually copies more — list what it copies, field by field.

### Adding a member behind a seam means re-reading the seam's callers

The written contract covers the members that exist, and a caller is free to lean on a property all of them happen to share.
The Nth member then satisfies the written contract and not the unwritten one.
List the seam's callers and read each for such an assumption — the tell is a comment at the call site explaining why the call is safe.
`declared_size` came off the frame header for zstd and lz4; gzip declares it in the trailer, so a stream probe read four bytes of payload as a length and reserved up to 4 GB.
The generalization worth keeping: **trailer metadata is a design smell for anything that cannot assume bounded frames**, so the answer was "no streaming size hint for deflate", not a cleverer probe.

### A flake seen during a review is chased before it is deferred

**A failure seen while validating a branch gets a reasonable amount of chasing, whatever code it is in.**

```raw
we dont want flakes usually. so whenever we see one, we spend some reasonable time chasing it. only afterwards do we decide if we postpone it or not. so chase it for now
```

One debug-layer warning in an untouched test took a temporary print and a repeated run to explain: a log truncated at a recording chunk's end no longer contained the substring the allowlist matched.
"It passed 20 of 20 in isolation" reaches none of that.

### A change that touches an example is reviewed by looking at the example

**Put the example's source and every image it produces in front of the maintainer**, as an [example-showcase](../../tools/review/docs/entry-types/example-showcase.md) entry.
This applies to every changeset touching an example, a capture sidecar or a reference image.
The only exemption is a touch that could not change what the example shows — a rename, a formatting sweep, a bulk include fix.
`Bin 0 -> 31695 bytes` is a picture nobody saw, and a run that neither crashes nor asserts routinely shows nothing worth looking at.
A committed reference image showed an imgui panel 110 pixels wide with sentences wrapping mid-word — one missing `SetNextWindowSize`.
It had been invisible for as long as the example existed, because the developer's own saved layout was what they had seen ever since.
**Open the image; do not infer it from the code.**
Findings from an image live in that entry, and **offer the deferral**: an imperfect example is not a reason to hold a change.

### A test's comment is a claim about the test, checked the way a doc claim is

Read what the test asserts, then read what its comment says it asserts.
A comment that overstates is worse than none: it tells the next reader the property is covered, so nobody covers it.
A headless-present test said a swapchain handing out the same image every frame would fail; the loop read back the view it had just cleared, so index 0 forever passed every check.

### A doc's statement of its own limits is a claim, not a boundary

A design doc's "what this does not address" is written early, about a case deliberately not built, and nothing rechecks it.
Accepting it is worse than a wrong finding, because it becomes a constraint the review scopes its own advice around.
A doc said bindless tables "need a register space per table, and nothing here assigns one"; two greps showed the pass already emitted exactly those bytes.
**The tell is a limitation stated in the design's vocabulary rather than in terms of the blocked thing.**

### A backend finding is shown as the library code that breaks

**When a backend violates its library's contract, open with the caller's code that goes wrong, not the backend's internals.**
An entry written in stage masks, encoder scopes and hazard-tracking modes got this back:

```raw
sorry i dont understand anything the way this entry is written. please make a sg code example where you think our metal backend does not adhere to the sg contract
```

The rewrite was seven lines of `sg` — two draws sharing a read-write buffer — the contract sentence, and one bullet per backend; the same shape also made the fix clearer.

### A review of code nobody here can run says so, and hands verification to the author

Everything such a review finds is read off the source, and the comment must say so rather than wear the register of a verified finding.
Frame the whole comment as inferred from source once, at the top, and make each item reverify → fix → test.
Mark the ones hinging on runtime ordering or vendor API behaviour as plausible, and keep them.
Most end in a missing test, which is worth having even when the finding is wrong.

## Settled calls: verification discipline

### A mechanism claim needs the line that proves it

**When a finding turns on "X is derived from Y", read the line that derives it.**
A plausible mechanism assembled from two things that look related survives review, gets agreed to, and produces a fix for a bug that was never there.
One finding claimed an instance id came from an evicting pool, so eviction would restart a converged image; it came from an append-only vector nothing evicts, and the fix had already been approved.
Grep the constructor of the value, not the type that looks like it owns it.

The shapes this takes, each seen at least once:

- **A recommendation is a mechanism claim too.** A fix assuming a static driver-to-child link was unbuildable, because the link is discovered at runtime.
  The draft had even written "I have not checked how…" beside it, and a sentence like that is the check, left undone.
- **A proposed assert is checked against what the code declares today.** Every bound resource was declared read-write, whatever its binding allowed.
  So an assert on "declared with a fragment-stage write" would have fired on every frame's font texture.
- **"This guard becomes unreachable" is a claim about every path into it**, including shutdown's early returns.
- **A member of the right type is not the mechanism wired.** A stream system holding a `transfer_drain` never called `notify_on_drained`; grep for the connecting call, not the member.
- **A fix names inputs, and each has to be reachable where the fix goes** — not a context a handler captures nothing of on purpose, not a completion hidden in a `unique_function`.
- **So is the price of an option you did not recommend.** "Make the executor resumable" was priced as much larger without the executor being read.
  It was a return in one driver's "no progress" branch, and the inflated price is what made the recommended option look cheap.
- **"Nothing has an effect" is a claim about every write the language can make.** Buffer stores had landed on the same branch, and checking found a real ordering bug.
- **A fix that replaces a limit owes everything the limit was protecting.** A depth limit's doc said "cycles"; the commit that set it said "a pool worker's 512 KiB stack under a sanitizer".
  Read the commit that set the value.
- **A generated symbol is checked against this target's generator input**, not a sibling target that happens to have the symbols.
- **Beware two mechanisms with similar names.** A cache-key claim was true of the DXC compile key and false of the slib asset key, and named neither.
- **A prescribed drain, wait or guard is a claim about what is outstanding.** "2 async items leaked, settle `background_work`" — the items were stream uploads `background_work` does not cover.
  Name what leaked before naming the remedy.
- **Look for the configuration that makes the race deterministic before writing the item.** A `singlethreaded-*` preset removes exactly the concurrency a one-run finding depends on.
  Two minutes there convert "I saw it once" into a named mechanism.

### Check whether the code already does the thing you are asking for

A finding that asks for a check or a guard is a claim that it is absent, and it is the check most often skipped because the request came from reasoning about what *should* exist.
An ask that a generated `bind<G>` "check the group index against the layout" was already satisfied by an assert in both backends.
The item needed to say the templated version must keep passing the index.
**The tell is an instruction phrased as "make X check Y."** Grep the two or three places it would have to live.

### A count is a claim, and it is checked by enumerating

**The failure is counting from a grep's output rather than from the set the claim is about** — comments, files outside the population, the same file twice.
One draft carried three wrong counts, and the argument each supported was correct all three times; the finding survives, the credibility does not.
**Either enumerate the set in the comment, or drop the number.**
A list of nine paths is checkable and self-correcting while you write it; where a count really is the point, produce it with a command whose output you paste.

### Verbatim means pasted

A quoted `///` or doc line is a claim about a file, checked the way a line number is.
Retyping from memory truncates, moves a comma, or merges two lines into a sentence the file does not contain — and the truncation lands exactly where the quote stopped supporting the point.
Paste it, keep its line breaks, and cite the line the paste starts at.

- **Take line numbers from a read of that one file.** `cat -n` over several files numbers them as one stream; a draft cited lines 293–299 of a 115-line file.
- **Cite where it is actually from.** A sentence introduced as "Q14 measured it" was the author's own doc; the attribution borrowed an authority that was never lent.
- **One phrase cited against two files is two quotes**, each with its own line, or the author searches one file for text it does not contain.

### A rename's call-site list comes from a grep of the name

Listing the sites you happened to read is not listing the sites; the missed ones are systematically the ones a reviewer does not visit — a `///` family list, a prose mention, a retired-gap entry.
Five places named for one rename missed four, every one a plain text match.
**The same holds for any change to a symbol** — removing a table row, changing a constructor, replacing an overload, renaming a CMake target.
Each owes the list of tests, fakes, corpus cases and docs that reference it.
**Removing a default argument owes the prose that explains the default**, which a grep for "default" finds and a grep for the call does not.
An instruction the author has to complete themselves is not an instruction.

### An instruction to "both halves" is checked against each half separately

Where a design implements one thing twice — a runtime parser and a build-time generator — an instruction written once assumes the halves have the same machinery.
They rarely do, and the difference is what the author discovers halfway through.
"Pin offsets in both halves" assumed the C++ parser computes them; it computed only source offsets, so the instruction was new layout code rather than a corpus change.
Open each half and find the code the instruction would extend.

### Items in one comment are checked against each other

An instruction's target wording in one section can undo another: one item let non-address `[[vk::…]]` attributes through, four items later another documented them as invalid.
**A test one item asks for is checked against the fixes the other items ask for** — a trace test would have raced a compile another item introduced.
Read the draft once for exactly this before handing it over.

### A bound is checked against every path the code dispatches to

A limit derived from the input is worked out against the path just read, and a second path with a different expansion factor rejects valid input.
An allocation ceiling correct for adaptive RLE would have rejected legal old-format files whose flat path expands 4 bytes into 255 pixels.
The honest conclusion — no linear bound exists, use a sane constant — superseded a recommendation the maintainer had already approved.
**Catching this after approval is normal; that is what the adversarial pass is for.**

### A summary of what the review landed is checked against its own commits

A `land-changes` comment describes commits the reviewer just wrote, and that is where a summary drifts most.
The summary is written from what the fix was meant to do, and the commit does slightly less or slightly else.
One draft said a fix counted "a callee's asserts" when it counted every assert of the run, and said the second of two expectations "always" saw `passed` when that held only if the first was met.
It also said a doc "no longer" described something that three of its lines still partly did.
**Read the diff of each commit while writing its bullet, and name every hunk a reader will see.**
A sort comparator, a `nan` spelling or a nested-test case left out of the comment is a hunk the author cannot account for.

### A second citation is a second claim

A finding propped on a supporting citation is only as strong as the weaker one, and reaching for a second reference because the first felt thin doubles what a reader can check.
A correct doc-rot finding added that another file "marks all five phases `[done]`"; that file had no phase rows, and it was the only false claim in the item.
If a finding does not stand on one verified line, re-derive it rather than buttress it.

### A measurement of a failing system says nothing about why it is failing

A routine chain declined every frame; 300 frames ran in 267 ms, "far too fast for a shader compile, therefore stuck".
The frames were fast **because** they declined, and a 50 ms sleep per frame made it pass — a latency race.
**Ask what the measurement would look like under the other hypothesis.** Where the answer is "the same", it is not evidence.

### Measure the code, not a model of it

**A number taken from a re-implementation is a claim about the re-implementation**, and for a precision finding the rounding difference is the whole subject.
A numpy float32 model showed thin quadrics "dissolving" at distance; run against `sv::intersect` itself, every ray aimed outside the sphere reported a hit — the silhouette inflates.
The cause and fix were right, the symptom was wrong, and that is worth stating rather than smoothing over, because an author who checks the symptom first discards the item.
Before modelling, ask whether the thing under test is already callable; where it is, the model is strictly worse evidence.

### A UB claim is checked against the standard the repo compiles as

C++20 made signed left shift modular, so `255 << 24` is well-defined here and negative.
"This is UB" filed on a pre-C++20 reflex gets discarded, and it hid the real bug: the negative value drove an index below the buffer, an out-of-bounds write worse than the overflow claimed.
Say which construct is undefined and why, and check the version — shifts, signed overflow, `char` signedness and aggregate init all changed.

## Feed the adversarial pass back into this document

The `reviewing-a-pr` skill has a subagent read the drafted comment against the branch with none of the review's context, asking per item whether it could be implemented as written.
**Its findings are not all about that comment.**
What is wrong with *this* comment goes into the next round; what names a habit belongs **here**, as a rule with its case, in the same session.
A pass that produces only per-item corrections has probably been read too narrowly.

## Asking good questions

Questions are how this document grows, so they have to be answerable without the reviewer re-deriving the context.

- **Always name where you saw it** — file, line, the actual code.
- **Give the tradeoff, both sides**, and what it would cost to change.
- **Say what you would do**, so a one-word answer is possible.
- **Keep them few and separate** from the findings, at the end.

## Open questions

- Where is the line between a design question worth raising and second-guessing a design the author already thought through?
  A partial answer: when a redesign is already the author's planned next step, the review's job is to keep this PR from cementing the old shape, not to specify the new one.
- How much of a proposed redesign belongs in the review comment versus a linked issue, once a finding turns into a multi-type API change?

## Reference

- [CLAUDE.md](../../CLAUDE.md) — hard rules, layering, and the style preferences a review assumes.
- [docs/coding-guidelines.md](../coding-guidelines.md) — design conventions and the prose rule.
- [docs/error-handling.md](../error-handling.md) — the assert / `cc::result` / exception split a review applies.
- [docs/guides/prose.md](prose.md) — the lint workflow behind the prose findings.
- the `reviewing-a-pr` skill — the flow and the entry structure.
- [tools/review/readme.md](../../tools/review/readme.md) — the tool the flow drives.
