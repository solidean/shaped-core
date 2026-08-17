# Decisions

Every settled decision behind `vdoc`, with its reasoning and what would reopen it.

This file exists so a decision is argued **once**.
If a change would contradict an entry here, the entry is what to argue with — not the code, and not the person who last touched it.
Reopening one is entirely allowed; doing it by accident is not.

Each entry states what was decided, why, and the condition under which it should be revisited.
"Reopen when" being *nothing* means the decision is structural: it cannot be changed without redesigning the library.

---

## Scope and shape

### The library ships zero components

**Decided.** `vdoc` defines the storage model, the merge semantics and the interpretation machinery, and no component types at all.

The set of components is what makes a document *an application's* document.
A library that shipped a `transform` would be shipping an opinion about what a document is for, and would immediately be wrong for the next application.
What the library does own is the machinery — traits, registry, versioning, deletion — so every application's components get the same guarantees.

**Reopen when:** nothing — a built-in component set is the specific failure mode this design exists to avoid.

### Two libraries, not one

**Decided.** `versioned-document` depends on clean-core alone; `versioned-document-file` adds persistence and depends on babel-serializer.

The split is forced by dependencies and right on its own merits.
An application that builds documents in memory — a test, a generator, a converter, a server-side merger — must not link a database engine to do it.

**Reopen when:** persistence stops needing an engine, which is not foreseeable.

### The name

**Decided.** `versioned-document`, namespace `vdoc`; `versioned-document-file`, namespace `vdoc::file`.

It sits in the same tier as `babel-serializer`: a substantial library with a domain, not foundational vocabulary.
Two-character namespaces are reserved for the libraries whose names appear on nearly every line of shaped-core code (`cc`, `tg`, `nx`, `sg`), and this is not one of them.

**Reopen when:** nothing worth the churn.

### The full design is built in v1

**Decided.** Every layer described in [the concepts](_index.md#concepts) is implemented, including merges, multi-values, snapshots, pruning and verification.

**Partial implementation is never a licence to simplify the design.**
"Nothing uses it yet" is not a reason to cut a layer, collapse a seam, or skip a case — it is a statement about the calendar, not about the design.
A design cut down while it is half-built is not a smaller design; it is a broken one whose breakage surfaces a year later, in the shape of a format that cannot express what it was designed to express.

This was a standing instruction to everyone implementing the plan, and it is why the whole design was described before any of it was written.

**Reopen when:** nothing.

### The design lives in `docs/concepts/`, one file per question, and the plan folder is gone

**Decided at the end of milestone 6.** `concept.md` became ten files under `docs/concepts/`, indexed from [the hub](_index.md#concepts); `docs/todo/` and `structure.md` were deleted.

`concept.md` was ~550 lines and was the document everyone was told to read first.
At that size "read this before starting" stops being an instruction anyone follows, and a reader who wanted to know how multi-values resolve had to find it.

**What replaces it is an index, not a summary.**
A summary is a second place for the design to live, and the second place is the one that goes stale.

The plan folder went because a plan describes something being built, so it stops being true the moment the thing is built — and scaffolding left standing is read as part of the building.
The same argument retired `structure.md`, which tracked `[done]` versus `[planned]` per piece: a status tracker whose every entry has become `[done]` is a file that can only ever be wrong again.

**Splitting is also where three drifted claims were caught**, which is the argument for reconciling during a move rather than after one.
Moving a section is the moment it is actually reread.

**Reopen when:** a concept file grows past the size at which it stops being read, which is the same failure this split fixed.

---

## Storage model

### Entity ids are strings

**Decided.** An `entity_id` is an arbitrary application-chosen string — no structure, no required uniqueness scheme, no built-in generation.

An application that wants collision-free keys puts a uuid in the string.
An application that wants a well-known entity it can address directly uses a name, and gets a quasi-singleton for free.
Both are legitimate, and a library-imposed 128-bit key would take the second away while giving the first nothing it cannot already have.

Strings also make documents readable in a dump and writable by hand, which matters more than it sounds like for a test-driven library.

**Reopen when:** interning turns out not to make id comparison cheap enough for the query paths, which would be a clean-core problem before it was a `vdoc` problem.

### Multi-values are coarse: whole property, never sub-parts

**Decided.** A property is multi-valued or it is not, and parts *within* a value never conflict independently.

Sub-value merging would require the storage layer to understand value structure, which is the exact knowledge this design keeps out of it.
It also produces states nobody authored — one writer's `x` with another writer's `y` — and calls that a resolution.

**Reopen when:** nothing — this is the granularity the whole conflict model is built on.

### Equal concurrent writes are still structurally multi-valued

**Decided.** Two concurrent writers of byte-identical values leave the property with two entries in the raw document.
The interpretation layer collapses it silently and records it in the agreed-multi-values side channel.

Storage records what happened, and what happened is two independent writes.
Collapsing at write time would mean the storage layer comparing values and deciding they are "the same edit", which is interpretation.

This case is common, easy to get wrong, and carries its own tests.

**Reopen when:** nothing.

### There is no delete

**Decided.** Deletion is a `$alive` property read by the interpretation layer, never removal of stored data.

Removing data would break immutability, synchronization and history — the three things the format exists to provide.

Contested `$alive` keeps the thing alive, because resurrecting is recoverable and vanishing is not.

An **abstention** does not weaken this: it removes a *write*, never a *thing*.
The op that abstained is still in the history, still hashed, and still says what it did — what stops being there is one property's value, which is the same kind of statement as overwriting it.

**Reopen when:** nothing.

### Ops and blobs stay in separate tables

**Decided.** The file keeps ops, snapshots, assets and blobs as distinct tables.
They are **not** unified into a single content-addressed object store.

A git-style object model is conceptually tidy and practically wrong here: ops are small, numerous and read in their entirety at open, while blobs are enormous and read lazily in pieces.
One table for both would mean one set of page-cache, locking and chunking tradeoffs serving two access patterns that share nothing.

**Reopen when:** the two access patterns converge, which would mean the format is being used for something else.

---

## Values

### The value codec is binary; JSON is a display metaphor

**Decided.** Values are canonically-encoded bytes with a tag byte and a payload.
The library can print a value as JSON-ish text for a human; that is one-way.

Because JSON round-tripping is explicitly not required, an entire category of problem disappears.
`NaN` payloads, the two zeroes, shortest-form integers, number-precision loss, key-order stability in a text format.
None of them is a concern, and none of them needs a rule.

Float canonicalization is the application's business.
An application that wants `-0.0` normalized normalizes before writing; the library will not rewrite a caller's number behind their back.

**Reopen when:** nothing.

### Equality and hashing are defined on bytes

**Decided.** Two values are equal exactly when their bytes are equal, and decoding rejects non-canonical encodings so that this stays true.

Diffing, content-addressing and the agreed-writers check are all memcmps because of this.
Accepting a non-canonical encoding "leniently" would silently break all three.

**Reopen when:** nothing.

### The codec starts in `vdoc`, not in clean-core

**Decided.** `value` lives in `vdoc`. It is not promoted to a `cc::` type now.

There is far too much design space here to one-shot a foundational version: tagging, growth, views, arenas, schema hints, streaming.
A `cc::value` designed for one speculative second user would be worse for both.

**Reopen when:** a genuine second user appears.
Then promote it, informed by two real sets of requirements instead of one and a guess.

**And be honest about what that would cost.**
A general-purpose any-value format would very likely not be byte-compatible with this one.
So replacing `vdoc::value` would be a **breaking change to the `.vdoc` format**, not a refactor.
Whether we would ship a migration is genuinely undecided: shaped-core is in a "can still break" mode for the foreseeable future, and nothing here promises otherwise.

What we do keep is the *option*.
The file carries a `user_version` ([format.md](../../versioned-document-file/docs/format.md#identification-and-versioning)).
A future build can therefore tell an old encoding from a new one, and migrate in principle.
That is a cheap door left open, not a compatibility guarantee — do not design anything on the assumption that today's value bytes will still be readable.

### No hash or reference type in the codec

**Decided.** Asset references are ordinary strings.

A file is only one source of assets; built-in, procedural, remote and cached assets resolve through completely different machinery, and a string is the only identifier all of them can share.
A dedicated reference type would imply the document knows how references resolve, which is precisely the coupling being avoided.

**Reopen when:** nothing foreseeable.

### A length prefix counts the bytes that follow it

**Decided.** In all four length-prefixed kinds the `u32` prefix counts everything after itself.
For `string` and `bytes` that is the data; for `array` and `object` it is the `u32` element count plus the entries.

[values](concepts/values.md) left this ambiguous by saying "payload byte length" without fixing where the payload starts.
Both readings give an O(1) skip, so the tiebreak is elsewhere.
One meaning across all four kinds makes skipping a single rule — `5 + prefix`, whatever the tag — instead of two rules a reader has to remember apart.
A container payload shorter than 4 is therefore invalid by construction: it could not hold its own count.

**Reopen when:** never.
This is on disk the moment a file exists.

### Decoding does not validate UTF-8

**Decided.** `try_decode` enforces structure, not text.
A `string` whose bytes are not valid UTF-8 decodes.

The canonicality list this format needs is the one that keeps byte equality meaningful: sorted and unique object keys, a boolean of 0 or 1, exact length prefixes, no trailing bytes, bounded depth.
UTF-8 validity is in none of it — two values with equal bytes are equal whether or not those bytes are text.

Adding the check would be a durable format rule that can never be relaxed, and it would cost a scan of every string byte on every decode.
It would also give `string` and `bytes` different trust levels, for no gain the merge layer can use.
What survives is the intent: `string` means the application means these bytes as text, and validating that is the application's job, exactly like float normalization.

**Reopen when:** something below the application layer starts interpreting string contents.
Nothing in the design does.

### The nesting limit is 64, and it is a format constant

**Decided.** `value_view::max_depth` is 64, counting the value itself as level 1.

The limit exists so a corrupt or hostile input cannot drive the decoder into unbounded recursion, which means it has to be a fixed number rather than a budget.
64 is far past anything a real component writes: a transform is two levels, a material with nested layers maybe four.
It is also shallow enough that the decoder's recursion is trivially bounded, so the decoder stays recursive and stays checkable line by line against this document.

**Reopen when:** never downward, since that would reject existing files.
Raising it is a format change like any other.

### `value_builder` has a fallible build

**Decided.** `try_build()` returns `cc::result<value, value_build_error>`; `build()` asserts on the same conditions.

The original plan said only that the builder "rejects a duplicate key", which an assert would also satisfy.
An assert is wrong for the case that actually happens: an importer feeding externally-sourced key/value pairs straight into a builder.
There a duplicate key is input to reject, not a bug to abort on.
So the fallible form is the primitive and the asserting one is the convenience.

Depth is deliberately *not* a build error.
The builder cannot compose past `max_depth` without the decoder saying so, and a `CC_ASSERT`-gated re-decode inside `try_build` catches it in assert-enabled builds.
That keeps an O(n·depth) walk off the release path, for a mistake no real component set can make.

**Reopen when:** a caller needs the depth failure as a recoverable error rather than as a bug.

---

## Ops and the DAG

### The op retains its bytes and decodes on demand

**Decided.** An `op` holds `op_id`, its parents, and an `optional<op_payload>` of the producer's `metadata_bytes` and `assignment_bytes`.
`metadata()` and `assignments()` are decoded views over those bytes; nothing decoded is stored.

The original plan sketched an op carrying decoded `metadata` and `assignments` *plus* an optional payload.
That is a fourth copy of state already present three times.
It also corresponds to nothing storage hands back.
[format.md](../../versioned-document-file/docs/format.md#ops--the-dag) stores the metadata and the assignments as two separate blobs, with no single payload column anywhere.
The combined form would therefore have to be synthesized, which is re-serialization wearing a different hat.

Inverting it makes [the no-re-serialization rule](concepts/ops-and-content-addressing.md#the-op-is-its-bytes) structural rather than remembered.
An op holding only decoded assignments would leave verification hashing `encode(decode(bytes))`.
A future change to a formatter, an integer width or an ordering would then turn every good stored op in the wild into a mismatch, and a mismatch is indistinguishable from tampering.
With the bytes retained there is no encoder near a loaded op for such a change to reach.

It also buys the properties compatibility rests on.
An op is storable and relayable without being interpretable, write-back is lossless by construction, and an op assigning to unknown component types round-trips byte-identically.

`optional` then means exactly one thing — the op was pruned — instead of doubling as "we did not keep the bytes".

**Reopen when:** never for the verification rule.
The *representation* could change — an arena shared across an `op_graph` rather than per-op buffers — as long as the bytes are what is retained and no encoder is reachable from a load.

### `op_id` orders by its canonical 32 bytes

**Decided.** `op_id::compare_bytes` — a memcmp over the 32 canonical bytes — is the single ordering used for the parent sort, `raw_property` ordering, and the parse policy's "smallest op id wins".

`cc::hash256` has a defaulted `<=>` that orders four `u64` limbs, each assembled little-endian from the byte sequence.
It is total and run-stable, so it looks usable, and it is *not* the order of the 32 bytes.

The parent sort feeds the hash preimage.
A third-party reader implementing the format from [ops and content addressing](concepts/ops-and-content-addressing.md#the-producer-canonicalizes-the-hash-just-hashes-bytes) would sort by bytes.
It would produce a different parent order, and compute a different `op_id` for identical content.
Once files exist that break cannot be fixed by either side.

The same argument covers ids: [`cc::interned_string`](../../../base/clean-core/src/clean-core/string/interned_string.hh) deliberately has no `operator<`, and only `compare_bytes` is reproducible.
`compare_identity` is a pointer order that differs every run, and must not be reachable from anything that reaches output.

Byte order here means **unsigned** byte order.
`cc::string_view::compare` used to widen `char`, which is signed on our platforms, so any id byte >= 0x80 sorted ahead of every ASCII one.
Milestone 1 met this on object keys and worked around it locally; milestone 2 met it again on ids, where the sort feeds the op hash, and it was fixed in clean-core instead.
A workaround in the caller is the wrong answer for an ordering that a wire format is defined in terms of.

**Reopen when:** nothing.
This is a wire-format property.

### A multi-valued property always differs

**Decided.** `op_builder::build` emits an assignment for a path with two or more surviving writers, even when every one of them holds byte-identical bytes.

The diff exists to skip writes that would change nothing, and a multi-valued path is not a value that could equal the desired one — it is two independent writes.

It is also the only way a conflict is ever resolved through the normal edit path.
[multi-values](concepts/multi-values.md) says a later op that writes the path resolves it back to a single value, and that later op is this one.
A user who sees `10` and sets `10` must be able to collapse it.
The agreed-multi-value side channel exists precisely as a tidy-up hint for this write, and diffing it away would make the channel dead.

Concluding "both writers said 10, so the current value is 10" is the collapse [the interpretation layer](concepts/multi-values.md#writers-that-agree-still-conflict-structurally) owns.
Making the storage layer's diff depend on writers agreeing would be the same category error as sub-value merging.

Idempotency survives: the emitted op collapses the path, so a second `build` against it sees one equal writer and emits nothing.

**Reopen when:** nothing at the storage layer.
A higher layer may of course offer "resolve only if they disagree" as a user-facing choice.

### Dominance is resolved by propagating a superseded set, not by ancestor queries

**Decided.** Materialization walks in topological order carrying two writer sets per path — `surviving` and `superseded`.
Applying op X to path p does `superseded |= surviving`, then `surviving = {X}`.
Merging parents unions both sets from every side, then drops from `surviving` anything in the combined `superseded`.

The obvious alternative is a global ancestor test — "is any other writer of p a descendant of this one".
Doing that exactly on a DAG needs reachability bitsets at O(n²) memory, or a chain decomposition or 2-hop labelling whose worst case is no better than a walk.
An interval label is an exact ancestor test only on a *tree*, which is the trap that makes this look cheaper than it is.

The superseded set avoids the question entirely, because each parent's ancestor set is ancestor-closed.
A dominating pair therefore always lands wholly inside one parent, so a domination fact that exists globally always exists locally in some branch.
That is what makes the local union sound for nested diamonds, criss-cross merges and n-ary merges alike.

`superseded` is monotone and may only be unioned forward.
Dropping an entry because no side currently lists it as surviving resurrects a dominated writer, and that is the failure this design is most likely to be broken by later.

It may be dropped *wholesale* at an articulation point, where one live state remains in the sweep.
**That justification is per-sweep and does not survive being stored.**
The decision below works out what a stored snapshot does instead.

This pass is also the oracle the snapshot cache is checked against, which is a second reason to prefer it.
An oracle has to be correct by inspection, and "correct modulo a chain cover" is not.

**Reopen when:** profiling shows the state copy at merges dominating, which is a question about representation rather than about the rule.

### The sweep's per-path state carries side lists, so nothing walks the whole path set per op

**Decided in milestone 7**, after measuring.

A sweep state is dense by path index, because a path index is what everything else is keyed by.
On any real document almost every slot is empty: an op touches a handful of paths and the document has tens of thousands.

Three passes used to walk all of them anyway, and the third is the one that mattered.
`merge_into` iterated every slot per merge edge, `build_document` iterated every slot once, and the **articulation-point clear iterated every slot per op**.
In a [mostly-linear history](concepts/workloads.md) every op is an articulation point, so that last one is ops × paths.
On the benchmark document that is 8,000 × 48,000 ≈ 384 million iterations, which measured as 1.1 s of the 1.2 s a materialization took.

The irony is that the clear exists to *remove* a quadratic term: without it, N writes to one path accumulate N−1 superseded entries.
It traded a quadratic-in-writes-per-path term for a quadratic-in-ops×paths one, and on real documents the second is far worse.

So the state carries two index lists beside its slots — `occupied` for slots that have been non-empty, `dirty` for slots whose `superseded` has been.
Both are **supersets**: an index goes in when a slot first becomes non-empty and never comes out, because a merge or the clear itself can empty a slot again.
Every consumer re-checks the slot, so a stale index costs one comparison and never a wrong answer.
The lists live inside the state rather than beside it, so a stolen state brings them along and a linear history still copies nothing.

Cost after: **45.8 ms** for the same materialization, down from 1,099 ms.

The failure mode is a missed record, and it is invisible — the sweep skips work it owed and returns a plausible wrong answer.
Two things catch it.
`side_lists_cover` asserts at the end of every sweep that the lists still cover every non-empty slot, with no index listed twice.
A corpus-wide differential test then pins the clear against `drop_superseded_at_articulation_points = false`, which is what that option exists for.

**Reopen when:** a workload makes the dirty list as large as the path set — a document where nearly every path is overwritten between articulation points.
Then the side list is pure overhead and the dense clear was right all along.

### A snapshot stores `surviving` only, and its validity is decided at use

**Decided in milestone 6**, and it reverses what the plan originally specified.

A cached snapshot is exactly a `raw_document` — the surviving writers and nothing else, over bytes it owns.

**Storing `superseded` was designed first, and rejected on size.**
It totals `32 B × (all historical assignments − distinct paths)`, so a snapshot would grow with the very history it exists to replace.
A document of a few million properties that saw fifty million assignments over its life would carry over a gigabyte of superseded ids.
That stands in for a document that fits in a tenth of the space.
The asymptotics are wrong, not merely the constant.

**`surviving(X)` is a function of X's own causal past alone.**
State flows forward through the sweep, so the state at X never depends on anything outside X's ancestry, and the articulation-point clear cannot corrupt it.
So any op may be snapshotted, from any sweep, and there is no eligibility question at creation time.

**Validity moves to use time, and is re-checked against today's DAG.**
The walk terminates at any op the cache holds, which leaves the walked set parent-closed except at terminators.
A snapshot may then be seeded only where that set has **exactly one source** — one op with no parent inside the walk — and that source is the snapshot.
Every non-source has a parent inside the walk, so descending parent edges from anywhere lands on that single source.
That makes the source an ancestor of everything walked, and an ancestor cannot present a stale branch.
It costs nothing, because it is the in-degree computation Kahn already does.

**"Every source is cached" is unsound, and was the first rule tried.**
Materializing `{T, X}` where X is a distant ancestor of T gives two cached sources, and unions their surviving sets into a multi-value nobody wrote.
The mathematically minimal condition is pairwise-incomparable sources, but comparing them is the ancestor query the decision above declines to pay for.
Exactly-one is the cheap sufficient case, and it fits real histories.

A second condition is needed and is easy to miss: **no op other than the seed may have a parent that is in the graph but outside the walk.**
Only a terminated walk can produce one, and such an op would be replayed from a partial set of ancestors.

Everything else falls back to a plain replay, which costs time and never a result.
So correctness never depends on the optimism; only speed does.

**Consequence, stated because it is real behaviour rather than a corner case:**
a merge of two branches that both reach the root has two sources, and replays in full until a snapshot exists at or below the merge base.

**Reopen when:** profiling shows the fallback firing on a workload that is not linear-heavy.
That is a question about the *condition*, not about the representation.

### Seeding a filtered sweep costs the filter's size, not the snapshot's

**Decided in milestone 7.**

A snapshot is a whole `raw_document`, and a filtered sweep used to walk all of it and discard everything outside the filter.
That made a snapshot worthless at exactly the call it exists for — `op_builder::build` materializing one entity — because seeding was O(document) either way.

It now iterates the wanted entities and reaches into the snapshot by binary search, so the cost is `|wanted| · log(entities)`.

The list it iterates is a sorted vector rather than the membership set, because path indices are allocated in iteration order and a hash container's order must not reach the output.
`build_document` sorts at the end, so it could not have leaked — this simply removes the question.

The result is identical either way, so no comparison can tell a lookup from a walk.
`materialize_stats::snapshot_entities_read` reports it instead, and a test asserts it against a 64-entity document filtered to one.

**Reopen when:** a filter routinely names most of the document, at which point the walk was cheaper than the lookups.

### The edit path may reach the snapshot cache

**Decided in milestone 7.** `op_builder::build` gains an overload taking a `snapshot_cache&`.

The entity filter applies to assignments and never to edges — filtering edges would sever ancestry and fabricate multi-values.
So a one-entity diff still *walks* the whole history, and a snapshot is the only thing that shortens it.
Without the overload the edit path structurally could not reach the caching built for exactly this, and every build replayed the entire history.

The two overloads are defined to produce the identical op, always: a cache changes how long the diff takes and never what it produces.
Content addressing makes that checkable by comparing one id, which is total, and the corpus test does it with a snapshot installed at every op in turn.

Cost at 8,000 ops, fifty ops built: **2.0 ms**, down from 1,046 ms.

**Reopen when:** N sequential builds against one snapshot cost materially more than one, which is the case for a burst-oriented API.

### A snapshot may be advanced in place along a single-parent edge

**Decided in milestone 7.**

The question the edit loop asks is "how often should a snapshot be recomputed", and the honest answer turned out to be *never*.
`surviving(child) = surviving(parent)` with the child's assignments overwriting their paths, exactly on a single-parent edge, so a snapshot moves forward for the cost of one op's writes.
A session that advances on every accepted op keeps its head permanently one op behind a snapshot, which is a stronger property than any cadence could buy and costs 3 µs regardless of document size.

Two things had to change for it.

The snapshot arena became a **chunk list**, each chunk reserved once and never grown, because appending to a single buffer would reallocate and dangle every `value_view` at once.
Overwriting therefore strands the old bytes; they are counted, and the snapshot is rebuilt from scratch once dead outweighs live.
That rebuild is the only O(document) event on the path, and it happens a logarithmic number of times per session rather than once per op.

The cache entry is **re-keyed** — removed from the parent, installed at the child, pin and all.
`snapshot_cache::take` exists for that and for nothing else.
What lands at the child genuinely is `surviving(child)`, so "an op id commits to everything behind it" still holds.
What breaks is any `raw_document` borrowing the old entry, which `raw_document.hh` already says a cache modification does.

**It is caller-driven rather than automatic**, and that is the load-bearing part.
During a wide fan the frames are siblings, so advancing onto one would leave every other frame replaying the whole history.
The snapshot stays at the branch point until a frame is accepted as history, and only the application knows when that is — so it is an API, not a policy knob.

**Reopen when:** an advance across a merge is wanted.
It needs the parent's superseded set, which is what the decision above rejected on size, so this would reopen that one first.

### A snapshot's empty superseded is what bounds how far history may be pruned

**Decided in milestone 6.** A `required` snapshot carries no superseded set, exactly like a droppable one.

That is sound only while nothing can present a writer from behind it.
Ops behind a prune boundary are skeletons, and a skeleton carries no assignments, so a branch arriving through one contributes no writer to resurrect.

**But a ref that forked *before* the boundary breaks that**, and this was found by a test rather than by reasoning.
Such a branch keeps its own ancestors, because they are history it still needs.
So it does still offer writers that ops behind the boundary superseded, and merging the two fabricates a multi-value.
Replaying instead is no escape: the ops that would have suppressed it are skeletons by then, so the replay is *lossy* rather than merely slow.
Both paths are wrong, which means the prune itself was the error.

So `store::prune` **refuses unless every ref descends from the prune point**, and names the ref that blocked it.
The boundary a document may prune to is the oldest op every ref still descends from.

**Reopen when:** a workflow genuinely needs to prune past a long-lived divergent ref.
The fix would be to give a required snapshot its superseded ids after all: 32 bytes each, no payload, and computable at prune time while history is still present.
That reintroduces the size question above, bounded this time to one snapshot rather than every one.

### A discarded editing frame is dropped outright, not skeletonized

**Decided in milestone 7.**

Continuous editing emits one op per frame, each branching from the same state, and only the last becomes history — see [workloads](concepts/workloads.md#continuous-editing-goes-wide).
Nothing removed an op before this, so a ten-second drag at 120 fps left 1,200 of them in `_ops` and 1,200 entries in one op's child list, for the rest of the session.
Roughly half a megabyte per ten seconds of dragging, never reclaimed.

It is **only memory**: a sweep walks parent edges and never child ones, so the fan costs nothing in time.
That is why the API is as small as it can be — `drop_leaf`, which asserts if anything descends from the op, plus `leaves()` so a session can sweep rather than track.

`skeletonize` is the wrong tool and looks like the right one.
It exists because ancestry must survive a pruned op; this is for an op with no ancestry to preserve.
Keeping a skeleton per discarded frame would leave the leak in place under a different name.

A scratch `op_graph` for transient frames does not work either: both `build` and `materialize` need the branch point's ancestry, which lives in the real graph.

Forgetting is safe rather than lossy because the DAG is content-addressed — a dropped op that ever comes back is recreated byte-identically by `add`.

**Reopen when:** an undo stack wants to reach a dropped frame.
At that point those frames are history, and the mistake was dropping them rather than the API that allowed it.

### A skeleton op reports unverifiable, and never a hash mismatch

**Decided.** `verify_op` has three outcomes rather than two, and a skeleton gets its own.

A skeleton has no bytes to hash, so it is unverifiable *by construction* — this is not a check that failed, it is a check that could not be run.

Reporting it as a mismatch would be a false alarm about the one thing content addressing exists to detect.
Pruning is a normal, encouraged operation, so a document that has been pruned would raise tampering alarms as a matter of course, and everyone would learn to ignore the alarm that matters.

The load path files a hash mismatch only from a genuine decode failure, and the two outcomes are pinned apart by a test over one document that is both pruned *and* corrupt.

**Reopen when:** nothing.
A third outcome costs one enum value and buys the alarm its meaning.

### Persisting a snapshot is explicit, and never a side effect of publish

**Decided in milestone 6.** `publish_snapshots` is its own call, and `publish` never writes a snapshot.

A snapshot is derived, so writing one on a heuristic would make publishing **non-idempotent**: the same publish would produce different bytes depending on what the cache happened to hold.
It would also grow the file with caches nobody asked for, at gigabyte scale on the documents that most need the file to stay small.

An op with no cached snapshot is skipped rather than reported, because the cache is derived and may legitimately have evicted it.

**Reopen when:** a heuristic appears that is good enough to be worth the idempotence, which would have to be argued rather than assumed.

### Recovery verifies a whole batch before it applies any of it

**Decided in milestone 6.** `try_verify_batch` decodes and checks every received op, reaching the graph for nothing; `apply_verified_batch` is infallible.

The property wanted is "a partial or hostile batch leaves the replica exactly as it was".
The obvious way to get it is to undo what was applied, and the obvious way is wrong.
A rollback is code that runs only on the hostile path, which is the path least likely to be exercised and most likely to be attacked.

Splitting the verb makes it true for free instead.
Every fallible step runs before the first mutation, so there is nothing to roll back because nothing happened.

The split is also what lets a caller with more to weigh — a store holding a required snapshot — refuse the batch on its own grounds while that guarantee still holds.

**Reopen when:** nothing.

### Filling a skeleton is its own verb, on the graph and on the writer alike

**Decided in milestone 6.** `op_graph::fill_payload` sits beside `add`, and `store_writer::fill_op_payload` beside `insert_op`.

Both could have been folded into the existing verb, and both were deliberately not.

`op_graph::add` is idempotent by id, which is what makes re-adding content safe everywhere.
`insert_op` is `ON CONFLICT DO NOTHING`, which is where publishing's idempotence comes from — "a conflict means the identical row is already there".
That claim stops being true the moment a row can be skeletonized.
Relaxing it to an upsert would let an ordinary publish rewrite a stored op — on the safest operation in the format, to serve the rarest one.

A dedicated verb also buys a guarantee an upsert cannot: the fill is scoped to rows whose payload is NULL, so it **cannot damage a good row even when called wrongly**.

**Reopen when:** nothing.
The asymmetry is the point: emptying and filling are both narrow, and appending stays unable to destroy.

### A skeleton's parents are covered by no hash, so integration compares them explicitly

**Decided in milestone 6.** A received op whose parents differ from the ones the replica holds under that id refuses the batch.

An op id commits to its parents, so two full ops that hash alike cannot disagree about them — the check is unreachable there.
A **skeleton** is different: its parents came out of storage and no hash has ever covered them, because the payload they were hashed with is gone.

So this is the one place content addressing does not reach, and integration is the one moment it becomes checkable at all.
It costs a comparison on a path that is already verifying.

**Reopen when:** skeletons gain a hash over their surviving fields, which would make the check redundant.

### A required snapshot is demoted, not recomputed, once its ancestry is payload-complete

**Decided in milestone 6.** Recovering everything behind a required snapshot flips its `required` flag and unpins it, and touches not one byte of its payload.

There is nothing to recompute.
The snapshot was materialized *before* the prune, over the very ops that just came back, so it already is the true `surviving` at that op.
What changes is not its contents but its standing: history can reproduce it again, so it stops being load-bearing and becomes an ordinary cache entry.

Re-encoding a payload that can run to gigabytes in order to move one bit would be the expensive way to change nothing.

The demotion is written **after** the fills, and the order is load-bearing.
A crash between them leaves a snapshot still marked required over history that is already back, which costs one pinned cache entry.
The reverse would leave a droppable snapshot standing over history that is still gone, which is data loss.

**Reopen when:** nothing.

### A batch forking below a still-required snapshot is refused, unless it completes that ancestry

**Decided in milestone 6.** This is the pruning boundary above, arriving from the other direction.

An op that forks below a required snapshot presents a writer the emptied ops superseded, and the snapshot has no `superseded` set to suppress it with.
Merging fabricates a multi-value; replaying reads the skeletons as silent and is lossy.
Both are exactly what `prune` refuses to create, so integration refuses to create them too.

**The escape is what makes this a boundary rather than a ban.**
Sending the rest of that snapshot's ancestry in the same batch completes it, which demotes the snapshot — and once demoted there is no boundary left to fork below.
So the two rules are one rule: complete the ancestry and the fork is fine, leave it incomplete and the fork is refused.

Refilling *ancestors* of a required snapshot is always safe and is never refused.
The walk terminates at the snapshot and never expands its parents, so ops it does not reach cannot change any sweep.
It is worth stating because the reverse is the intuitive guess, and the plan that specified this item guessed it.

**Reopen when:** a required snapshot gains its superseded ids, which is the same change that would relax pruning's boundary.

### Metadata is any canonical value, not necessarily an object

**Decided.** `try_decode_op` requires the metadata blob to be a canonically encoded value, and checks nothing else about it.
`op_builder` writes an object, and a decoder still accepts whatever kind it is handed.

The original sketch said "an object", which would have been a second format rule to enforce.
Nothing interprets metadata, so constraining its kind buys no safety.
Enforcing it now would also make relaxing it later a **forward-compatibility break**, since builds predating the relaxation would reject files they could otherwise carry unharmed.

That asymmetry is the general rule here, and it is worth stating once: a decoder that rejects more than it must costs compatibility, and a decoder that rejects less can always tighten later.
Canonicality is different, and non-negotiable — it is what makes equality byte equality, so it stays enforced.

**Reopen when:** something starts interpreting metadata, which would make its shape load-bearing.

### An unknown assignment encoding tag is a decode error

**Decided.** `try_decode_op` rejects an assignment blob whose leading tag it does not know, with an error naming the tag.

The tag exists so the assignment encoding can change without touching the hashing rule, and a build that predates tag 2 genuinely cannot read a tag-2 op's assignments.
The alternative is holding such an op as verifiable-but-uninterpretable, so it could still be stored, verified and relayed.
That is representable under the byte-first shape, and is deliberately not taken yet.

Forward compatibility in this design lives one layer up.
The entity/component set is where applications evolve indefinitely, and the op encoding is meant to stay fixed.
So spending complexity on relaying ops nobody can decode buys a compatibility axis [compatibility.md](compatibility.md) does not depend on.

**Reopen when:** a tag 2 is actually proposed.
At that point the question is whether tag-1-only builds must relay tag-2 ops, and the byte-first op has kept that door open.

### An abstention is an assignment kind, carried by a per-record byte in `sorted_v1`

**Decided in milestone 8.**

An op could only ever *write*.
Deletion via `$alive` removes a whole component or entity, so there was no way to un-write one property — and reverting an override to whatever is underneath is exactly that.
`assignment_kind::abstain` is it: an assignment that supersedes its ancestors as a write does, and then contributes nothing.

**It is a property of the assignment, not of the value**, which is what decided where it goes.
A `value_kind::absent` would have widened a format constant validated by a hard upper-bound tag check in three separate switches.
And `value.hh` is consumed by `versioned-document-file` for assets and the workspace, so every value consumer in two libraries could then receive a value that is not a value.
A third blob in `op_payload` was worse still: it changes the hash preimage, and `op_payload`'s two-blob shape is what the file layer stores.

So every assignment record now opens with a kind byte, and an abstention carries no value at all.
Not a `null` — two spellings of one thing is the canonicality problem again.

**It went into `sorted_v1` rather than a new tag 2, and that was deliberate.**
A second encoding was drafted first: written only when something abstained, so abstain-free ops kept their bytes and their ids and stayed readable by an older build.
That is the right shape *once something depends on it*, and nothing does: vdoc has no users outside its own tests.
So all the compatibility apparatus bought was two decode paths, a canonicality rule the decoder had to enforce, and an extra decode error.
Changing `sorted_v1` costs one round of op ids and nothing else, and it leaves one code path where there would have been two forever.

Every op id therefore moved.
A golden test over fixed inputs pins the new ones, because an op id *is* the content address.
If the encoding or the preimage drifts again, every stored op silently stops matching its own id, and no other test would notice.

**Reopen when:** a `.vdoc` exists that someone would be upset to lose.
From that point a format change needs the tag, and the drafted two-encoding scheme above is how to do it.

### An abstention never reaches the raw document

**Decided in milestone 8.**

Inside the sweep an abstaining writer is an ordinary survivor carrying a flag; `build_document` drops it, and a path whose every survivor abstained is simply absent.
So `property_value`, `raw_property` and the snapshot format are all untouched, and `versioned-document-file` needed no change at all.

**The consequence to state plainly: a concurrent write beats a concurrent abstention, and nothing reports it.**
That is storage resolving a conflict, and it sits against [equal concurrent writes are still structurally multi-valued](#equal-concurrent-writes-are-still-structurally-multi-valued).
That entry is the rule that storage records what happened and never collapses anything.

It is taken anyway, on two arguments.
The direction is the one `$alive` already established: the non-vanishing side wins deterministically, because losing a value is not recoverable and a re-attempted withdrawal is.
And the permanence is asymmetric — op bytes are forever, while `raw_document`'s shape and the snapshot encoding are recomputable and already versioned.
So this is the half that can be revisited later at no cost to anything stored.

The alternative, designed and deliberately not built: a parallel `cc::vector<op_id> abstaining_writers` beside `raw_property::writers`.
It keeps "every entry in `writers` is a real value" intact, leaves each per-writer record byte-identical, and makes the conflict diagnosable.
Its cost is a `snapshot-v2` in `versioned-document-file`, plus the few sites that read writer lists directly.
That is pre-planned there: the codec is versioned in its own name, and a snapshot that will not decode is a load issue rather than a failure.

Worth noting the motivating case cannot hit it: a per-frame override layer is a linear rebase, so it has no concurrency at all.

**Reopen when:** a UI has to explain why a reset-to-default did not take.
That is the diagnostic this cannot produce, and the parallel channel above is the answer.

### Layering composes per property path, and a higher layer replaces rather than merges

**Decided in milestone 8.**

A `layer_stack` composes several independent histories into one document, and does it on `raw_document`s rather than on typed ones.

**Property granularity is the requirement, not a refinement.**
A typed `document` holds component structs in dense columns, so the only thing a higher layer could do to a component is replace it whole.
Then a base that animates a transform, with an override on `position` alone, would have `rotation` frozen at the edit.
That is exactly the rebasing the feature exists for, broken.
So the composition has to happen below the typed layer, and once it does, materializing one ordinary `document` on top costs nothing extra and keeps every downstream guarantee intact.

**Replace, never merge**, per path: the winning layer's entire writer list stands.
That makes layering the totally ordered conflict-free composition, in contrast to a DAG merge.
It also means a conflict is always layer-local: a contested path inside the winning layer reaches `parse_policy` exactly as it would unlayered.

The composition unit is the **entity**, because `impl::select_entity` takes one `raw_entity`.
Composing at that granularity is what lets a layered parse reuse the ordinary selection and construction phases; `impl::parse_from` exists so that reuse is literal rather than a second copy.

Composed entities are held **all at once** rather than one at a time, which is a lifetime requirement.
Selection records `raw_component const*` into what it was handed and construction reads them afterwards, so a single reused buffer would dangle every entity but the last.

**Reopen when:** arbitrary layer reordering is wanted.
Nothing here forecloses it — it would mark the stack fully dirty exactly as muting does — but no case has asked for it.

### A layer stack pulls every delta rather than being handed one

**Decided in milestone 8**, and it replaced a design that had been agreed the other way round.

The stack holds each graph layer's head, and `set_head` is the only way to move one.
A `direct_layer` bumps its own version from inside its mutators.
So `apply` derives every layer's delta itself.

The alternative, drafted first, was a `layered_change` carrying per-layer op movements.
`apply` would validate those against the version the composed document was built at, reporting a mismatch rather than asserting it, since `CC_ASSERT` is compiled out in `release-*`.

Pull is better on the same axis that motivated the validation: **a stale or forgotten change set is not expressible**, rather than caught.
It also deletes the annotation types, the per-layer version comparison, and a fallback reason.
What it costs is that the stack consumes a direct layer's accumulated dirty set, so such a layer belongs to exactly one stack — recorded in its header.

The one thing push offered that pull would have lost is a producer handing in a dirty set it already knows, skipping the byte compares.
`direct_layer::mark_dirty` covers that without reopening the hole.

What remains is `apply_fallback_reason`, because *how* a layer moved is not always derivable.
A merge on a graph layer's chain, a chain past the bound, or a structural change each recompose in full and say so.

**Reopen when:** a second stack has to observe one direct layer.
That needs the layer to retain changes per consumer, which is the point at which push becomes the simpler shape again.

### A directly written layer is attributed to a synthetic writer id, and versioned by a counter

**Decided in milestone 8.**

`synthetic_writer_id(name)` is `blake3("vdoc::layer/v1" ‖ name)` — domain-separated from `"vdoc::op/v1"` on the same argument that separator itself rests on, so it cannot collide with a real op id.
It is a function of the name alone, so writer sorts and the ids inside diagnostics reproduce across runs and machines.
It is never the all-zero id, which already means "absent parent" and "nothing chosen".

One id per layer rather than per path: hashing the path too would make the ids unbounded and would make the byte-equal-writers agreement check meaningless.
It essentially never reaches `resolve_multi_value` anyway.
Replace-not-merge means a direct layer's writer and a graph layer's never appear in one candidate list, and a direct layer is single-writer per path by construction.

**The risk is leakage, not collision.**
A composed document contains writer ids no op has, so installing one into a `snapshot_cache` would put a fabricated op id into a file.
That is the same class as the existing filtered-result warning, and `snapshot_cache::install` now names it.

A direct layer's **version is a plain counter**, bumped by its mutators.
A graph layer's version is an op id, which is a real content address; a counter is not, and is good for nothing but telling the stack that something moved.
It is bumped inside the mutators rather than by the caller because "forgot to say it changed" is precisely the silent staleness the pull model exists to make unreachable.

**Reopen when:** two layers need distinct provenance under one name, or a layer's contents need a content address.
The second would mean hashing the layer, which is O(n) per frame and is what the counter exists to avoid.

### Provenance is per path, and a composed component may come from several layers

**Decided in milestone 8.**

`layer_stack::provenance_of` takes a `property_path` and nothing coarser.

Per component is not merely inconvenient, it is **not well-defined**.
Replacement is per path, so one typed component is routinely assembled from two layers, which is the case the whole feature is built around.
And the typed `document` holds no raw property data at all, so provenance is unrepresentable there rather than just absent.

It materializes the entity per layer on demand instead of keeping every layer's document resident, because the incremental path deliberately materializes only the dirty subset.
So it is a UI query and not something to run in a loop, which its header says.

**Reopen when:** a UI wants provenance for a whole document at once.
That wants a different shape — one pass producing a path-to-layer map — rather than this called in a loop.

### `$schema_version` must agree among the layers that supply a component

**Decided in milestone 8.**

Every layer contributing a real property to a component and also stamping a `$schema_version` must agree with the stamp that won, or the component is dropped with a `layered_schema_version_conflict`.
A layer that does not stamp has **no opinion**, rather than "version 0".

**This is the default hazard rather than an edge case**, which is why it is enforced rather than documented.
`op_builder::set` always stamps, so every layer written through the typed API carries a version, while the composed stamp is whichever layer won that one path.
The failure is silent in one direction — v1-shaped data read at v2 — and total in the other, since a too-new or contested version skips the whole component.

The stricter rule was rejected: requiring a layer that overrides any property to also carry the component's version would forbid overriding `transform/position` alone, which is the feature.

**Reopen when:** an application legitimately wants layers at different versions of one component.
That needs a migration at the composition boundary, which is a much larger thing than this check.

---

## Interpretation

### `component_traits::parse` is handed a `property_reader`, not a `raw_component`

**Decided.** A component's `parse` receives a `property_reader`, which carries the entity, the component type, the resolved schema version, the policy and the report.
Its `try_get` is the only place the multi-value rules exist.

The sketch had `parse(raw_component const&, entity_id, parse_policy const&, parse_report&)`.
That signature makes reimplementing the collapse the path of least resistance: the obvious thing to write is `raw.try_get("x")->single()`, which asserts on exactly the case the rules are for.
Rules that live in one place stay in one place only if the easy path goes through them.

`reader.raw()` is still there as an escape hatch for a component that must iterate what it was not told about.
Its existence is the argument for the default rather than against it: coming through it means owning the rules yourself, and that is now a visible act.

**Reopen when:** a real component needs the untyped shape often enough that `raw()` stops reading as an exception.

### `$alive` and `$schema_version` never reach `resolve_multi_value`

**Decided.** Both reserved properties are read straight off `raw_property::writers`, never through `property_reader::try_get`, so a policy is never asked to pick between their writers.

A policy that resolved a contested `$alive` to false would make a thing vanish, which is the one failure this design refuses — resurrecting is recoverable and vanishing is not.
A policy that resolved a contested `$schema_version` would hand a component's `parse` a version nobody wrote, and migration would run against a shape that never existed.
Both are skipped instead: a contested `$alive` stays alive with a diagnostic, and a contested version skips the component with one.

**Reopen when:** nothing.
A policy voting on the library's own conventions is the failure mode, not a feature.

### `op_builder::set` stamps `$schema_version`, and `component_writer` rejects the sigil

**Decided.** `write` does not stamp its version; `op_builder::set<C>` does, once, before calling it.
`component_writer::set` asserts on any `$`-prefixed property name.

The sketch had `write` stamp, which is one more thing every component author must remember and one more place the stamp can be wrong.
With one stamp site the invariant is structural, and with the sigil rejected outright a component author cannot write a reserved name even deliberately.
`set_alive` and `set_entity_alive` on the builder are what deletion is spelled with, for the same reason — otherwise every application hand-writes the reserved path and the convention drifts.

**Reopen when:** nothing.

### The document arena is vdoc-local until clean-core grows one

**Decided.** `vdoc::impl::document_arena` is a bump allocator behind a `cc::memory_resource`, in `impl/document_arena.hh`.
It moved out of `document.cc` in milestone 7, when `document_builder` became a second thing that allocates from one.

clean-core has the `cc::memory_resource` seam but no bump resource behind it, and a document wants exactly one: build cheaply, free in a single release.
This is the **second** hand-rolled copy in the tree — `cc::impl::intern_shard` is the first — which is the point at which it should be written once, in the library that owns the capability.
It is not written there yet only because the document's own layout was still moving; the seam is already the right shape, so the migration is a deletion rather than a refactor.

**Reopen when:** clean-core grows a bump `cc::memory_resource` next to `cc::system_memory_resource`. Then this copy goes.

### A document is evolved by consuming it, not by mutating it and not by sharing it

**Decided in milestone 7**, and it argues with [the typed document](concepts/the-typed-document.md), which said the absence of a mutation API was "not a limitation to be lifted later".

That sentence stands, read precisely: `document` still has no `set`, and a document you are holding still cannot change.
What was also true and is no longer is that **the only way to a new document was a full parse**.
`document_builder` consumes a `document&&`, patches its arrays, and `freeze()`s back — the same shape as `op_builder` and `op`.

Three options were on the table, and the property to preserve was the one immutability was really bought for: a parsed document is safe to hand to another thread and hold indefinitely.

- **Consuming builder — taken.** Zero copy, no refcounts, and it keeps that property exactly, because evolving a document requires giving it up.
  It is the compile-time form of the "no readers right now" contract the next option asks for at runtime.
- **In-place `document::apply_batch` — rejected.** Fastest to write, and it gives up thread safety outright, which is the strongest claim the concept doc makes.
- **Immutable plus structural sharing — rejected, and it was the designed-for future.**
  It is the right answer when several versions are live at once, which an editor does not want: it keeps one document and the op graph, not a history of documents.
  The cost is not the refcount per column but the shared *destruction*.
  Components may own heap — `vdoc_test::mesh` holds a `cc::string` — so a shared column needs a shared arena and a shared destroy, which is an allocator redesign.
  The layout still does not preclude it.

The failure mode here is a slot destroyed twice or never, which is silent.
The oracle is a full `parse`, compared over the whole query surface, since `document` has no `operator==` and deliberately never will.
One of the test components owns heap, so a leak surfaces under ASAN rather than passing quietly.

**Reopen when:** an application genuinely needs two versions of one document live at once.
That is what structural sharing exists for, and consuming evolution cannot serve it at any price.

### `component_schema` gains a relocate hook

**Decided in milestone 7.** `relocate_range(dst, src, count)` move-constructs and then destroys the sources, handling overlap.

A dense column cannot insert, remove or grow without moving its tail, and the library cannot memmove instead.
`is_component` requires only move-construction and destruction, so nothing generic knows whether a component is trivially relocatable.
The hook is a `cc::function_ptr` like the other three, and the compiler collapses it to a memcpy loop for the types where that is what it means.

**Reopen when:** clean-core grows a trivially-relocatable trait, at which point the hook can be skipped for the types that satisfy it.

### The incremental apply's gate is a bounded single-parent chain, not an ancestor query

**Decided in milestone 7.**

`vdoc::apply` takes the fast path exactly where `to` reaches `from` through single-parent edges within `max_chain_ops`, default 64.

**Single-parentage is the whole argument.**
Each op on the chain dominates every writer it overwrites and contributes no other, so the chain's assignments are the complete delta.
No untouched entity changed, and none became multi-valued.

That is weaker than "no multi-values anywhere", which was the obvious condition and is the wrong one.
Multi-values *inside* the touched set are fine, because those entities go through the full selection-and-construction path exactly as a parse runs it.

The bound exists for the same reason the snapshot gate's does.
Proving ancestry exactly on a DAG is the global query this design declines to pay for, and a bounded walk is the cheap sufficient case for a [mostly-linear history](concepts/workloads.md).
Everything else re-materializes and re-parses, so correctness never depends on the gate.

`incremental_apply_options::force_full_reparse` exists so the two paths can be run over identical input and compared, which is what the corpus-wide differential test does.
`took_fast_path` is asserted alongside, so a green run cannot mean "it always fell back".

**Reopen when:** `took_fast_path` is false in an ordinary session.
That is a statement about the workload rather than about the bound.

### An incremental apply recomputes diagnostics for touched entities and never retracts a document-scoped one

**Decided in milestone 7.**

A parse appends to its report; an apply edits it.
Entries naming a touched entity are dropped before anything re-decides that entity, then recomputed — carrying a stale finding is a correctness trap rather than untidiness.

`unsupported_component_type` cannot follow that rule.
It is document-scoped, reported once per type and carrying no entity.
So retracting one means proving the type is gone from the *whole* document, which is an O(document) scan and the one thing this path exists to avoid.

So it is never retracted, and a type that stops being present keeps its diagnostic until the next full parse.
The alternative considered was a full re-parse whenever a column empties, which is more correct and less predictable.
It is a rare O(document) spike, landing exactly when a user deletes the last instance of something.
Predictable was preferred, and the asymmetry is documented in [interpretation](concepts/interpretation.md#what-an-incremental-apply-owes-the-report) rather than left to be discovered.

**Reopen when:** a UI shows a stale `unsupported_component_type` and users notice.
The fix is then a periodic full re-parse, not a per-apply scan.

### A re-interpretation is driven by a dirty path set, and coarsening one is conservative

**Decided in milestone 8.**

`change_set` is a sorted `property_path` vector plus a `change_granularity` of property, component or entity, plus an `everything` state.
It is what an incremental re-interpretation consumes, and `(graph, from, to)` became one way to *produce* one rather than the only way to ask for one.

The reason is that a single-parent op chain is not the only thing that knows a delta.
A directly written source knows its own, and a composition knows the union of its parts' — none of which can be phrased as a pair of ops.
Generalizing the input was cheaper than growing a second apply beside the first.

**The invariant is that `covers` over-reports at worst and never under-reports.**
That is what makes granularity a speed dial rather than a correctness risk: every consumer is correct at every granularity, and only the amount of recomputation varies.
So a producer that cannot be precise is free to be coarse, and is never forced to be wrong.
`coarsen_to` asserts on a request to *refine*, because inventing the finer information would turn a conservative set into a lying one, and no consumer could detect that.

Coarsening is one forward pass because `property_path::compare_bytes` is entity-major, so everything that collapses together is already adjacent.
That ordering was chosen for the op hash, and this falls out of it for free.

Below the granularity an entry's fields are default ids that **must not be read**.
An id has no invalid state — a default-constructed one equals `of("")`, a legal id that sorts first — so the granularity and never the data is what says how much of a path means anything.
Note this is the *opposite* convention to `parse_report::drop_for_entities`, where an empty entity id means "matches nothing" so that a document-scoped diagnostic survives.
Identical bytes, opposite readings, and the two must not borrow each other's wording.

**Reopen when:** a consumer wants a granularity between property and component, or wants to subtract one set from another.
Subtraction is the one operation deliberately absent: layering needs "minus what a higher layer shadows", and that is a query against the layer stack rather than a set operation.

### A change summary keeps its enumeration rather than gaining an `everything` flag

**Decided in milestone 8**, when `change_set` gained exactly that flag and the symmetry looked obvious.

It is not symmetric, because the two face opposite directions.
On an *input* set, `everything` is the safe over-approximation — the honest thing to say when no delta is available.
On an *output* summary it is a **loss**: the slow path already enumerates every entity and component explicitly, and that list is strictly more useful to an invalidator than a flag would be.

A flag would also be a second representation of the same fact, which every consumer would have to remember to check before reading the vectors.
Forgetting it is silent under-invalidation, which is the failure mode this library spends the most effort making unreachable.

**Reopen when:** the slow path's O(document) summary enumeration shows up in a profile.
The fix is then a flag *plus* an audit of every consumer, not a flag alone.

### A fallback names its reason

**Decided in milestone 8.**

`incremental_apply_stats::fallback_reason` says why an apply re-parsed: `forced`, `no_single_parent_chain`, or `chain_too_long`.

Before this, only `took_fast_path == false` was observable, which conflates three situations wanting three different responses.
`chain_too_long` is a statement about `max_chain_ops` and is fixed by raising it.
`no_single_parent_chain` is a statement about the history and is not fixed by anything.
`forced` is a test pinning the slow path, and should never be read as either.

It costs one enum on a struct a caller already passes, and it is what makes a fallback that should not be happening findable instead of merely slow.

**Reopen when:** nothing.
This is an observability field, and the cost of another enumerator is one line.

---

## Hashing

### BLAKE3, over 32-byte ids — with a standing reservation

**Decided.** Op ids and blob content hashes are BLAKE3-256.
This is what makes a replica able to accept history from an untrusted peer and verify it by recomputation.

**Philip's reservation, recorded deliberately.**
He considers the threat model overblown, and does not see many realistic situations where the hash is the weakest link in the system.
He accepts BLAKE3 to settle the argument rather than because he is convinced by it, and BLAKE3 is roughly **4× slower than xxHash**.

The condition attached to that acceptance: **if this decision makes the normal use case unresponsive in order to guard against a scenario that never happens, he will tear into the design.**
That is a legitimate outcome, not a grumble to be managed.

So the reservation is written as something testable rather than a matter of taste.
Hashing is bounded to exactly two places:

- **once per op**, over a few hundred bytes;
- **once per blob, at import.**

It is **never** in the materialize path, never in a query, never in a parse, never per-frame.
In-memory maps key on a truncated 64 bits of the digest, so the full 32 bytes cost storage and a memcmp on collision, and nothing else.

**Measured, milestone 0 — the 4× was optimistic.**
`cc::blake3` now sits beside XXH3 in [hash-benchmark.cc](../../../base/clean-core/tests/benchmarks/hash-benchmark.cc), so the ratio is a recorded number instead of an estimate.
On an i9-12900H under `release-clang`, against XXH3-128:

| input | XXH3-128 | BLAKE3 | ratio |
|---|---|---|---|
| 8 B | 2.35 GB/s | 0.11 GB/s | 21× |
| 256 B — an op | 12.99 GB/s | 1.09 GB/s | 12× |
| 64 KiB — a blob chunk | 27.11 GB/s | 3.11 GB/s | 8.7× |

The ratio is two to five times worse than the 4× the reservation was accepted on, and that is the honest reading.
BLAKE3 pays a fixed per-call cost that XXH3 does not, so the gap widens as the input shrinks.
Note that this machine is Alder Lake, where AVX-512 is fused off — BLAKE3's widest path is unavailable, and a CPU that has it roughly doubles the 64 KiB figure.

**What that costs where the design actually hashes**, which is the question the reservation asks:

- **An op**, a few hundred bytes: about **270 ns**. Committing a thousand ops in one go is a quarter of a millisecond.
- **A blob at import**, at 3.1 GB/s: a gigabyte takes about **0.3 s**, against a disk read of the same bytes that will not be faster.

So the ratio got worse and the conclusion did not change: at the two places the design hashes, the absolute cost is far below anything a user perceives.
The reservation stands as written, and this is the evidence it asked for rather than a case for reopening.

**Measured, milestone 6 — the loop itself, and the condition did not fire.**
The reopen condition names a profile of an ordinary open / edit / save loop, and milestone 6 is the first point at which one existed.
[document-loop-benchmark.md](../../versioned-document-file/docs/benchmarks/document-loop-benchmark.md) is the write-up; the same i9-12900H under `release-clang`, so it composes with the table above.

| document | loop | of which hashing | share of loop | share of the open |
|---|---|---|---|---|
| 200 ops | 32 ms | 0.14 ms | 0.5% | 5.6% |
| 2,000 ops | 278 ms | 1.15 ms | 0.4% | 15.9% |
| 8,000 ops | 2,182 ms | 4.58 ms | 0.2% | 20.4% |

Doubling the loop's hashing does not produce a measurable change in the loop: the injected delta scatters around zero, because it is smaller than run-to-run variance.
That is the strongest form the answer comes in.

**The share worth watching is hashing's share of the OPEN, not of the loop.**
The loader re-hashes every op it reads, so that cost is linear in history length rather than in document size — which is the one place it could ever grow into something.
Pruning is what bounds it, and at 4.6 ms for 8,000 ops there is nothing to act on today.

What the loop is actually spent on is materialization and op building, and neither is what the reservation was about.
The sharpest of those is that `op_builder::build` takes an `op_graph` with no `snapshot_cache` overload, so the edit path cannot reach the caching built to make exactly this cheap.
That is a finding this measurement produced rather than a hashing question, and it is recorded in the write-up.

The table above is the milestone-6 measurement and is kept as it was taken.
The loop has since got much faster — the 8,000-op figure is 1,110 ms rather than 2,182 ms — so hashing's share of the *loop* has roughly doubled while its absolute cost has not moved.
Its share of the *open* is unchanged, and that is the one the reopen condition is really about.

**Reopen when:** BLAKE3 shows up in a profile of an ordinary open / edit / save loop.
Checked in milestone 6 and it did not; re-check if the loop's shape changes, above all if loading stops being one hash per stored op.
If it ever does fire, the design put hashing somewhere it does not belong, and the fix is to move the hashing — after which the choice of hash can be re-argued on evidence.

### Snapshot bytes get their own chunk table, and share the blob codec

**Decided in milestone 6.** A snapshot's payload lives in `snapshot_chunk`, cascading off `snapshots`, and not in the blob store.

Chunking is not optional: SQLite caps a single value near a gigabyte, and a snapshot of a document with millions of properties goes past that.
Once a user's document is over the line, no later change can rescue the file they already have.

The blob store already chunks, already has an encoding seam, and already deduplicates — so pointing snapshots at it looks obviously right.
**The argument against is lifetime.**
Blob lifetime is decided by a mark-and-sweep over the asset index, and a required snapshot is load-bearing data whose loss is unrecoverable.
Putting it behind a GC makes a marking bug into a data-loss bug, in the one place the format promises never to lose anything.
A cascade makes the lifetime structural instead: those bytes die when their snapshot row dies, and nothing else can reach them.

The blob store's two real advantages do not apply here either.
Snapshots do not repeat, so cross-payload dedup buys nothing, and a snapshot must be decoded whole to be a document, so ranged reads buy nothing.

What *is* shared is the **codec**: `blob_codec` became `payload_codec`, and one table serves blobs and snapshots alike.
Adding zstd later stays one entry there rather than two.
The chunker is shared for the same reason — two implementations could only ever differ by writing a file one build reads and another does not.

**Reopen when:** a third chunked payload appears, at which point the table-per-kind pattern is worth generalizing rather than copying a third time.

### String interning belongs in clean-core

**Decided.** The interner is a `cc::` facility, not a `vdoc` one.

It is general vocabulary — anything with symbolic identity wants it — and there is nothing document-specific about it.

**Built in milestone 0, and the handle carries a pointer rather than a numeric id.**
`cc::interned_string` holds the address of an entry that is never moved and never freed, so `as_string_view()` and byte-ordering are direct rather than a table lookup.
It also settles the "never serialize the raw id" rule by construction: there is no id to write down, only a private pointer.
Identity stays process-local, exactly as [the model](concepts/the-model.md#entity-ids-are-strings) says.

**There is no `operator<`, and `vdoc` must pick its order explicitly.**
`compare_bytes` is reproducible across processes and costs a memcmp; `compare_identity` is a pointer compare whose order differs every run.
Everything whose order is written to a file, sent to a peer or shown to a person — the sorted entity table above all — takes `compare_bytes`.
`compare_identity` is available for a scratch sort nobody observes, and using it anywhere durable would silently break determinism, which is why the type refuses to guess.

**Reopen when:** nothing.

---

## Storage engine

### babel::sqlite is the engine, and gets extended rather than bypassed

**Decided.** The file is a SQLite database, reached through `babel::sqlite`.
Where that wrapper lacks something the format needs — incremental blob I/O, transaction scoping, connection configuration — the wrapper grows.

Reaching around it to `sqlite3.h` would put a third-party type in a second library and split ownership of the engine across two places.
Growing the lower library is the repo's standing preference, and this is a clean instance of it.

Milestone 4 grew it twice more.
`babel::sqlite::error` carries a result code, because a file that is not a database, a database that is damaged and a lock somebody else holds are three different situations.
Matching on message text to tell them apart is not a mechanism.
And `database` gained the `application_id` / `user_version` accessors, which is how a `.vdoc` file identifies itself and how a version from the future is refused.

The store's own `.shaped-lint.yml` does carry one entry — `<memory>`, for the polymorphic store handle, which is a clean-core gap and the third library to file it.
No sqlite header appears anywhere, which is the clause that was ever load-bearing.

**Reopen when:** nothing.

### One actor owns the connection

**Decided.** A `cc::threaded_actor` holds the database handle exclusively, and results are pushed back as `cc::async` values.

A SQLite connection is not a shared resource, and the alternative — a mutex around a connection touched from arbitrary threads — puts I/O latency on whichever caller is unlucky.
An actor makes the ownership a fact of the structure rather than a convention, and gives the store one thread to serialize its writes on.

**Reopen when:** nothing.

### The store hands itself back synchronously, and reports the load separately

**Decided.** `store::open` returns the handle and a `cc::shared_async` that is ready once the load finished, rather than an async that resolves to the handle.

The caller must own the store from the first instant, because the actor may only ever borrow a pointer to it.
An actor that held the last reference would destroy the store — and with it the actor — on the actor's own thread, which is a join against itself.
Handing the handle back makes that impossible by construction rather than by care, and nothing touches the disk on the calling thread either way.

The alternative — the actor owning the store until the open resolves — was written first and deadlocked on the very first hard failure it was asked to report.

**Reopen when:** clean-core grows a way to tear an actor down from its own thread, which is not obviously a thing anyone should want.

### The in-memory store writes into a detachable image, and has no actor

**Decided.** The in-memory arm writes into a `memory_image` the caller owns, and completes its hooks inline.

The image outliving the store is what makes close-and-reopen mean the same thing on both arms: the load runs again, with its decoding, its verification and its issues.
Without it the in-memory arm could only ever test the write path, and the load path — where the interesting failures live — would be exercised on one implementation instead of two.
That is the difference between an oracle and a shortcut.

It has no actor because the connection an actor exists to own exclusively is the thing this arm does not have.
A thread per unsaved document would buy nothing and would put a second scheduler under a suite that exists to be deterministic.

**Amended in milestone 5: its `on_pump` does report work, for blob fetches alone.**
A fetch must not be resolved inside `blob_source::load`, which may be called with a caller's lock held.
So the arm that could answer instantly is precisely the one that has to queue, and it drains in `pump()`.

That narrows the original claim rather than breaking it.
A correct caller already had to pump: with `SC_THREADS=OFF` the file arm's actor also runs on the calling thread and never progresses otherwise.
So "no caller can tell them apart" holds for every caller that pumps, which is every caller that works on both builds — and a caller that never pumps was already broken in one configuration.

**Reopen when:** nothing.

### A failed publish un-claims its ops

**Decided.** The durable-op set is an optimization for computing a delta, and a publish that failed removes what it had added to it.

Without that, a set that is too large would make the optimization *required* for correctness: the next publish would skip ops that were never written, and the failure would become silent data loss.
With it, a set that is too small costs a rewrite and nothing else — which is the only kind of wrongness an optimization is allowed to have.

A consequence worth stating, because it constrains recovery: a skeleton loaded from a file **is** in storage, so it is durable, and a publish will not offer its filled-in payload later.
Recovery therefore builds its own job rather than routing through publish's reachability delta — which it could not use anyway, since a recovered op has no ref reaching it yet.

**Reopen when:** nothing.

### Recovering is a seventh store hook, because the hooks split by what a write can destroy

**Decided in milestone 6.** `on_recover` sits beside `on_publish` and `on_write_snapshots`, rather than becoming a mode of either.

The split criterion was already written down when pruning got its own hook.
A publish only ever **appends** and is idempotent by content addressing, while a prune **destroys**, and the safest operation in the format should not share a code path with the only destructive one.

That criterion generalizes, and a recovery is a third kind: it **fills a hole back in**.
Three kinds, three hooks.

Riding `on_publish` is not open to it for a second reason: that path derives its ops from the refs by reachability, which is a safety property, and a recovered op has no ref reaching it.
Riding `on_write_snapshots` would put an append on the destructive path, which is what the criterion argues against.

Atomicity settles it either way.
A recovery must fill payloads and demote snapshots in **one** transaction, and splitting it across two existing hooks would be two.

**Reopen when:** a fourth kind of write appears that is none of the three, at which point the criterion is worth restating rather than extending by habit.

### Blobs ship raw-only, with the encoding seam reserved

**Decided.** The `encoding` column and the encode/decode seam exist and are exercised from the start; `raw` is the only encoding registered in v1.

The seam is what costs nothing later — adding compression becomes a vendored dependency plus one codec registration, with no format change and no migration.
Vendoring a compressor before there is content to compress is work in the wrong order.

A file naming an unknown encoding is reported as a load issue and the blob is skipped, exactly like any other soft failure, rather than failing the open.

**Reopen when:** embedded content sizes justify it.
It is an additive change by construction, so nothing about the format has to move.

### Decoding runs on the storage thread while `raw` is the only codec

**Decided.** `fetch_blob` keeps the chunk read and the decode as two visibly separate steps, and `raw`'s identity decode runs on the thread that did the read.

**A whole-blob fetch goes through decode even under a byte-addressable codec.**
The byte-range fast path exists to avoid materializing a multi-gigabyte blob for 64 bytes of it, so it is taken only for a partial range.
Routing every full read through the codec is what keeps that half of the seam exercised instead of dead until the first real codec.
Under `raw` the decode is a move, so the fast path would buy nothing there anyway.

This departs from milestone 5's own wording, which says decoding happens "never on the storage thread".
Honouring that literally today would mean building a decode scheduler — its lifetime, its single-threaded fallback, its ownership — entirely for a function that returns its argument.
Nothing would validate it until a real codec exists.

The seam a real codec would move at is what actually ships.
The read and the decode are separate statements rather than one expression, so introducing a compressing codec moves one call and changes no structure.

**Reopen when:** a second codec is registered.
That is the point at which the hand-off has something to carry and something to test it with.

### Part names are the contract, and position within a name disambiguates

**Decided, reversing an earlier rule.** A part is addressed by `(name, index)`, where the index is its position among the parts sharing that name.
Whole-list position carries nothing: reordering an asset's parts changes no behaviour, and renaming one is the format change it looks like.

The rule used to be the opposite — order was the contract and names were cosmetic, on the argument that keying on a name makes renaming a format change.
That argument is true and cuts both ways: under the old rule *reordering* was equally a format change, and it is the one nobody notices they made.
A rename is a visible, deliberate edit; a reorder happens by touching an export loop.

**The failure modes decide it.**
A wrong index silently returns a different part — plausible bytes, wrong content, undetectable.
A wrong name returns nothing, immediately.
Trading a silent failure for a loud one is worth the rule it costs.

`$main` is the reserved default name, so a single-part asset costs no ceremony: `{.hash = h, .format = "png"}` is reachable through `main_part()`.
Defaulting the field rather than warning about an unset one is what keeps the common case honest — an empty name is then only reachable by asking for it, and is reported.
`$` is reserved generally, leaving `$preview` and friends available without colliding with an application's names.

**A singular lookup errors rather than picking one.**
`main_part()` and `try_find_part(name)` return a result and distinguish `not_found` from `ambiguous`.
An application that expected one part and silently got the first of three has a bug it cannot see.

That is only reachable because **duplicates are kept at load**: the loader reports them and keeps them, exactly as it keeps a dangling ref.
So the report names the problem at open and the lookup names it at use, and dropping them would make `ambiguous` unreachable while losing the caller's data.

**Reopen when:** nothing.
The format never moved — `name` was always stored per part — so this was an API and an invariant change only.

### Asset dependencies are declared by the application, and reclamation takes a root set

**Decided.** An asset carries a `deps` list of asset ids, and `reclaim(roots)` keeps the closure of those roots under it.

The store **never interprets a blob**, so it cannot discover that one asset references another — the bytes that would say so are exactly the bytes it refuses to parse.
Without a declared list the only place a closure could be computed is the application, which would have to resolve its whole asset graph before it could ask for anything to be collected.
Declaring it moves the walk into vdoc at the cost of one nullable column, and assets already load whole at open, so the map is resident and the flood fill needs no new machinery.

The list is **uninterpreted**, which is what lets it carry ids that resolve elsewhere — built-in, procedural, remote — alongside the ones in this file.
An id naming nothing here is silently skipped rather than reported: a file is one asset source among many, so a dangling entry is the expected case.
Reporting one would fire constantly and train readers to ignore the report.
Cycles are ordinary and terminate on the visited set.

This restates, rather than abandons, the rule that reclamation marks from the asset index and only from there — the index is now first narrowed by a caller-supplied root set.
An application that under-declares gets a sweep that collects too much, which is the failure mode a declaration-based scheme has to own.

The alternative was a separate edge table.
It buys queryable edges and costs a table, a row type, reader and writer methods, delete-on-upsert semantics and a second whole-table read — for data the asset row already carries to the same place.

**Reopen when:** something needs to ask "what depends on this?" rather than "what does this depend on?".
That is a reverse index, and it is the one question a per-row list answers badly.

---

## The one deliberate hole in immutability

### The asset mapping is mutable, and remapping is retroactive

**Decided.** Blobs are immutable and content-addressed; the name → asset mapping is not.
Re-pointing a name changes what every past version of the document resolves to, and that is intended.

Consequently **op ids do not commit to asset content**, and a document is reproducible only relative to an asset resolution.

The alternative was considered and rejected: hashing asset bytes into the DAG turns replacing a placeholder, fixing a texture, or relinking a moved library into a rewrite of history.
For a format meant to hold real content work, that strictness would cost it its purpose.

An asset-index edit therefore creates no op, moves no ref, and is not undoable through the document's history.

**Reopen when:** nothing.
This will look like an oversight to anyone reading the integrity guarantees in isolation, and it is not one.
