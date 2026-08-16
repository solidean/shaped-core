# Milestone 6 — Snapshots, pruning and recovery

**Goal.** Make materialization independent of history length, make history prunable, and make history received from an untrusted peer verifiable.

**Why last.** Every piece here is a *change to how* something already works rather than a new capability, and each is only correct if the slow, obvious version it replaces is already known-correct.
The milestone-2 replay is the oracle the snapshot cache is checked against; there is no way to write that check before it exists.

This is also the milestone that closes the documentation loop, so the design docs and the implementation stop being two separate claims.

Depends on milestones 2 through 5.

---

## Work items

### 1. Snapshot-terminated materialization

**[done]**

Cache a materialized `raw_document` against an op id.
Materializing walks history until it meets a cached snapshot, and stops there.

Real editing produces mostly-linear histories, so this turns "replay everything since the document was created" into "replay the last few ops".
That is the difference between a document that stays fast for years and one that does not.

- The cache is in-memory and derived, so it must be safe to drop entirely at any moment.
- **A snapshot must be indistinguishable from the replay it replaces.**
  That is the property everything else here rests on, and it is what the tests below check exhaustively.

**This item originally specified storing the full materialization state, `superseded` included.**
**That was wrong, and the rest of this section is the corrected design.**
The reasoning behind the correction is [decisions.md](../decisions.md#a-snapshot-stores-surviving-only-and-its-validity-is-decided-at-use); what follows is what shipped.

**A snapshot stores `surviving` only** — exactly a `raw_document`, over bytes it owns, since a pruned op's payload is gone.

Storing `superseded` was rejected on size: it totals `32 B × (all historical assignments − distinct paths)`, so a snapshot would grow with the history it exists to replace.

**`surviving(X)` is a function of X's own causal past alone**, because state flows forward and the state at X never depends on anything outside X's ancestry.
The articulation-point clear cannot corrupt it either.
So any op may be snapshotted, from any sweep, and there is no eligibility question at creation.

**The original analysis was right that the articulation-point rule does not survive being stored** — that part stands.
A rule justified by "no op *already in this DAG* can present a stale branch" cannot be recorded and trusted later, when the DAG has grown.
The fix is not to store more, but to **re-check the same property against today's DAG, at use time**:

1. Walk back from the heads, treating any op the cache holds as a terminator whose parents are not expanded.
2. The walked set is then parent-closed except at terminators, so its sources are the in-degree-0 set Kahn already computes.
3. **Seed a snapshot only where there is exactly one source and it is that snapshot.** Everything else replays.

**"Every source is cached" is unsound and must not creep back in.**
Materializing `{T, X}` with X a distant ancestor of T gives two cached sources, and unions `{X}` with `{T}` into a multi-value nobody wrote.

A second condition is easy to miss: **no op other than the seed may have a parent that is in the graph but outside the walk**, or it replays from a partial ancestry.

Two more things that must not creep back in:

- A seeded op's own assignments are **not** re-applied.
  `surviving(T)` already holds T's writes, so applying them again moves T into `superseded` while it is also in `surviving`, and the next merge drops them.
- A **filtered** result is a projection, not `surviving(head)`, and installing one silently truncates every later sweep that terminates there.

`required = 0` versus `required = 1` is therefore purely lifetime and failure severity, not a difference in representation.

### 2. Persisted snapshots

**[done]**

The `snapshots` table from milestone 4, now populated.

- `required = 0` — a droppable cache, so deleting the row still leaves the document materializable, just more slowly.
- `required = 1` — **load-bearing**: the history behind this op is gone, and deleting the row destroys data.
- A droppable snapshot that will not decode is a load issue; a *required* one that will not decode is a hard failure, because what it stood in for no longer exists.

The distinction has to be visible everywhere a snapshot is touched.
A tool that prunes caches to save space must not be able to delete a required one by accident — which is why a required snapshot is **pinned** in the cache, so `clear_unpinned()` cannot reach it.

Departures from what this item assumed, all in [format.md](../../../versioned-document-file/docs/format.md#snapshots--materialization-caches):

- **The payload is chunked**, in `snapshot_chunk` cascading off `snapshots`, because SQLite caps a single value near a gigabyte.
  It is deliberately not a blob — [decisions.md](../decisions.md#snapshot-bytes-get-their-own-chunk-table-and-share-the-blob-codec) says why.
- **`snapshot_entry` carries no bytes.**
  A decodable snapshot is already in the cache and an undecodable one is bytes nobody can use, so holding either would mean a second resident copy of something that runs to gigabytes.
  A row this build cannot read still round-trips untouched, because publishing only ever inserts.
- **Persisting is explicit**, via `publish_snapshots`, and never a side effect of `publish` — which would make publishing non-idempotent.

### 3. Pruning

**[done]**

Attach a snapshot to an op, mark it required, delete the ops behind it.

What remains where an op was is a **skeleton op**: its id and its parents, with no payload.
The DAG keeps its shape, so reachability, merges and child walks all still work; only the content is gone.

**Deleting the row instead would change what the document says.**
Ancestry is defined over ops that are present, so removing a mid-history op severs the path through it.
Two writes that were ordered become concurrent, and materialization manufactures a multi-value nobody authored.
That is the concrete reason the skeleton exists, and it is a semantic change caused by a storage-layer operation — which is why it gets a test rather than a comment.

Trade-offs, stated where a user can see them: less storage and faster loading, against losing deep history and shortening the range over which two replicas can still synchronize.
Pruning is always optional, and never automatic.

**How far a document may prune was not obvious, and the answer is stricter than this item assumed.**
`prune` refuses unless **every ref descends from the prune point**, naming the ref that blocked it.
A ref that forked earlier keeps its own ancestors, so it still offers writers that the emptied ops superseded, and a required snapshot has no superseded set to suppress them with.
Replaying instead reads the emptied ops as silent.
[decisions.md](../decisions.md#a-snapshots-empty-superseded-is-what-bounds-how-far-history-may-be-pruned) carries the argument.

Pruning is a **sixth store hook** rather than a mode of publishing: a publish only ever appends and is idempotent by content addressing, while a prune destroys.

### 4. Skeleton ops, and the false alarm that must not happen

**[done]**

A skeleton has **no bytes to hash**, so it is unverifiable *by construction*.

Verification must report that as its own outcome — unverifiable — and **never as a hash mismatch**.
A mismatch means corruption or tampering, which is exactly the thing the whole content-addressing scheme exists to detect.
Reporting a routinely-pruned op as tampering would train everyone to ignore the one alarm that matters.

Most of this already landed in milestone 2: `op::is_skeleton`, `verify_op` returning `unverifiable`, and a load path that files `op_hash_mismatch` only from a genuine decode failure.
What this milestone adds is `op_graph::skeletonize`, so a prune takes effect without a reopen, and the test that pins the two outcomes apart on one document that is both pruned *and* corrupt.

### 5. Recovery from an untrusted peer

Because an op id recursively commits to everything behind it, a replica missing history can accept it from anyone.

The procedure, and it is short: receive the ops, recompute their hashes, compare against the ids already expected.
If they match, the history is correct — no trust in the sender at any point.

- An op whose recomputed hash does not match is rejected, and the rejection names the op.
- A partial or hostile set is rejected without corrupting what the replica already had.
- Verification is over the bytes as received; nothing is re-serialized on the way in.

This is what BLAKE3 is for, and this milestone is where that decision earns its keep — or fails to.

### 6. Check the hashing reservation, on evidence

[decisions.md](../decisions.md#blake3-over-32-byte-ids--with-a-standing-reservation) records a standing reservation about BLAKE3's cost, with a testable condition attached.
Hashing must never appear in a profile of an ordinary open / edit / save loop.

**Verify it here, now that a real loop exists**, using [`dev.py profiling`](../../../../../docs/guides/profiling.md) over open → edit → publish → close on a document of realistic size.

Record the numbers in the guide-benchmark form, so the answer lives in the repo rather than in someone's memory.

If hashing *does* show up, the conclusion is not "the reservation was wrong" — it is that hashing ended up somewhere it does not belong.
Find where, move it, and re-measure.
Only then is the choice of hash itself worth re-arguing.

### 7. Close the documentation loop

- Flip every remaining `[planned]` in [structure.md](../structure.md).
- Remove the `[planned]` banners from both cheat-sheets, and check every entry against what actually shipped.
- Reconcile [concept.md](../concept.md) with what was built.
  Where the two differ the implementation is not automatically right — decide which one is, and change the other deliberately.
- Add anything learned to [decisions.md](../decisions.md), including decisions that were *reversed*.
  A reversal with its reasoning is worth more than a clean-looking list.
- Update `readme.md` for both libraries, since they stop being "at the design stage".

## Tests

Items 1 through 4 are done; the rest wait on items 5 and 6.

- **[done] Snapshot equivalence, exhaustively**, over a generated corpus of DAGs: linear, branching, diamonds, merges, multi-valued properties.
  Materializing with the cache must equal materializing without it, byte for byte.
  Generate the corpus rather than hand-writing it; this is the property everything else in the milestone depends on.
  The corpus is checked against milestone 2's brute-force oracle *before* any cache test runs.
  Every equivalence case also asserts the cache was actually used, since a green run over a cache nobody consulted proves nothing.
- **[done] Cache-dropping is invisible**: drop the cache at arbitrary points mid-workload and the results never change.
- **[done] Required versus droppable**: deleting a droppable snapshot changes nothing but speed.
  One that will not decode is an issue naming its op, while a *required* one that will not decode fails the open outright.
- **[done] Pruned history**: a pruned document materializes to exactly what it did before pruning.
  Asserted again after a close and reopen, which is the leg that proves the encoding round-trips rather than just the cache.
- **[done] A required snapshot is load-bearing**: replaying a pruned document *without* it is asserted to differ, because that is what `required` means.
- **[done] Skeleton ops** report unverifiable, never mismatch — asserted for a document that is pruned *and* has a genuinely corrupt op, so the two outcomes are distinguished in one file.
- **[done] The prune boundary is enforced**: pruning past a ref that forked earlier is refused, nothing is written, and pruning at a point every ref descends from is allowed.
- **[done] Two replicas pruned to different depths agree**, each through its own snapshot.
- **[done] The snapshot codec on its own**: canonical output, every truncated prefix refused, trailing bytes refused, and arbitrary garbage decoded to an error rather than a crash.
- **Recovery**: reconstruct pruned history from a second replica and verify by recomputation; a tampered op in the received set is rejected by id and leaves the replica intact.
- **Profiling**: the open / edit / save loop is measured and recorded, per item 6.

## Acceptance

- A snapshot is provably indistinguishable from the replay it replaces, over a generated corpus.
- A required snapshot cannot be dropped silently.
- A skeleton op is never reported as a hash mismatch, in any path.
- History from an untrusted peer is verified by recomputation, and a tampered set is rejected without damage.
- The hashing reservation has been checked against a real profile, and the numbers are in the repo.
- No `[planned]` marking remains anywhere in either library's docs, and both cheat-sheets match what shipped.
