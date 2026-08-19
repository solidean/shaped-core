# Concept: ops and content addressing

The document [the model](the-model.md) describes is a **materialized view**.
It is not what is stored.

What is stored is an immutable, content-addressed DAG of **ops**.
An op is a set of property assignments, plus its parents, plus free-form metadata.

```text
op {
    id:      op_id                            a 32-byte BLAKE3 digest of everything below
    parents: [op_id, ...]                     verbatim, in the op's own order
    payload: optional {                       absent only on a skeleton op left behind by pruning
        metadata_bytes:    an encoded value   author, timestamp, description — informational only
        assignment_bytes:  a tag byte, then the assignments
    }
}
```

**An op holds the producer's bytes, and nothing else.**
`metadata` and `assignments` are decoded *views* over those bytes, produced on demand and stored nowhere.
That is a correctness property rather than a memory optimization — [the op is its bytes](#the-op-is-its-bytes) is why.

Metadata never affects document semantics.
It is committed to by the hash — so it cannot be altered after the fact — but nothing interprets it.

## The producer canonicalizes; the hash just hashes bytes

`op_builder` sorts, deduplicates and serializes; the resulting bytes are the op's payload; `op_id` is a plain BLAKE3-256 hash of those exact bytes.

**No load path ever re-serializes.**
Verification re-hashes the bytes as stored and compares.

That is a correctness property, not an optimization.
If verification re-serialized, any change to a formatter, an integer width or a map iteration order would turn a good stored op into a hash mismatch.
A mismatch is indistinguishable from tampering.
With this rule, a re-serialized payload is simply a *different op*, which is exactly what it is.

The hash input, all integers fixed-width little-endian:

```text
u64 length | "vdoc::op/v1"                  domain separation, so an op id can never collide with any other digest we compute
u32 parent count | each parent's 32 bytes    verbatim, in the op's own order
u64 length | metadata bytes
u64 length | assignments bytes
```

The assignment payload opens with a **one-byte encoding tag**, so the assignment encoding can evolve without changing the hashing rule.
An unknown tag is a decode error naming the tag, not a corruption report.

## An assignment either writes or abstains

Each assignment record opens with an **`assignment_kind` byte** saying what it does to its path:

| kind | meaning |
|---|---|
| `set` | writes a value, which follows in the record |
| `abstain` | withdraws this history's contribution to the path, and carries no value at all |

An abstention supersedes its ancestors' writes exactly as a write does, and then contributes nothing — so the path ends up **absent** rather than holding some other value.
It is the only way to un-write a property: `$alive` removes a whole component or entity, and nothing else removes a single one.

**It is transparent, never masking.**
It withdraws this history's opinion, and cannot hide someone else's.

An abstention encodes no value rather than a `null`, because two spellings of one thing is exactly the canonicality problem the whole format is built to avoid.

The builder's canonical order is: parents sorted and deduplicated, assignments sorted by `(entity, component, property)`, and no path assigned twice within one op.
Identical content therefore produces an identical `op_id`, whatever order the caller supplied.

Sorting is by **canonical bytes** throughout — the id strings, and for parents the 32 digest bytes.
Neither an interned id nor a digest's in-memory representation may order anything that reaches the hash, or two machines would disagree on the same content.

BLAKE3 is the hash because content addressing has to survive an untrusted sender — [recovery](pruning-and-recovery.md#recovery-from-an-untrusted-peer) is what that buys.
[decisions.md](../decisions.md#blake3-over-32-byte-ids--with-a-standing-reservation) carries the choice, the reservation attached to it, and the measurements that tested it.

## The op is its bytes

An op retains what it was read as, and decodes on demand.
It does not keep a decoded assignment list beside those bytes, because holding both means holding two representations that can disagree.

The rule above is what forces this.
If an op held only decoded assignments, verification would have no choice but to hash `encode(decode(bytes))`.
That is re-serialization under a different name, with every failure mode the rule exists to prevent.
Keeping the bytes makes the guarantee structural: there is no encoder anywhere near a loaded op, so no future change to one can reach it.

Three things follow, and the third is the one worth reading twice:

- **An op *could* be stored and relayed without being interpretable.** Byte retention is what would let a build hand on an op whose assignment encoding it cannot read.
  Today it does not: an unknown assignment encoding tag is a decode error.
  [decisions.md](../decisions.md#an-unknown-assignment-encoding-tag-is-a-decode-error) records why that door is left open rather than walked through.
- **Write-back is lossless by construction.** Nothing is dropped on save, because nothing was rewritten.
- **An op assigning to component types this build has never heard of round-trips byte-identically.**
  Not by convention, and not because some layer takes care to preserve it — there is simply no code path that could alter it.
  This is the mechanism underneath cross-version and cross-application compatibility, and [compatibility.md](../compatibility.md) is where the consequences are worked out.

## Ops write only what changed

`op_builder::build` materializes the touched entities as seen from the op's own parents, and emits an assignment only where the desired value differs from the current one.

So re-setting an unchanged component produces an op with no assignments, and setting one field of a ten-field component writes one property.
This is what keeps history small enough to keep forever, and it is why an op's assignment list is a genuine changelog rather than a snapshot.

## The DAG

Each op names zero or more parents.

| parents | meaning |
|---------|---------|
| none    | a new document |
| one     | extends that history |
| several | merges those histories |

A document is identified by a single op id — its **head**.
Named heads are **refs**, which live in storage rather than in the model.

Materializing a head means walking its transitive parents, collecting assignments, and resolving overwrites.
Every op id resolves deterministically to exactly one document state, forever.

A naive materialization replays everything reachable, which is linear in history and therefore unbounded over a document's life.
[Snapshots](snapshots.md) are what bound it.

## Overwrites and dominance

If op B is a descendant of op A and both write the same path, B wins.
This is ordinary last-write-wins along a line of history, and it covers almost every write that ever happens.

What happens when neither dominates is [multi-values](multi-values.md), and it is the more interesting half.
