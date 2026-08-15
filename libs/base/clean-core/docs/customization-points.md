# Customization points

How a clean-core operation lets a type opt into custom behavior — hashing, formatting, enum traits and async failure channels today.
The mechanism centers on the `cc::custom::` namespace and is broadly uniform across operations, so learning it for one teaches you most of the rest.
The per-operation variations are small, and the note under "The tiers" has them.
(Hub: [_index.md](_index.md).)

## The tiers

An operation `op` resolves its implementation for a type `T` by checking, **in order**:

1. **`cc::custom::op_trait<T>` specialization** — the *override* tier.
   A struct template in the `cc::custom::` namespace with a static `op` member, checked **first, always**.
2. **ADL hidden friend** — `friend R op(T const&)` defined inside `T`.
   The *default* tier for types you own.
3. **Member function** — `t.op()`.
   An optional *third* tier some operations offer, such as formatting / `to_string`; hashing does not use it.

The first tier that matches wins.
A type should provide **exactly one** implementation.

> **Naming and tier set are conventions, not rules.**
> What actually binds a customization point is the **`cc::custom::` namespace**.
> The `<op>_trait` suffix and the full trait→friend→member tier set are conventions that fit ADL-dispatched operations like hashing.
> An operation may use a differently-named template and a different subset of tiers when that suits it better.
> Formatting is the example: its point is **`cc::custom::formatter<T>`**, a specialization with a static `format` plus a consteval `validate`.
> It intentionally **omits the ADL tier** — `cc::custom::formatter<T>` then member `to_string()` only — so that `cc::format` does not trigger ADL over every argument on every call.
> [formatting](formatting.md) owns that protocol, with the required signatures and a worked example.

### Why this order

- **The `cc::custom::` trait is first by design.**
  It is the only tier that can override a type you don't own — a third-party type, a fundamental type, or one whose hidden friend is wrong for your use.
  Putting it first is what makes "override" actually mean override.
  The cost is that a stray specialization silently shadows a type's own friend or member.
  So the trait is for **uncommon, usually external or builtin** types, not the everyday path.
- **The hidden friend is the default** for types you own.
  It sits with the type, needs no separate include, and adds nothing to the normal `cc::` API surface, since it is found only by ADL.
  This is the common case.
- **The member function**, where an operation supports it, is the most discoverable form for a type's own author.
  It still loses to the trait, so that external overrides win.

### Why `cc::custom::`

The traits live in `cc::custom::` rather than `cc::` so the customization surface never clutters the normal API.
You reach for `cc::custom::` only when writing an override.
Everyday code calls the operation entry point, such as `cc::make_hash`, and never sees the trait.

`cc::<op>` as a *free function* is intentionally left undefined for ADL-based operations.
The entry point calls the bare name `op(v)` so it resolves only to hidden friends, and a `cc::op` would shadow them.

## Worked example: hashing

Hashing uses tiers 1 and 2; there is no member tier.
Entry points live in [common/hash.hh](../src/clean-core/common/hash.hh), which owns the composable-versus-finalized contract.
`cc::make_hash` is a niebloid, so it cannot be ADL-hijacked.

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

clean-core ships `cc::custom::hash_trait` specializations for the fundamentals — integers, enums, floats and pointers — so they work without any per-type friend.

## Worked example: enum traits

`cc::custom::enum_traits<EnumT>` in [common/enum_traits.hh](../src/clean-core/common/enum_traits.hh) is what an enum tells clean-core about itself.
Today that is whether it is a flag enum, which integer its flags pack into, and how its values map onto bits.

That last one has to be declared because it cannot be detected: nothing about `e = 4` says whether it means bit 4 or bit 2.
So `cc::flag_encoding` has no default, and each opt-in macro names one outright — `CC_FLAG_ENUM_INDEXED` for a plain enum, `CC_FLAG_ENUM_BITMASK` for one whose values already are bit patterns.

It uses **tier 1 only**, and not by preference: an enum can carry neither a hidden friend nor a member, so tiers 2 and 3 do not exist for it.
Unlike hashing's trait, its primary IS defined — it answers `is_flag_enum = false` — so `cc::flag_enum<E>` is a plain read rather than a detection idiom.
That is also what keeps an enum which later declares something else about itself from becoming a flag enum by accident.

Tier 1 has a consequence here that it does not have elsewhere.
An explicit specialization must be written at a namespace enclosing `cc`, so it can never sit next to the enum.
`operator|` on that enum is reachable only through ADL, so it must sit *inside* the enum's namespace.
The two halves of the opt-in cannot share a scope, which is why the macros in [common/flags.hh](../src/clean-core/common/flags.hh) take the namespace as an argument and open it themselves:

```cpp
namespace app { enum class shape { visible, selected, locked }; }
CC_FLAG_ENUM_INDEXED(app, shape, u32);
```

One macro at one site, and the enum's own header never stays open around the operators.
That is the direction the codebase wants generally: forward-declare, then define as `struct cc::flags` rather than reopening a namespace.
Writing the specialization by hand, with no macro at all, is the supported path for an enum you cannot annotate at its definition.

## Worked example: an async failure channel

`cc::custom::async_error_from_exception_trait<E>` in [thread/async.hh](../src/clean-core/thread/async.hh) is how a `cc::async<T, E>` failure channel opts into exception containment.
It says the channel can represent an exception that escaped a compute frame.
Tier 1 only, and again not by preference: `E` is frequently an enum, so tiers 2 and 3 do not exist for it.

Its primary is deliberately **defined but empty**, so a channel that declares nothing is *detected* as absent rather than being a hard error.
That is the difference from enum traits, whose primary answers a default.
There is no sensible default failure value for an arbitrary `E`, so cc must tell "no mapping" apart from "a mapping", and it reports the missing one at runtime instead.
[async.md](systems/async.md#exceptions-escaping-a-frame) owns what happens either way.

## Adding a new customizable operation

Put the customization template in `cc::custom::` — that namespace is the binding part.
Naming it `cc::custom::<op>_trait<T>` with a static `<op>` member is the convention for ADL-dispatched ops, and is *not* strongly enforced (see the note above).
Pick what reads best for the operation.
Then have the entry point dispatch trait → (friend) → (member) with `if constexpr (requires { ... })`, trait first, dropping the tiers that do not apply.
Mirror the `impl::hash_one` dispatch in [hash.hh](../src/clean-core/common/hash.hh).
Document the type's expected contract — for hashing, that `make_hash` is composable and must not finalize.
