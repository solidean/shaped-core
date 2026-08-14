# Milestone 6 — Snapshots, pruning and recovery

**Goal.** Make materialization independent of history length, make history prunable, and make history received from an untrusted peer verifiable.

**Why last.** Every piece here is a *change to how* something already works rather than a new capability, and each is only correct if the slow, obvious version it replaces is already known-correct.
The milestone-2 replay is the oracle the snapshot cache is checked against; there is no way to write that check before it exists.

This is also the milestone that closes the documentation loop, so the design docs and the implementation stop being two separate claims.

Depends on milestones 2 through 5.

---

## Work items

### 1. Snapshot-terminated materialization

Cache a materialized `raw_document` against an op id.
Materializing walks history until it meets a cached snapshot, and stops there.

Real editing produces mostly-linear histories, so this turns "replay everything since the document was created" into "replay the last few ops".
That is the difference between a document that stays fast for years and one that does not.

- The cache is in-memory and derived, so it must be safe to drop entirely at any moment.
- **A snapshot must be indistinguishable from the replay it replaces.**
  That is the property everything else here rests on, and it is what the tests below check exhaustively.

### 2. Persisted snapshots

The `snapshots` table from milestone 4, now populated.

- `required = 0` — a droppable cache, so deleting the row still leaves the document materializable, just more slowly.
- `required = 1` — **load-bearing**: the history behind this op is gone, and deleting the row destroys data.
- A droppable snapshot that will not decode is a load issue; a *required* one that will not decode is a hard failure, because what it stood in for no longer exists.

The distinction has to be visible everywhere a snapshot is touched.
A tool that prunes caches to save space must not be able to delete a required one by accident.

### 3. Pruning

Attach a snapshot to an op, mark it required, delete the ops behind it.

What remains where an op was is a **skeleton op**: its id and its parents, with no payload.
The DAG keeps its shape, so reachability, merges and child walks all still work; only the content is gone.

Trade-offs, stated where a user can see them: less storage and faster loading, against losing deep history and shortening the range over which two replicas can still synchronize.
Pruning is always optional, and never automatic.

### 4. Skeleton ops, and the false alarm that must not happen

A skeleton has **no bytes to hash**, so it is unverifiable *by construction*.

Verification must report that as its own outcome — unverifiable — and **never as a hash mismatch**.
A mismatch means corruption or tampering, which is exactly the thing the whole content-addressing scheme exists to detect.
Reporting a routinely-pruned op as tampering would train everyone to ignore the one alarm that matters.

Milestone 2 already established this outcome; here it stops being hypothetical.

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

- **Snapshot equivalence, exhaustively**, over a generated corpus of DAGs: linear, branching, diamonds, merges, multi-valued properties.
  Materializing with the cache must equal materializing without it, byte for byte.
  Generate the corpus rather than hand-writing it; this is the property everything else in the milestone depends on.
- **Cache-dropping is invisible**: drop the cache at arbitrary points mid-workload and the results never change.
- **Required versus droppable**: deleting a droppable snapshot changes nothing but speed; deleting a required one is detected and reported as a hard failure, not silently tolerated.
- **Pruned history**: a pruned document materializes to exactly what it did before pruning.
- **Skeleton ops** report unverifiable, never mismatch — asserted for a document that is pruned *and* has a genuinely corrupt op, so the two outcomes are distinguished in one file.
- **Recovery**: reconstruct pruned history from a second replica and verify by recomputation; a tampered op in the received set is rejected by id and leaves the replica intact.
- **Merge across a prune boundary**: two replicas that pruned to different depths still merge correctly, which is the case the whole skeleton mechanism exists for.
- **Profiling**: the open / edit / save loop is measured and recorded, per item 6.

## Acceptance

- A snapshot is provably indistinguishable from the replay it replaces, over a generated corpus.
- A required snapshot cannot be dropped silently.
- A skeleton op is never reported as a hash mismatch, in any path.
- History from an untrusted peer is verified by recomputation, and a tampered set is rejected without damage.
- The hashing reservation has been checked against a real profile, and the numbers are in the repo.
- No `[planned]` marking remains anywhere in either library's docs, and both cheat-sheets match what shipped.
