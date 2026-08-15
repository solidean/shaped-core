# versioned-document — Cheat Sheet

Structured documents that are versioned, mergeable and verifiable.
Namespace `vdoc`. Depends on clean-core only.

> **The storage layer is real; the typed layer above it is still `[planned]`.**
> Values, ids, ops, the DAG and the raw document are built; components, parsing and `document` are not.
> A `[planned]` section is the intended API surface, kept next to the design so the two are written together; the design itself is [docs/concept.md](docs/concept.md).
> Entries lose their `[planned]` marking as the [milestones](docs/todo/_index.md) land, and this banner goes when the last one does.

## The flow

```text
typed component edit                    op_builder{}.set(entity, comp)   [planned]
  -> diff vs parents, build an op   ->  .build(graph)              => op
  -> add to the DAG by content hash ->  graph.add(op)              => op_id
  -> materialize a head             ->  graph.materialize(head)    => raw_document
  -> interpret into a typed document -> parse(raw, policy, report) => document  [planned]
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

- **`add` is not append.** It inserts by content hash and moves no head, so two identical ops collapse to one entry.
- **`build` diffs.** Re-setting an unchanged property emits no assignments at all.
- **A multi-valued path always emits**, even when every surviving writer holds identical bytes — that op is how a conflict is resolved.
- Staging one path twice asserts: two code paths writing one property is a bug, not an update.
- `.set(entity, my_transform{...})` — generic over `component_traits` — is `[planned]` for milestone 3.

## Reading

```cpp
auto const raw = graph.materialize(head);            // or materialize(span<op_id>) for a merge
graph.materialize_entities(heads, entities);         // the cheap path op_builder diffs against
graph.collect_reachable(heads);                      // the local closure; missing ops are skipped
graph.children(id);                                  // inverted parent edges; may name ops not present
```

**A raw document borrows the graph's op bytes**, so it is valid only while those ops are still in the graph.

### The typed layer — `[planned]`

```cpp
vdoc::component_registry registry;
registry.register_component<my_transform>();

vdoc::default_parse_policy policy(registry);         // or (registry, graph, local_head) for merges
vdoc::parse_report report;
auto const doc = vdoc::parse(raw, policy, report);   // never fails; issues land in `report`
```

The typed document is **immutable** — there is no `set`.
Edits build an op and re-materialize, which is what makes a `document` safe to hold across threads indefinitely.

```cpp
doc.get<my_transform>(entity);                       // pointer, null if absent
doc.each<my_transform>([](auto id, auto const& t){ });
doc.each<my_transform, my_mesh>([](auto id, auto const& t, auto const& m){ });  // sorted-merge join
```

## Components — `[planned]`

The library ships **zero** components, so an application declares its own:

```cpp
template <>
struct vdoc::component_traits<my_transform>
{
    static constexpr cc::string_view type_name = "Transform";
    static constexpr i32 schema_version = 1;

    static void write(my_transform const&, vdoc::component_writer&);
    static cc::optional<my_transform> parse(vdoc::raw_component const&, vdoc::entity_id,
                                            vdoc::parse_policy const&, vdoc::parse_report&);
};
```

`parse` returning empty means "drop this component"; it has no failure mode.

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

- **`op_id` orders by its canonical 32 bytes**, never by `cc::hash256`'s defaulted `<=>`, which orders limbs and is a different order entirely.
- `[planned]` **Deletion is interpretation, not storage** — nothing is ever removed.
  `$alive` false drops a component, or on `$entity` the whole entity.
  Dead only if *unambiguously* false; a contested `$alive` stays alive plus a diagnostic.
- `[planned]` **Parsing never refuses.** Unknown components, unknown schema versions and unresolved conflicts become diagnostics while the rest of the document loads.
- **Op ids are canonical** — identical content gives an identical id, whatever order the caller supplied.
  Never hash persistent data by a raw interned id.
- **Verification never re-serializes.** It re-hashes the bytes as stored, so no formatting change can look like tampering.
  The op holds those bytes and decodes on demand, so there is no encoder near a loaded op to change.
- **A pruned parent is a skeleton op** — id and parents, no payload — and is unverifiable by construction, never a mismatch.
- **Op ids do not commit to asset content**, so a document is reproducible only relative to an asset resolution.
  See [decisions.md](docs/decisions.md#the-asset-mapping-is-mutable-and-remapping-is-retroactive).
