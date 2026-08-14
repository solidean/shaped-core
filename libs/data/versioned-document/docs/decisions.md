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

### No hash or reference type in the codec

**Decided.** Asset references are ordinary strings.

A file is only one source of assets; built-in, procedural, remote and cached assets resolve through completely different machinery, and a string is the only identifier all of them can share.
A dedicated reference type would imply the document knows how references resolve, which is precisely the coupling being avoided.

**Reopen when:** nothing foreseeable.

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

**Reopen when:** BLAKE3 shows up in a profile of an ordinary open / edit / save loop.
If it does, the design put hashing somewhere it does not belong, and the fix is to move the hashing — after which the choice of hash can be re-argued on evidence.

### String interning belongs in clean-core

**Decided.** The interner is a `cc::` facility, not a `vdoc` one.

It is general vocabulary — anything with symbolic identity wants it — and there is nothing document-specific about it.

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
