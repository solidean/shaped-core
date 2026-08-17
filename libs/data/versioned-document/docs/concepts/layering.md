# Concept: layering

An op graph describes one document.
Sometimes what you have instead is a **changeset over something that changes independently** — and that cannot be one graph.

The case that drove this is a three-level stack:

- a C++ side computes its own document, regenerated every frame;
- a user edits on top in UI, and those edits must be rebased onto the new frame every frame;
- a third C++ layer forces runtime values over the user's edits.

One graph cannot express it.
The per-frame base is a new op every frame, so the user's edits would change the identity of the whole graph every frame, because their parent would be different.

A **`layer_stack`** composes several independent histories — each with its own ops, hashes and snapshots — into one document.

```text
layer 2   forced      →  ─┐
layer 1   user edits   →   ├─  one ordinary vdoc::document
layer 0   per frame    →  ─┘
```

## A higher layer replaces a lower one per property path

That sentence is the whole feature, and both halves of it are load-bearing.

**Per property path**, because anything coarser breaks the case above.
If the base animates a transform and the user overrode only `position`, component-granular replacement would freeze `rotation` at the moment the edit was made.
The animation would stop applying, which is exactly the rebasing layering exists for.
So property granularity is not a refinement here — it is the requirement.

[The model](the-model.md) already names the property path as the addressable unit of the whole system.
Layering is the fourth item on that list, after ops, conflicts and diffs.

**Replaces, never merges.**
Where a layer wins a path, its *entire* writer list replaces the lower one.

So layering is the **totally ordered, conflict-free** composition, in deliberate contrast to a DAG [merge](multi-values.md), which is the partially ordered conflict-ful one.
A conflict can only ever be layer-local.
A contested path inside the winning layer still reaches `parse_policy` exactly as it would unlayered, and no policy ever has to choose between two layers.

## It composes below the typed layer

The composition happens on `raw_document`s and produces **one ordinary `document`**.

That follows from property granularity rather than being a separate choice.
A typed `document` holds component structs in dense columns, so the only thing a higher layer could do to a `my_transform` is replace it whole — which is what property granularity forbids.
So the merge has to happen below the typed layer, and once it does, materializing a single typed document costs nothing extra.

The payoff: **nothing downstream of `composed()` learns that layering exists.**
The dense columns, `each<A, B>`'s leapfrog join, immutability and the safe-to-hand-to-another-thread guarantee are all unchanged.

**The composition unit is the entity**, because that is what the selection phase takes.
`impl::select_entity` is handed one `raw_entity`, so composing at that granularity lets a layered parse reuse the ordinary selection and construction code.
Growing a second copy of it is the drift that factoring selection out for the incremental apply already avoided once.

## What a layer is

| kind | version | typical use |
|---|---|---|
| graph-backed | its head `op_id` | the user's edits, with real history, undo and sync |
| directly written | a monotonic counter | a per-frame computed base, or runtime forced values |

A [`direct_layer`](../../src/versioned-document/direct_layer.hh) is written property by property and owns its bytes.

**Its writes are diffed**, so a producer may re-write everything every frame and still hand over a small dirty set.
`set` compares the new bytes against what is stored, trading O(n) re-interpretation for O(n) property writes.

`begin_rebuild` / `finish_rebuild` let such a producer express *removal* by simply not writing a path again.
`mark_dirty` skips even the compares, for a producer that already knows what moved.

**A wholesale rebuild has a scale limit, and it is worth knowing before relying on one.**
Each write walks three nested sorted vectors with an interned-string comparison at every step, at roughly 100 ns.
So rewriting 8,000 entities of 7 properties is about 6 ms — most of a frame — while 2,000 is 1.3 ms and 500 is 0.3 ms.
Past a couple of thousand entities a producer should write only what moved.
[The benchmark](../benchmarks/edit-latency-benchmark.md#layering-per-frame) is where those numbers live, and it is what would catch them regressing.

Its values are attributed to a **synthetic writer id** derived from the layer's name, domain-separated so it cannot collide with a real op id.
That keeps writer sorts and diagnostics reproducible across runs.
It is also why a composed document must never be installed into a `snapshot_cache`: those ids name no op.

## The stack pulls every delta

`layer_stack::apply` is not told what changed.
It holds each graph layer's head — `set_head` is the only way to move one — and each direct layer bumps its own version from inside its mutators.

So it derives every layer's delta itself, and **"the caller forgot to mention a layer moved" is not expressible** rather than merely detected.
That was the alternative design: annotate a change set with per-layer op movements, and validate them on the way in.
Pull makes the bug unreachable instead, which is strictly better than catching it.

What the stack cannot always derive is *how* a layer moved.
A merge on a graph layer's chain, a chain past the bound, or muting a layer each falls back to a full recompose.
That is correct and slow, and `layered_apply_stats::fallback_reason` says which happened.

Per frame the cost is then **O(dirty entities × layers)** rather than O(document).
That is the same gate [workloads](workloads.md) sets for a single graph.

## Reserved properties compose too

`$alive` composing per path is **intended**.
A higher layer can revive an entity the base killed, or kill one it created, which is what a forced-values layer needs.

Cross-layer it is fully deterministic and higher-wins, because replace-not-merge means only the winning layer's writer set is ever seen.
So the "contested stays alive" rule never fires *across* layers.
Withdrawing a deletion means [abstaining](ops-and-content-addressing.md#an-assignment-either-writes-or-abstains) the `$alive` path itself.
Writing `true` instead would pin it alive against the base, which is a different statement.

`$schema_version` composing per path is a **hazard**, and it is the default rather than an edge case.
`op_builder::set` always stamps, so every layer written through the typed API carries a version, and the topmost stamp would describe only its own properties.

Two ways it bites, and the second is worse:

- the base stores v2-shaped properties and an override claims v1 → the component reads v1 and parses v2 data, silently;
- an override claims a version this build does not know → the *whole component* vanishes behind an `unknown_schema_version`.

So it is checked.
**Every layer that supplies a real property to a component and also stamps a version must agree with the stamp that won**, or the component is dropped with a `layered_schema_version_conflict`.

A layer that does not stamp has **no opinion** rather than "version 0".
That is exactly the shape an override layer should have, and it is why overriding one property is still allowed.

## What layering is not

**It is not persistence.**
Layering is a runtime composition, and each layer persists independently as an ordinary single-graph `.vdoc`.
The [file format](../../../versioned-document-file/docs/format.md) does not change at all.

**It is not reordering.**
Layers are pushed bottom-first and stay put.
Muting is supported and forces a recompose; arbitrary reordering is not implemented, and would too.

**It is not a merge.**
Nothing here helps two people edit one document — that is what the DAG and [multi-values](multi-values.md) are for.

The settled choices, and what would reopen each, are in [decisions.md](../decisions.md#layering-composes-per-property-path-and-a-higher-layer-replaces-rather-than-merges).
