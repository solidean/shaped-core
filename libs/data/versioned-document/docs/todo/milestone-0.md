# Milestone 0 — Prerequisites in lower libraries

**Goal.** Land the three capabilities `vdoc` needs that do not exist yet, in the libraries that own them.

None of this is `vdoc` code.
Each item is an addition to a lower library, per the repo's standing preference for growing the lower library over hand-rolling locally — and each is wanted by more than just this one caller.

Nothing in milestones 1–6 can start without item A (hashing) and item B (interning).
Item C (SQLite) is only needed by milestone 4, and can be deferred within this milestone if it is convenient — but not past it.

---

## A. A cryptographic hash — `cc::blake3`

**Where.** `extern/blake3/` and `libs/base/clean-core/src/clean-core/common/`.

**Why.** Content addressing has to survive an adversary, or the trustless-recovery property in [concept.md](../concept.md#recovery-from-an-untrusted-peer) is a fiction.
`cc::hash128` is XXH3: excellent, and trivially collidable on purpose.
A replica cannot verify history received from a peer with a non-cryptographic hash.

### Work items

1. **Vendor BLAKE3**, on exactly the `extern/xxhash` model:
   - `extern/blake3/vendor-blake3.py` — a `uv`-run script with PEP 723 inline metadata, pinning an upstream commit.
     Mirror [extern/xxhash/vendor-xxhash.py](../../../../../extern/xxhash/vendor-xxhash.py).
   - `extern/blake3/CMakeLists.txt` defining a `blake3` target, plus `LICENSE`.
   - `SC_USE_VENDORED_BLAKE3` in [extern/CMakeLists.txt](../../../../../extern/CMakeLists.txt), defaulting ON, with the `add_subdirectory` guarded like its neighbours.
   - Vendored in-source rather than fetched-on-demand, because it is small and every build needs it.
     That is the xxhash tier, not the sqlite one.
2. **`cc::hash256`** next to [hash128.hh](../../../../base/clean-core/src/clean-core/common/hash128.hh), in the same shape.
   Structurally comparable, a `create(span<byte const>)` factory, and a `hash()` friend surfacing one limb so it works as a map key.
3. **`cc::blake3`** — a one-shot `create(span<byte const>) -> hash256`, plus a streaming state for hashing a byte sequence built in pieces.
   Streaming is how an op payload is hashed without concatenating it first.
4. **Record the decision at the source**, as a comment at `cc::blake3` pointing at [decisions.md](../decisions.md#blake3-over-32-byte-ids--with-a-standing-reservation).
   Why a second hash exists alongside XXH3, when to use which, and the standing reservation about its cost.
   Someone will eventually wonder why the repo has two hashes; the answer must be where they are standing.

### Tests

- Known-answer vectors from the BLAKE3 specification, including the empty input.
- Streaming in arbitrary chunk splits equals one-shot over the concatenation.
- `hash256` as a `cc::map` key, and its ordering.
- A benchmark alongside [hash-benchmark.cc](../../../../base/clean-core/tests/benchmarks/hash-benchmark.cc), reporting BLAKE3 next to XXH3 at the sizes that matter.
  This is what makes the reservation in [decisions.md](../decisions.md) checkable rather than a matter of opinion — record it, do not just observe it.

### Acceptance

- A fresh checkout configures and builds with the vendored BLAKE3.
- The known-answer vectors pass on every CI platform.
- The benchmark reports both hashes, so the 4× is a number in the repo rather than a claim in a document.

---

## B. Process-local interning — `cc::interned_string`

**Where.** `libs/base/clean-core/src/clean-core/string/`.

**Why.** `entity_id`, `component_type_id` and `property_id` are strings compared and hashed constantly on every query path.
Interning makes that a word comparison and stores each distinct string once.
Nothing about it is document-specific: any symbolic identity wants it.

### Work items

1. **The interner** — a global-by-default table mapping bytes to a stable id, with the option of a caller-owned table so tests can be isolated.
2. **`cc::interned_string`** — a small trivially-copyable handle with `as_string_view()`, equality, ordering and hashing.
   Ordering must be **by canonical bytes**, not by id, or every sorted structure built on it becomes run-dependent.
3. **The rule, enforced by documentation and by shape**: the raw id is process-local and must never be serialized, nor used to hash anything durable.
   Make the numeric id awkward to reach by accident.
4. **Thread safety** — interning from several threads is normal, and must not need external locking.
   Honour `CC_HAS_THREADS`: single-threaded builds pay nothing.

### Tests

- Interning the same bytes twice yields the same handle; different bytes never collide.
- Ordering follows byte order, and is stable across insertion orders — the property that keeps a document's sorted arrays deterministic.
- Concurrent interning from several threads produces one entry per distinct string.
- A single-threaded build behaves identically.

### Acceptance

- Ordering is provably independent of intern order.
- The threaded and single-threaded builds pass the same tests.

---

## C. `babel::sqlite` additions

**Where.** `libs/io/babel-serializer/src/babel-serializer/data/sqlite.{hh,cc}`.

**Why.** [format.md](../../../versioned-document-file/docs/format.md) needs three things the wrapper does not expose yet.
They are added to the wrapper, not reached around: `sqlite3.h` stays confined to that one TU, which is what keeps the engine out of everyone's public API.

### Work items

1. **Incremental blob I/O** — a handle over `sqlite3_blob_open` supporting reads at an offset.
   This is what lets a chunk be read without materializing the whole row, and it is why `blobs` is a rowid table.
   Writing through the handle can come later; reading cannot.
2. **Transaction scoping** — an RAII transaction that commits on success and rolls back otherwise.
   Publishing writes ops, refs, assets, blobs and chunks in **one** transaction; hand-rolling `BEGIN`/`COMMIT` around that is how a half-published file happens.
3. **Connection configuration** — journal mode (WAL), busy timeout, and `foreign_keys`.
   The cascade in `blob_chunk` is a correctness dependency on `foreign_keys`, so it must be settable and observable rather than assumed.
4. **Follow babel's own rules** — [its coding guidelines](../../../../io/babel-serializer/docs/coding-guidelines.md) own the header non-leak rule and the absent-backend behaviour.
   Everything added here must probe `is_available()` the same way the rest does.

### Tests

- In `babel-serializer`'s own suite, not `vdoc`'s.
  Incremental reads at offsets and across a chunk boundary; a transaction that commits.
  A transaction abandoned by an early return leaving no rows behind; cascade delete with `foreign_keys` on and off.
- The absent-backend path still reports cleanly for each new entry point.

### Acceptance

- No `sqlite3` type appears in any babel public header.
- A rolled-back transaction leaves the database byte-identical.
- `versioned-document-file` needs no sqlite include of its own, now or later — its [`.shaped-lint.yml`](../../../versioned-document-file/.shaped-lint.yml) stays empty.

---

## Notes for whoever does this

Three libraries are touched, so update each one's own docs in the same change.
That is clean-core's cheat-sheet for the two new `cc::` facilities, plus babel's cheat-sheet and [structure.md](../../../../io/babel-serializer/docs/structure.md) for the sqlite additions.

If any of these turns out to want a different shape than sketched here, that is a normal outcome.
Change the shape, and note it in [decisions.md](../decisions.md) if it changes something `vdoc` depends on.
