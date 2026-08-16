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

**Decided.** Every layer described in [concept.md](concept.md) is implemented, including merges, multi-values, snapshots, pruning and verification.

**Partial implementation is never a licence to simplify the design.**
"Nothing uses it yet" is not a reason to cut a layer, collapse a seam, or skip a case — it is a statement about the calendar, not about the design.
A design cut down while it is half-built is not a smaller design; it is a broken one whose breakage surfaces a year later, in the shape of a format that cannot express what it was designed to express.

This is a standing instruction to everyone implementing the milestones, and it is why [todo/](todo/_index.md) describes the whole thing before any of it is written.

**Reopen when:** nothing.

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

[concept.md](concept.md#values) left this ambiguous by saying "payload byte length" without fixing where the payload starts.
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

[milestone-1.md](todo/milestone-1.md) said only that the builder "rejects a duplicate key", which an assert would also satisfy.
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

[milestone-2.md](todo/milestone-2.md) originally sketched an op carrying decoded `metadata` and `assignments` *plus* an optional payload.
That is a fourth copy of state already present three times.
It also corresponds to nothing storage hands back.
[format.md](../../versioned-document-file/docs/format.md#ops--the-dag) stores the metadata and the assignments as two separate blobs, with no single payload column anywhere.
The combined form would therefore have to be synthesized, which is re-serialization wearing a different hat.

Inverting it makes [the no-re-serialization rule](concept.md#the-op-is-its-bytes) structural rather than remembered.
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
A third-party reader implementing the format from [concept.md](concept.md#the-producer-canonicalizes-the-hash-just-hashes-bytes) would sort by bytes.
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
[concept.md](concept.md#multi-values) says a later op that writes the path resolves it back to a single value, and that later op is this one.
A user who sees `10` and sets `10` must be able to collapse it.
The agreed-multi-value side channel exists precisely as a tidy-up hint for this write, and diffing it away would make the channel dead.

Concluding "both writers said 10, so the current value is 10" is the collapse [the interpretation layer](concept.md#writers-that-agree-still-conflict-structurally) owns.
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
[milestone-6.md](todo/milestone-6.md#1-snapshot-terminated-materialization) works out why a persisted snapshot needs a strictly stronger condition, and what each snapshot kind does instead.

This pass is also the oracle milestone 6's snapshot cache is checked against, which is a second reason to prefer it.
An oracle has to be correct by inspection, and "correct modulo a chain cover" is not.

**Reopen when:** profiling shows the state copy at merges dominating, which is a question about representation rather than about the rule.

### Metadata is any canonical value, not necessarily an object

**Decided.** `try_decode_op` requires the metadata blob to be a canonically encoded value, and checks nothing else about it.
`op_builder` writes an object, and a decoder still accepts whatever kind it is handed.

The sketch in [milestone-2.md](todo/milestone-2.md) said "an object", which would have been a second format rule to enforce.
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

**Decided.** `vdoc::impl::document_arena` is a bump allocator behind a `cc::memory_resource`, declared and defined entirely inside `document.cc`.

clean-core has the `cc::memory_resource` seam but no bump resource behind it, and a document wants exactly one: build cheaply, free in a single release.
This is the **second** hand-rolled copy in the tree — `cc::impl::intern_shard` is the first — which is the point at which it should be written once, in the library that owns the capability.
It is not written there yet only because the document's own layout was still moving; the seam is already the right shape, so the migration is a deletion rather than a refactor.

**Reopen when:** clean-core grows a bump `cc::memory_resource` next to `cc::system_memory_resource`. Then this copy goes.

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

**Reopen when:** BLAKE3 shows up in a profile of an ordinary open / edit / save loop.
If it does, the design put hashing somewhere it does not belong, and the fix is to move the hashing — after which the choice of hash can be re-argued on evidence.

### String interning belongs in clean-core

**Decided.** The interner is a `cc::` facility, not a `vdoc` one.

It is general vocabulary — anything with symbolic identity wants it — and there is nothing document-specific about it.

**Built in milestone 0, and the handle carries a pointer rather than a numeric id.**
`cc::interned_string` holds the address of an entry that is never moved and never freed, so `as_string_view()` and byte-ordering are direct rather than a table lookup.
It also settles the "never serialize the raw id" rule by construction: there is no id to write down, only a private pointer.
Identity stays process-local, exactly as [concept.md](concept.md) says.

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

**Reopen when:** nothing.

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
