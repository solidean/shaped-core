# Milestone 6 — Snapshots, pruning and recovery

**Goal.** Make materialization independent of history length, make history prunable, and make history received from an untrusted peer verifiable.

**Why last.** Every piece here is a *change to how* something already works rather than a new capability, and each is only correct if the slow, obvious version it replaces is already known-correct.
The milestone-2 replay is the oracle the snapshot cache is checked against; there is no way to write that check before it exists.

This is also the milestone that closes the documentation loop, so the design docs and the implementation stop being two separate claims.

Depends on milestones 2 through 5.

**Status: items 1 through 4 have landed.**
Snapshots, their persistence, pruning and skeleton ops are done and tested on both store arms.
What remains is item 5 (recovery), item 6 (the profiling check), and item 7 (the documentation loop) — and item 7 is where this folder stops existing.

Two of this milestone's own items were **wrong**, and were corrected while building them.
Both corrections are in [decisions.md](../decisions.md); the items below say what shipped rather than what was first specified.

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

**Two things items 1 through 4 left in the way, both found while building them:**

- **`op_graph::add` is idempotent by id, so a skeleton cannot be upgraded to a full op.**
  Re-adding a hash already present changes nothing, deliberately — but that is exactly what receiving the payload for an op this replica holds as a skeleton has to do.
  So integration needs its own verb rather than a loop of `add`, and that verb is what verifies before it accepts.
- **Un-pruning invalidates the boundary a required snapshot was written under.**
  A required snapshot carries no `superseded` because everything behind it is payload-free; restoring a payload behind one puts a live writer back where nothing can suppress it.
  Integrating a batch must therefore recompute or demote any required snapshot whose past it just filled in.
  That is the same failure item 3 refuses to create in the first place, arriving from the other direction.

Take the batch as a set rather than op by op: the replica needs one point at which it either accepted the whole set or changed nothing.

### 6. Check the hashing reservation, on evidence

[decisions.md](../decisions.md#blake3-over-32-byte-ids--with-a-standing-reservation) records a standing reservation about BLAKE3's cost, with a testable condition attached.
Hashing must never appear in a profile of an ordinary open / edit / save loop.

**Verify it here, now that a real loop exists**, using [`dev.py profiling`](../../../../../docs/guides/profiling.md) over open → edit → publish → close on a document of realistic size.

Record the numbers in the guide-benchmark form, so the answer lives in the repo rather than in someone's memory.

If hashing *does* show up, the conclusion is not "the reservation was wrong" — it is that hashing ended up somewhere it does not belong.
Find where, move it, and re-measure.
Only then is the choice of hash itself worth re-arguing.

### 7. Close the documentation loop, and retire this folder

The docs were written to describe something being built.
Once it is built, that shape is wrong: a plan and a set of milestone files are scaffolding, and scaffolding left standing is read as part of the building.

**This item ends with `docs/todo/` deleted.**
Nothing is preserved from it except what is already true elsewhere — a milestone file is a record of intent, and git holds that.
Anything in here that is still *load-bearing* is a decision that was never written down, and the fix is to write it into [decisions.md](../decisions.md) before the file goes, not to keep the file.

#### Split `concept.md` into `docs/concepts/`, one file per major concept

[concept.md](../concept.md) is ~550 lines across ten top-level sections, and it is the document everyone is told to read first.
At that size "read this before starting" stops being an instruction anyone follows.
The system is large enough that per-concept granularity is the right unit, and [clean-core's `docs/systems/`](../../../../base/clean-core/docs/systems/) is the shape to follow.

Its current sections are close to the file boundaries already:

the model, values, ops and content addressing, multi-values, interpretation,
the typed document, assets and blobs, validation layers, compatibility and pruning,
and what lives outside the document.

Treat that as a starting point rather than a specification — the split should follow what a reader comes looking for, not what the headings happen to be today.

What replaces `concept.md` is an index, not a summary: one line per concept saying what question it answers.
A reader who wants the whole design still reads everything, but a reader who wants to know how multi-values resolve reads one file.

**Reconcile as you split, rather than after.**
Where a concept doc and the implementation disagree, the implementation is not automatically right — decide which one is, and change the other deliberately.
Moving a section is the moment you actually reread it, so it is the cheapest moment to catch a claim that stopped being true three milestones ago.
Three known ones to check: snapshots (they no longer resemble what `## Compatibility, pruning and recovery` describes), pruning's boundary rule, and whatever milestone 5 changed about assets.

#### The rest of the loop

- Flip every remaining `[planned]` in [structure.md](../structure.md), and decide whether that file survives at all.
  It is a status tracker, and status trackers are the other thing that stops being true quietly.
  If it stays, it stays as a map of what lives where rather than as a roadmap.
- Check every cheat-sheet entry against what actually shipped, on both libraries.
- Add anything learned to [decisions.md](../decisions.md), including decisions that were **reversed**.
  A reversal with its reasoning is worth more than a clean-looking list, and this milestone alone produced three.
- Update `readme.md` for both libraries, since they stop being "at the design stage".
- Point everything that cited a milestone file at whatever now owns that answer.
  A dangling `todo/milestone-N.md` link is the failure mode of deleting this folder, and `dev.py check` catches it — so run it before assuming the deletion is clean.

**Deleting this folder is not a tidy-up to do last and fast.**
It is the step that decides which of the reasoning written here survives, and the only one whose mistakes are invisible afterwards.

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
- **Skeleton upgrade**: integrating the payload for an op held as a skeleton fills it in, where a plain `add` would not.
- **Un-pruning is not silently unsound**: integrating a batch behind a required snapshot leaves that snapshot correct, or demoted, and never quietly wrong.
- **Profiling**: the open / edit / save loop is measured and recorded, per item 6.

## Acceptance

- **[done]** A snapshot is provably indistinguishable from the replay it replaces, over a generated corpus.
- **[done]** A required snapshot cannot be dropped silently.
- **[done]** A skeleton op is never reported as a hash mismatch, in any path.
- **[done]** Pruning cannot produce a document whose merges are wrong, and refuses rather than half-doing it.
- History from an untrusted peer is verified by recomputation, and a tampered set is rejected without damage.
- The hashing reservation has been checked against a real profile, and the numbers are in the repo.
- Both cheat-sheets match what shipped, and no `[planned]` marking remains anywhere in either library's docs.
- **`docs/todo/` is gone**, every reference to it has an owner elsewhere, and `dev.py check` passes with no dangling link.
