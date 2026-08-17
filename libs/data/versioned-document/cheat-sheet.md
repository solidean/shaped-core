# versioned-document — Cheat Sheet

Structured documents that are versioned, mergeable and verifiable.
Namespace `vdoc`. Depends on clean-core only.

## The flow

```text
typed component edit                    op_builder{}.set(entity, comp)
  -> diff vs parents, build an op   ->  .build(graph)              => op
  -> add to the DAG by content hash ->  graph.add(op)              => op_id
  -> materialize a head             ->  graph.materialize(head)    => raw_document
  -> interpret into a typed document -> parse(raw, policy, report) => document
```

Editing needs only an `op_graph`.
No "current head" is stored anywhere — parents are passed explicitly, which is what makes the whole edit path testable in isolation.

## Values

A canonically-encoded byte sequence: a tag byte plus its payload.
Equality and hashing are over the bytes, and nothing else.

> **Not frozen.** A general-purpose any-value format landing elsewhere could replace this one, which would break the `.vdoc` format rather than refactor it.
> A migration may or may not be provided — shaped-core is still in "can break" mode.
> See [decisions.md](docs/decisions.md#the-codec-starts-in-vdoc-not-in-clean-core).

```cpp
#include <versioned-document/value.hh>
#include <versioned-document/value_builder.hh>
#include <versioned-document/value_debug.hh>

auto const v = vdoc::value::of(42);              // integer; also bool / any integer / f32 / f64 / string
auto const b = vdoc::value::of_bytes(span);      // the byte-string kind; of_null() for null
auto const p = vdoc::value_builder::array()      // [1.0, 2.0, 3.0]
                   .push(1.0).push(2.0).push(3.0)
                   .build();
auto const o = vdoc::value_builder::object()     // nesting is by COMPOSITION: build a sub-value, then set it
                   .set("name", "wall").set("p", p)
                   .build();                     // sorts keys; try_build() -> result<value, value_build_error>

v.kind();                                        // vdoc::value_kind::integer
v.as_i64();                                      // 42 — as_bool / as_f64 / as_string / as_bytes; wrong kind ASSERTS
v.bytes();                                       // cc::span<byte const> — the canonical encoding
a == b;  hash(a);                                // byte equality and a byte hash; there is no structural compare

o.size();  o.key_at(i);  o.element_at(i);        // element_at WALKS: O(i), not O(1)
o.try_find("name");                              // cc::optional<value_view>; linear scan, early-exits on sorted keys

vdoc::try_decode(bytes);                         // result<value_view, value_decode_error> — the ONLY route from bytes
vdoc::skip_value(bytes);  vdoc::encoded_size(bytes);  // walk a buffer of adjacent values; input must be validated
vdoc::to_debug_string(v);                        // one-way JSON-ish text; NOTHING may parse it back
```

| kind | payload |
|------|---------|
| `null` | empty |
| `boolean` | one byte, exactly 0 or 1 |
| `integer` | 8 bytes LE |
| `number` | 8 bytes, the binary64 bit pattern |
| `string` | u32 length + UTF-8 |
| `bytes` | u32 length + bytes |
| `array` | u32 payload length + u32 count + elements |
| `object` | u32 payload length + u32 count + entries, keys sorted |

A length prefix always counts **the bytes that follow it**, so skipping any length-prefixed kind is `5 + prefix`.

Gotchas:

- **`try_decode` is the only route from bytes.** Everything else assumes bytes that already passed it, and asserts rather than validates.
- **Decoding rejects non-canonical input**, because tolerating it would break byte equality.
  Structural errors: a `boolean` payload of 2, a length prefix that disagrees with its contents, trailing bytes, nesting past `value_view::max_depth` (64).
  Ordering errors: object keys that are unsorted or duplicated.
- **UTF-8 is not validated.** The canonicality rules are structural; `string` and `bytes` differ in intent, not in what the decoder checks.
- **Float canonicalization is yours.** `NaN` payloads and `-0.0` are stored as written, so two `NaN`s with different payloads are different values.
- **A value under 36 bytes does not allocate**, which is nearly all of them; past that it is heap-backed and merely slower.
- **Bulk data is not a value.** A mesh or a texture is a blob, referenced by an asset id string.

## Identity

```cpp
auto const e = vdoc::entity_id::of("wall-17");    // interned, process-wide
e.as_string_view();                               // the canonical bytes back
e.compare_bytes(other);                           // -> strong_ordering; the ONLY order that reaches storage
vdoc::entity_id::by_bytes{};                      // the matching sort predicate

vdoc::property_path{.entity = e, .component = c, .property = p};   // the addressable unit
path.compare_bytes(other);                        // entity, then component, then property — a FORMAT CONSTANT
```

`entity_id`, `component_type_id` and `property_id` are distinct types over an interned string, so they cannot be mixed up.
**Stay in id space on hot paths**; interned ids are process-local, so never serialize one or hash persistent data by it.
**There is no `operator<`** — the id order feeds the op hash, so the call site names `compare_bytes` and nothing else.

## Editing

```cpp
vdoc::op_graph graph;

auto const op = vdoc::op_builder{}
                    .set_parents(head_span)               // empty = a new document; sorted+deduped at build
                    .set_metadata(author_and_time)        // free-form, informational, still hashed
                    .set_raw(path, vdoc::value::of(3))    // or set_raw(entity, component, property, value)
                    .build(graph);                        // diffs vs parents; only changed properties

auto const head2 = graph.add(cc::move(op));               // keyed by content hash; returns it
```

`build(graph, cache)` is the same op, with the diff's walk allowed to terminate at a cached snapshot.
The filter applies to assignments and never to edges, so a snapshot is the only thing that shortens a build — **use this overload in any edit loop**.

```cpp
staged.abstain(path);                                     // or abstain(entity, component, property)
```

**`abstain` is the only way to un-write a property** — `$alive` removes a whole component or entity, nothing else removes one property.
It withdraws this history's contribution and lets whatever is below show through, so it is what reverts an override or a stored value to absent.
Its diff is the **mirror** of `set_raw`'s: abstaining over a path nobody wrote emits nothing, so re-staging the same withdrawal every frame gives the same op id and costs nothing.

- **`add` is not append.** It inserts by content hash and moves no head, so two identical ops collapse to one entry.
- **`build` diffs.** Re-setting an unchanged property emits no assignments at all.
- **A multi-valued path always emits**, even when every surviving writer holds identical bytes — that op is how a conflict is resolved.
- Staging one path twice asserts: two code paths writing one property is a bug, not an update.
- `.set(entity, my_transform{...})` writes a whole component, stamping `$schema_version` for you.
- `.remove_component(entity, component)` / `.remove_entity(entity)` spell deletion, which is an ordinary write and never a removal.
  `.restore_component` / `.restore_entity` undo one; all four are shorthands for `.set_alive` / `.set_entity_alive`.
- **Restoring only means anything against a stored `false`** — `$alive` absent already means alive, so restoring something nobody removed writes a property that says exactly the default.

## Reading

```cpp
auto const raw = graph.materialize(head);            // or materialize(span<op_id>) for a merge
graph.materialize_entities(heads, entities);         // the cheap path op_builder diffs against
graph.collect_reachable(heads);                      // the local closure; missing ops are skipped
graph.children(id);                                  // inverted parent edges; may name ops not present
graph.skeletonize(id);                               // drop an op's payload, keep its id and parents
graph.fill_payload(id, cc::move(payload));           // the inverse; `add` leaves a skeleton a skeleton
graph.drop_leaf(id);                                 // forget it entirely; ASSERTS if anything descends from it
graph.leaves();                                      // every op nothing descends from, sorted by id bytes
```

**`skeletonize` and `drop_leaf` are for opposite situations.**
A skeleton keeps ancestry alive through a pruned op; `drop_leaf` is for a discarded editing frame with no ancestry to preserve.

**A raw document borrows the bytes it was materialized from**, and owns none of them.
A value points into the writing op's payload, or into the arena of the snapshot that terminated the sweep.
So it is valid only while both the graph and the cache that served it are alive and unmodified.

### Snapshots

```cpp
vdoc::snapshot_cache cache;                          // caller-owned; op_graph holds none
graph.materialize(head, cache);                      // walks back only as far as a usable snapshot
graph.materialize_entities(heads, entities, cache);  // a snapshot serves a filtered sweep by projection

vdoc::install_snapshot(graph, head, cache);          // explicit; nothing installs behind your back
vdoc::install_snapshot_if_useful(graph, head, cache, {.min_ops_behind = 4096});

vdoc::advance_snapshot(graph, cache, parent, child); // moves the entry onto a SINGLE-PARENT child, ~3 us

cache.clear_unpinned();                              // invisible: costs speed, never a result
cache.erase(id); cache.is_pinned(id); cache.size();
cache.unpin(id);                                     // a snapshot that stopped being load-bearing, bytes kept
cache.take(id);                                      // removes and hands it over; advance_snapshot's primitive
```

A snapshot is **surviving writers only** — exactly a `raw_document` over bytes it owns, so it outlives the ops that wrote them.

**The editing loop is: pin one snapshot after the load, then `advance_snapshot` on every accepted op.**
That keeps the head permanently one op from a snapshot, so a build and a materialization never get slower with the session.
Do **not** advance during a drag: the frames are siblings, and moving the snapshot onto one leaves the rest replaying everything.

### Recovery from an untrusted peer

```cpp
vdoc::received_op const batch[] = {{.id = id, .parents = parents,       // spans into the RECEIVE BUFFER
                                     .metadata_bytes = m, .assignment_bytes = a}};

auto const done = vdoc::integrate(graph, batch);     // verify + apply, or change nothing
done.error().op;                                     // the op it was refused at
done.error().reason;                                 // malformed | parents_disagree
done.error().decode_error;                           // hash_mismatch here means corruption or tampering

vdoc::try_verify_batch(graph, batch);                // -> cc::vector<op>, storing nothing
vdoc::apply_verified_batch(graph, cc::move(ops));    // infallible; the split a store needs to check more first
```

**No trust in the sender, at any point** — an op id commits to everything behind it, so recomputing the hashes is the whole check.
**The batch is a set**: one bad op refuses all of it, and the graph is left exactly as it was.
A skeleton in the graph is **filled in**; a skeleton *offered by a peer* is refused, since bytes that are not there cannot be verified.

### The typed layer

```cpp
auto const policy = vdoc::default_parse_policy::create_with_registry(registry);
// ...or create_with_local_head(registry, graph, local_head) for merges, which enables the local-closure branch
vdoc::parse_report report;
auto const doc = vdoc::parse(raw, policy, report);   // never fails; issues land in `report`
```

The typed document is **immutable** — there is no `set`, and there never will be.
Edits build an op and re-materialize, which is what makes a `document` safe to hold across threads indefinitely.

```cpp
auto b = vdoc::document_builder(cc::move(doc));      // CONSUMES it; a document you hold cannot change
b.insert_entity(e); b.remove_entity(e);              // the entity table; removing takes its components too
b.set_component(schema, e, construct);               // => change_kind; construct==false means "drop it"
b.remove_component(type, e);
b.dead_arena_bytes(); b.live_arena_bytes();
b.compact();                                         // reclaims what in-place edits stranded; caller's call
doc = cc::move(b).freeze();                          // immutable again
```

`document_builder` is a **storage** edit: no `$alive`, no policy, no diagnostics.
`vdoc::apply` is what does interpretation and drives this underneath — reach for that, not this, unless you are building a document from nothing.

### Evolving a document as ops arrive

```cpp
vdoc::change_summary changes;
vdoc::incremental_apply_stats stats;

doc = vdoc::apply(cc::move(doc), graph, from, to,       // CONSUMES doc; `from` is where doc currently is
                  policy, report, changes,
                  {.cache = &cache}, &stats);           // .max_chain_ops, .compaction_ratio, .force_full_reparse

stats.took_fast_path;                                   // false = it re-parsed, and `changes` then says "everything"
stats.fallback_reason;                                  // why: forced | no_single_parent_chain | chain_too_long
changes.entities; changes.components;                   // sorted; added / removed / modified
```

**Only `chain_too_long` is fixed by raising `max_chain_ops`** — `no_single_parent_chain` is a statement about the history.

### The dirty set an apply consumes

```cpp
auto b = vdoc::change_set_builder();               // or (change_granularity::entity) for a coarse producer
b.add(path);  b.add_entity(entity);                // any order, duplicates free; add_entity COARSENS the whole set
b.add_everything();                                // no honest delta available
auto dirty = cc::move(b).build();                  // sorts + dedups under the granularity

dirty.covers(path);  dirty.covers_entity(e);       // the only supported way to ask
dirty.entities();                                  // sorted, unique; what a touched set wants
dirty.coarsen_to(vdoc::change_granularity::entity); // O(n); ASSERTS on a request to refine
dirty.union_with(other);                           // takes the coarser granularity; `everything` absorbs
```

**`covers` over-reports at worst and never under-reports.**
That is what makes granularity a speed dial: every consumer is correct at every granularity, and only the recomputation varies.
Below the granularity an entry's fields are default ids that **must not be read** — an id has no invalid state, so the granularity and never the data says how much of a path means anything.

**The fast path needs a single-parent chain from `to` back to `from`, within `max_chain_ops` (64).**
A merge, a longer chain, or a `to` that does not descend from `from` re-materializes and re-parses — correct, and slow.

The report is **edited, not appended to**: findings for touched entities are dropped and recomputed.
`unsupported_component_type` is document-scoped and is never retracted — see [interpretation](docs/concepts/interpretation.md#what-an-incremental-apply-owes-the-report).

The whole realtime loop, per op:

```cpp
auto op = staged.build(graph, cache);                   // diff terminates at the snapshot
auto const previous = head;
head = graph.add(cc::move(op));
vdoc::advance_snapshot(graph, cache, previous, head);   // only for an op ACCEPTED as history
doc = vdoc::apply(cc::move(doc), graph, previous, head, policy, report, changes, {.cache = &cache});
```

**Chain a drag's frames, do not fan them.**
Sibling frames force a full re-parse each; single-parent frames stay on the fast path, and `drop_leaf` discards them on release.
See [workloads](docs/concepts/workloads.md#at-the-typed-layer-chain-the-frames-instead-of-fanning-them).

### Layering

```cpp
#include <versioned-document/layer_stack.hh>

auto base = vdoc::direct_layer("base");            // no op graph; owns its bytes
base.set(path, vdoc::value::of(1.0));              // DIFFED: identical bytes bump nothing
base.begin_rebuild(); /* rewrite everything */ base.finish_rebuild();  // unwritten paths are dropped
base.abstain(path);  base.mark_dirty(path);  base.clear();

vdoc::layer_stack stack;
auto const b = stack.push_direct_layer("base", base);        // bottom-first
auto const u = stack.push_graph_layer("user", graph, head, &cache);
stack.set_head(u, new_head);                       // the ONLY way a graph layer moves
stack.set_muted(u, true);                          // forces a full recompose

stack.rebuild(policy, report, changes);            // always correct, O(document)
stack.apply(policy, report, changes, {}, &stats);  // O(dirty entities x layers)
stack.composed();                                  // an ORDINARY vdoc::document
stack.provenance_of(path);                         // -> layer_handle; a UI query, not a loop
```

**A higher layer replaces a lower one per property path**, and its whole writer list replaces the lower's.
So overriding `transform/position` leaves `transform/rotation` coming from below — component granularity would freeze it.
Conflicts stay layer-local: a contested path inside the winning layer reaches the policy exactly as it would unlayered.

- **The stack pulls every delta.** It owns each graph layer's head and each direct layer bumps its own version, so a forgotten change set is not expressible.
- **`apply` is O(dirty entities × layers)** — measured flat at 0.01–0.03 ms from 500 to 8,000 entities.
  **A wholesale `begin_rebuild` is O(n) at ~100 ns per property**: 0.3 ms at 500 entities, 1.3 ms at 2,000, 6 ms at 8,000.
  Comfortable to a couple of thousand; past that write only what moved.
- **Nothing downstream of `composed()` knows about layering** — dense columns, `each<A,B>`, immutability, all unchanged.
- **`$alive` composes per path and that is intended**: a higher layer revives or kills what the base did.
  Withdraw a deletion by *abstaining* `$alive`, not by writing `true`.
- **`$schema_version` composes per path and that is a hazard.**
  Every layer supplying a real property *and* stamping must agree with the winning stamp, or the component is dropped with `layered_schema_version_conflict`.
  A layer that does not stamp has no opinion — which is what an override layer should be.
- **Never install a composed document into a `snapshot_cache`.** It is several histories at once, and a direct layer's writer ids name no op.
- Layering is **runtime composition, not persistence** — each layer saves as an ordinary single-graph `.vdoc`.

```cpp
doc.get<my_transform>(entity);                       // pointer, null if absent; binary search
doc.has<my_transform>(entity);
doc.each<my_transform>([](auto id, auto const& t){ });                         // linear scan
doc.each<my_transform, my_mesh>([](auto id, auto const& t, auto const& m){ }); // leapfrog sorted-merge join

doc.entities();                                      // span<entity_id const>, sorted by id bytes
doc.component_types();  doc.count_of(type);
report.diagnostics;  report.agreed_multi_values;     // string-free: a kind plus the ids it concerns
report.count_of(vdoc::diagnostic_kind::remote_conflict);
```

Per component type the document holds two parallel dense arrays sorted by entity id bytes — the ids, and the components.
That is what makes `get` a binary search and a multi-type join a merge with no indirection.

An entity that survived with **no known component is still in `entities()`**: it exists, and it is alive.
There is deliberately no `operator==` on a document, because it would need per-type equality the library cannot require.

## Components

The library ships **zero** components, so an application declares its own:

```cpp
template <>
struct vdoc::component_traits<my_transform>
{
    static constexpr cc::string_view type_name = "Transform";
    static constexpr i32 schema_version = 1;

    static void write(my_transform const&, vdoc::component_writer&);   // w.set("x", value::of(...))
    static cc::optional<my_transform> parse(vdoc::property_reader const&);
};

vdoc::component_registry registry;
registry.register_component<my_transform>();      // idempotent; two C++ types under one name ASSERT
registry.merge(other);                            // the union
```

`parse` returning empty means "drop this component"; it has no failure mode.
**`write` does not stamp `$schema_version`** — `op_builder::set` does, once, and `component_writer::set` asserts on any `$`-prefixed name.

### Reading properties — the one multi-value rule

```cpp
r.try_get("x");                            // -> cc::optional<value_view>; empty if unwritten or dropped
r.schema_version();                        // what the writer stamped, 0 if nothing did; migrate forward from it
r.entity();  r.component();                // where you are
r.raw();                                   // the escape hatch — and then the rules are yours
```

`try_get` is **the only supported way to read a property**, because it is the one place the rules live:

- one writer wins outright;
- several that agree byte-wise collapse into `report.agreed_multi_values`, with no diagnostic;
- several that disagree go to `parse_policy::resolve_multi_value`.

**Every `value_view` borrows the op's bytes**, so a component that keeps bytes must copy them — the raw document is gone as soon as the parse returns.

**Reserved names, `$`-prefixed and owned by the library** — applications must not use the sigil:

| name | meaning |
|------|---------|
| `$schema_version` | the version its writer stamped |
| `$alive` | deletion; absent means alive |
| `$entity` | a component type carrying entity-level `$alive`; no C++ struct |

## Raw document

```cpp
doc.try_get(entity);                       // -> raw_entity const*, binary search by id bytes
doc.try_get(path);                         // -> raw_property const*, null if nothing ever wrote it
doc.property_count();                      // paths carrying at least one write

prop->writers;                             // [ { op_id writer; value_view value; } ], sorted by writer bytes
prop->is_multi_valued();                   // more than one surviving writer
prop->single();                            // the value; ASSERTS when multi-valued
```

Every level is a vector sorted by canonical id bytes, never a hash container, so iteration order is the same on every machine.

A property normally has one value.
Concurrent writers where neither dominates leave several, and **that includes writers who wrote identical bytes** — collapsed silently at parse time into `report.agreed_multi_values`.

## Patterns & gotchas

- **A snapshot stores surviving writers only**, and whether one may be USED is decided per sweep against today's DAG — never recorded when it was taken.
- **Installing a *filtered* result poisons the cache.**
  A filtered materialization is a projection rather than surviving(head), so every later sweep terminating there is silently truncated.
- **Dropping the cache is invisible by construction.** It changes how long a materialization takes and nothing else.
- **Adding ops never invalidates a snapshot.** An op id commits to everything behind it, so surviving(id) cannot change once computed.

- **`op_id` orders by its canonical 32 bytes**, never by `cc::hash256`'s defaulted `<=>`, which orders limbs and is a different order entirely.
- **Deletion is interpretation, not storage** — nothing is ever removed.
  `$alive` false drops a component, or on `$entity` the whole entity.
  Dead only if *unambiguously* false; a contested `$alive` stays alive plus a diagnostic.
- **Parsing never refuses.** Unknown components, unknown schema versions and unresolved conflicts become diagnostics while the rest of the document loads.
- **An absent `$schema_version` is version 0, not "unknown"**, so a document written entirely through `set_raw` still parses.
  Only a *newer* or a *contested* version skips the component.
- **`$alive` and `$schema_version` never reach the policy.** Both are read straight off the raw writers, so no policy can vote a thing out of existence or onto a version nobody wrote.
- **`unsupported_component_type` is filed once per type**, not once per entity — a large document reports the missing types, not their every occurrence.
- **Op ids are canonical** — identical content gives an identical id, whatever order the caller supplied.
  Never hash persistent data by a raw interned id.
- **Verification never re-serializes.** It re-hashes the bytes as stored, so no formatting change can look like tampering.
  The op holds those bytes and decodes on demand, so there is no encoder near a loaded op to change.
- **An abstention never reaches the raw document.** It supersedes inside the sweep and is then dropped, so a withdrawn path is absent rather than special.
  The consequence: a **concurrent write beats a concurrent abstention, undiagnosably** — the non-vanishing side wins, as with `$alive`.
  It cannot arise on a linear history, which is where withdrawals are used.
- **Every assignment record carries a kind byte**, so adding abstain moved every op id — free while nothing has written a `.vdoc` outside the tests.
- **A pruned parent is a skeleton op** — id and parents, no payload — and is unverifiable by construction, never a mismatch.
- **Op ids do not commit to asset content**, so a document is reproducible only relative to an asset resolution.
  See [decisions.md](docs/decisions.md#the-asset-mapping-is-mutable-and-remapping-is-retroactive).
