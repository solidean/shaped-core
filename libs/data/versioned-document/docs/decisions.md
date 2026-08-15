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

**Reopen when:** nothing.

### One actor owns the connection

**Decided.** A `cc::threaded_actor` holds the database handle exclusively, and results are pushed back as `cc::async` values.

A SQLite connection is not a shared resource, and the alternative — a mutex around a connection touched from arbitrary threads — puts I/O latency on whichever caller is unlucky.
An actor makes the ownership a fact of the structure rather than a convention, and gives the store one thread to serialize its writes on.

**Reopen when:** nothing.

### Blobs ship raw-only, with the encoding seam reserved

**Decided.** The `encoding` column and the encode/decode seam exist and are exercised from the start; `raw` is the only encoding registered in v1.

The seam is what costs nothing later — adding compression becomes a vendored dependency plus one codec registration, with no format change and no migration.
Vendoring a compressor before there is content to compress is work in the wrong order.

A file naming an unknown encoding is reported as a load issue and the blob is skipped, exactly like any other soft failure, rather than failing the open.

**Reopen when:** embedded content sizes justify it.
It is an additive change by construction, so nothing about the format has to move.

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
