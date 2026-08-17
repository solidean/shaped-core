# Concept: pruning and recovery

History may be discarded, and history may be taken back from anyone.
The two are the same boundary seen from opposite sides, which is why they share a page.

Compatibility across builds and across applications is the third member of that family, and it has its own document: [compatibility.md](../compatibility.md).

## Pruning

Attach a [snapshot](snapshots.md) to an op, mark it required, and delete the ops behind it.

What remains where an op was is a **skeleton op**: its id and its parents, with no payload.
The DAG keeps its shape, so reachability, merges and child walks all still work; only the content is gone.

**Deleting the row instead would change what the document says.**
Ancestry is defined over ops that are present, so removing a mid-history op severs the path through it.
Two writes that were ordered become concurrent, and materialization manufactures a [multi-value](multi-values.md) nobody authored.
That is a semantic change caused by a storage-layer operation, which is why the skeleton exists and why it has a test rather than a comment.

The trade-off, stated where a user can see it: less storage and a faster load, against losing deep history and shortening the range over which two replicas can still synchronize.
Pruning is always optional, and never automatic.

### Dropping a leaf is the opposite situation, and looks the same

`op_graph::drop_leaf` removes an op outright — id, payload and parent edges — where `skeletonize` keeps all three but the payload.
The similar shape hides that they are for opposite cases.

A skeleton exists because **ancestry must survive**: something descends from the pruned op, and severing the path through it would turn ordered writes into concurrent ones.
`drop_leaf` is for an op with **no ancestry to preserve**, because nothing came after it and nothing outside has seen it — a [discarded editing frame](workloads.md#continuous-editing-goes-wide).
It asserts if the op has children, so the two can never be confused at a call site.

Forgetting is safe rather than lossy only because the DAG is content-addressed: the op is a pure function of its content, so if it ever comes back, `add` recreates it byte-identically.
That is also why it needs no place in the file format — it is an in-memory operation on ops that were never published.

### How far a document may prune

Further than the obvious answer, and less far than the convenient one.

A required snapshot carries no `superseded` set.
That is sound only while nothing can present a writer from behind it.
Ops behind the boundary are skeletons, which carry no assignments, so a branch arriving *through* one contributes nothing to resurrect.

**But a ref that forked before the boundary breaks that**, and it was a test that found it rather than reasoning.
Such a branch keeps its own ancestors, because they are history it still needs.
So it does still offer writers that ops behind the boundary superseded, and merging the two fabricates a multi-value.
Replaying instead is no escape: the ops that would have suppressed it are skeletons by then, so the replay is *lossy* rather than merely slow.

Both paths are wrong, which means the prune itself was the error.
So pruning **refuses unless every ref descends from the prune point**, and names the ref that blocked it.
The boundary a document may prune to is the oldest op every ref still descends from.

[decisions.md](../decisions.md#a-snapshots-empty-superseded-is-what-bounds-how-far-history-may-be-pruned) carries the argument and what would reopen it.

### A skeleton is unverifiable, never a mismatch

A skeleton has **no bytes to hash**, so it is unverifiable *by construction*.

Verification reports that as its own outcome — unverifiable — and **never as a hash mismatch**.
A mismatch means corruption or tampering, which is exactly the thing content addressing exists to detect.
Reporting a routinely-pruned op as tampering would train everyone to ignore the one alarm that matters.

## Recovery from an untrusted peer

Because an op id recursively commits to everything behind it, a replica missing history can accept it from anyone at all.

The procedure, and it is short: receive the ops, recompute their hashes, compare against the ids already expected.
If they match, the history is correct — no trust in the sender at any point.

- Verification is over the bytes **as received**; nothing is re-serialized on the way in.
- An op whose recomputed hash does not match is rejected, and the rejection names it.
- **The batch is a set.** Every op is verified before any of it is applied, so a partial or hostile set leaves the replica exactly as it was rather than half-integrated.

A skeleton cannot be *offered*: an empty payload has nothing to verify, and it is refused as malformed rather than quietly accepted as an op with no writes.
A skeleton the replica *holds* is filled in, which is the whole point — and it needs its own verb, because adding an op is idempotent by id and therefore leaves a skeleton a skeleton.

One integrity claim content addressing cannot make becomes checkable exactly here.
A skeleton's parents were never covered by any hash, so a corrupted parent list is undetectable while the op sits in storage.
Integration compares the received op's parents against the ones the replica holds, and refuses if they disagree.

### The prune boundary, from the other side

Filling history back in behind a required snapshot arrives at the same hazard pruning refuses to create.

An op that **forks below** a still-required snapshot presents a writer the emptied ops superseded, and the snapshot has no `superseded` set to suppress it with.
So such a batch is refused.

But completing that snapshot's ancestry retires the question entirely: once every op behind it has its payload again, a replay reproduces exactly what the snapshot holds.
The snapshot stops being load-bearing, and is **demoted** to a droppable cache and unpinned.

So a fork is refused only while the hole is still there, and sending the rest of the ancestry in the same batch is what makes it acceptable.
That is what makes this a boundary rather than a ban.
