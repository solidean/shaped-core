# Customization points

How a clean-core operation (hashing and formatting today; more soon) lets a
type opt into custom behavior. The mechanism is centered on the `cc::custom::`
namespace and is broadly uniform across operations, so once you learn it for one,
you mostly know it for all — with small per-operation variations (see the note
under "The tiers"). (Hub: [_index.md](_index.md).)

## The tiers

An operation `op` resolves its implementation for a type `T` by checking, **in
order**:

1. **`cc::custom::op_trait<T>` specialization** — the *override* tier. A struct
   template in the `cc::custom::` namespace with a static `op` member. Checked
   **first, always.**
2. **ADL hidden friend** — `friend R op(T const&)` defined inside `T`. The
   *default* tier for types you own.
3. **Member function** — `t.op()`. An optional *third* tier some operations
   offer (e.g. formatting / `to_string`); hashing does not use it.

The first tier that matches wins. A type should provide **exactly one**
implementation.

> **Naming and tier set are conventions, not rules.** What actually binds a
> customization point is the **`cc::custom::` namespace**; the `<op>_trait` suffix
> and the full trait→friend→member tier set are conventions that fit ADL-dispatched
> operations like hashing. An operation may use a differently-named template and a
> different subset of tiers when that suits it better. For example, formatting's
> point is **`cc::custom::formatter<T>`** (a specialization with a static `format`
> plus a consteval `validate`) and it intentionally **omits the ADL tier**
> (`cc::custom::formatter<T>` → member `to_string()` only) to avoid triggering ADL
> over every argument on each `cc::format` call.

### Why this order

- **The `cc::custom::` trait is first by design.** It is the only tier that can
  override a type you don't own — a third-party type, a fundamental type, or one
  whose hidden friend is wrong for your use. Putting it first is what makes
  "override" actually mean override. The cost is that a stray specialization
  silently shadows a type's own friend/member, so the trait is for **uncommon,
  usually external/builtin** types — not the everyday path.
- **The hidden friend is the default** for types you own: it sits with the type,
  needs no separate include, and adds nothing to the normal `cc::` API surface
  (it's found only by ADL). This is the common case.
- **The member function**, where an operation supports it, is the most
  discoverable form for a type's own author, but it loses to the trait so that
  external overrides still win.

### Why `cc::custom::`

The traits live in `cc::custom::` rather than `cc::` so the customization surface
never clutters the normal API. You reach for `cc::custom::` only when writing an
override; everyday code calls the operation entry point (e.g. `cc::make_hash`)
and never sees the trait.

`cc::<op>` as a *free function* is intentionally left undefined for ADL-based
operations: the entry point calls the bare name (`op(v)`) so it resolves only to
hidden friends — a `cc::op` would shadow them.

## Worked example: hashing

Hashing uses tiers 1 and 2 (no member tier). Entry points live in
[common/hash.hh](../src/clean-core/common/hash.hh); `cc::make_hash` is a niebloid
so it can't be ADL-hijacked.

Override tier — a type you don't own, or a builtin:

```cpp
// in your header, namespace scope
template <>
struct cc::custom::hash_trait<some_external::widget>
{
    [[nodiscard]] static u64 hash(some_external::widget const& w) { return w.id; }
};
```

Default tier — a type you own:

```cpp
struct point
{
    int x, y;
    // composable (NOT finalized — see hash.hh); fold members through cc::make_hash
    [[nodiscard]] friend u64 hash(point const& p) { return cc::make_hash(p.x, p.y); }
};
```

clean-core ships `cc::custom::hash_trait` specializations for the fundamentals
(integers, enums, floats, pointers) so they work without any per-type friend.

## Adding a new customizable operation

Put the customization template in `cc::custom::` — that namespace is the binding
part. Naming it `cc::custom::<op>_trait<T>` with a static `<op>` member is the
convention for ADL-dispatched ops and is *not* strongly enforced (see the note
above); pick what reads best for the operation. Then have the entry point dispatch
trait → (friend) → (member) with `if constexpr (requires { ... })`, trait first
(dropping tiers that don't apply). Mirror the [hash.hh](../src/clean-core/common/hash.hh)
`impl::hash_one` dispatch. Document the type's expected contract (for hashing:
`make_hash` is composable and must not finalize).
