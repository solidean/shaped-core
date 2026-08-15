# Milestone 3 — Raw and typed documents

**Goal.** Interpretation: the component protocol, the parse policy and report, the reserved conventions, and the immutable typed index an application actually queries.

**Why here.** With milestone 2 the storage model is complete and inert.
This is what makes it usable — and it is where every "it depends on the application" decision is concentrated, on purpose, so that storage stays free of them.

The design is [concept.md](../concept.md#interpretation) and [The typed document is an immutable index](../concept.md#the-typed-document-is-an-immutable-index).
Depends on milestone 2.

## What milestone 2 left you

The storage layer is complete and tested; this milestone only ever reads from it.
Four things about its shape matter here, because each one changes what the typed layer may assume:

- **`raw_document` borrows the graph's op bytes.**
  Every `value_view` in it points into the payload of the op that wrote it, so a parse must not outlive the `op_graph` — or must copy what it keeps.
  `document` owning an arena of its own is what resolves that, and it is already the design.
- **Every level of the raw document is a vector sorted by canonical id bytes**, with `try_get` doing a binary search.
  It is not a hash container, so the parser may iterate it directly and stay reproducible.
- **`raw_property::single()` asserts when the property is multi-valued.**
  Parsing must ask `is_multi_valued()` first, which is the point where the agreed-multi-value collapse and the conflict diagnostics belong.
- **`op_builder::set(entity, component)` does not exist yet** — it is generic over `component_traits`, so it lands here, on top of the existing `set_raw`.
  The diff, the canonicalization and the hashing beneath it are done and must not be reimplemented.

Everything reserved (`$schema_version`, `$alive`, `$entity`) is still purely a convention: storage assigns no meaning to a `$` prefix, and this milestone is where that meaning first appears.

---

## Work items

### 1. The component protocol

An application declares a component by specializing `component_traits<C>`:

```text
type_name       cc::string_view, stable, e.g. "Transform"
schema_version  i32, bumped when the stored shape changes
write(C, writer)              state -> properties, stamping $schema_version
parse(raw, entity, policy, report) -> cc::optional<C>
```

`parse` returning empty means **drop this component**.
It has no failure mode at all: anything questionable becomes a report entry, never an error return, because a component that will not parse must not be able to fail a document load.

An `is_component` concept over the traits keeps `op_builder::set` and the document's accessors generic without any of them knowing a concrete type.

**The library ships zero components.**
Not one, not even a convenience `name`. The moment there is a built-in component, the library has an opinion about what a document is for.

### 2. `component_registry`

The runtime, type-erased set of component types an application understands.

- `register_component<C>()`, lookup by `component_type_id`, `merge`.
- Extensible at any time, and a test may register a subset — the parser is driven entirely by what it is handed.
- `component_schema` is what the registry stores per type: the name, the current version, and the type-erased write/parse entry points.

### 3. `parse_policy` and `default_parse_policy`

The policy is the whole "how to interpret" surface, so a caller can vary interpretation without the parser knowing:

- `query_component_schema(type)` — null means this policy does not understand the type.
- `should_instantiate_entity(entity, raw, report)` — deletion, and anything else that suppresses an entity.
- `resolve_multi_value(entity, component, property, candidates, report)` — pick one, or drop it.

`default_parse_policy` bakes the conventions:

- registry lookup;
- `$alive`-based deletion;
- **conflict resolution**: if exactly one surviving writer is inside the local closure, that value wins with a *remote conflict* diagnostic.
  Otherwise the smallest op id wins, with a *multi-valued conflict* diagnostic.
  Both branches are total and reproducible — the same inputs give the same document on every machine.

Constructed either with a registry alone, or with a registry plus a graph and a local head, which is what makes the local-closure branch available.

### 4. Reserved names

`$`-prefixed, owned by the library, and applications must not use the sigil.

| name | where | meaning |
|------|-------|---------|
| `$schema_version` | any component | the version its writer stamped |
| `$alive` | any component | deletion; absent means alive |
| `$entity` | a component type | entity-level `$alive`; no C++ struct, and applications never see it |

Deletion rules, exactly:

- something is dead only if `$alive` is **unambiguously** false — every surviving writer says false;
- a contested `$alive` keeps it alive **and** files a diagnostic, because resurrecting is recoverable and vanishing is not;
- `$alive` on `$entity` drops the whole entity; on any other component, just that component;
- parsing never mutates the DAG, and deletion changes what is visible and nothing else.

### 5. Reading a property, once, correctly

A single helper every `parse` implementation uses, so the multi-value rules are implemented exactly once:

- one value → use it;
- several that agree byte-wise → use it, and record an **agreed multi-value** in the report.
  No diagnostic: this is not a problem, it is a tidy-up hint for a later write.
- several that disagree → hand to the policy.

If this logic is ever duplicated into a component's own `parse`, the rules will drift, so make the helper the obvious path.

### 6. Schema versioning

`write` stamps `$schema_version`; `parse` resolves it early and migrates forward from whatever it finds.
An unknown or contested version **skips that component with a diagnostic**, leaving the stored data untouched for a build that does understand it.

Old versions load and re-stamp on the next write, and stored history is never migrated in place.

### 7. Diagnostics

String-free: a kind plus the optional ids it concerns.
The path, the kind and the raw document together recover the specifics, and localize later without anything being re-parsed.

Kinds at minimum: `unsupported_component_type`, `unknown_schema_version`, `multi_valued_conflict`, `remote_conflict`, `contested_alive`.

`parse_report` carries the diagnostics plus the agreed multi-values, and the two are separate lists because they are different things: one is a problem, the other is housekeeping.

### 8. The typed document

Built once by `parse`, and **no mutation API** — no `set`, no `remove`, no `create`.
This is not a limitation to lift later: edits go through an op and re-materialize, and immutability is what makes a document safe to hand to another thread and hold indefinitely.

Layout:

- one arena for the whole document, so building is cheap and destruction is one release;
- an entity table sorted by entity id;
- per component type, two parallel dense arrays sorted by entity id — the ids, and the components.

Queries:

- `get<C>(entity)` — binary search, null if absent;
- `each<C>(f)` — a linear scan of contiguous memory;
- `each<A, B, ...>(f)` — a sorted-merge intersection, no sparse sets and no indirection;
- `has<C>(entity)`, and access to the entity list.

Keep the layout compatible with **structural sharing** — reusing untouched component arrays when re-parsing after a small edit.
Nothing implements it in v1; nothing may make it impossible either.

## API surface this lands

```text
vdoc::component_traits / is_component / component_schema / component_registry
vdoc::parse_policy / default_parse_policy
vdoc::diagnostic_kind / diagnostic / agreed_multi_value / parse_report
vdoc::parse
vdoc::document
vdoc::reserved::*    the $-prefixed names, as constants
```

## Tests

- **Unknown component types** land as diagnostics while every known component still parses.
- **Deletion**: `$alive` false drops a component, on `$entity` it drops the entity, and a contested `$alive` keeps it alive with a diagnostic.
  Then undelete by a later write.
- **Agreed multi-values collapse silently** — no diagnostic, an entry in the agreed list.
  This is directly the equal-value case milestone 2 preserved, so the two milestones' tests meet here.
- **Disagreeing writers** resolve by the documented rule, and the same inputs give the same result every time.
  Test both branches: exactly one local writer, and none.
- **Schema versions**: an older version migrates and re-stamps on the next write; a newer one skips with a diagnostic and leaves the stored data alone.
- **A parse never mutates the graph** — hash the graph before and after.
- **Query correctness**: `get` against a linear scan; a two-type join against the intersection computed naively; empty and single-element edge cases.
- **Determinism**: parsing the same raw document twice gives identical documents, including diagnostic order.
- **A registry subset** parses a document written with a superset, and reports exactly the missing types.

## Acceptance

- The library contains no component type.
- Parsing has no failure mode: there is no input for which it refuses to produce a document.
- The multi-value reading rules exist in exactly one place.
- The typed document exposes no mutation.
- [structure.md](../structure.md)'s component / parse / document entries are `[done]`, and versioned-document's cheat-sheet has lost its `[planned]` banner.
