# Containers

The contracts every `container/` type shares, and the table that says which one to reach for.

Per-type documentation lives in the headers; the [cheat-sheet](../cheat-sheet.md) lists the members.
This page owns the four things that would otherwise be restated in each of a dozen headers: how to choose, what `T` must be, what indexing checks, and when references die.

Storage itself is a level below — [systems/allocation](systems/allocation.md) owns `cc::allocation<T>`, the handle every heap container holds.
`cc::string` and `cc::string_view` live in `string/` rather than here, and [strings](strings.md) owns their contracts.

## Choosing one

Owning, heap-allocated — deep-copy value semantics, storage from a `cc::memory_resource`:

| type | size | grows | reach for it when |
|---|---|---|---|
| `array<T>` | runtime, fixed at creation | no | the count is known when you build it and never changes |
| `vector<T>` | runtime | at the back | the default growable sequence |
| `unique_array<T>` / `unique_vector<T>` | as above | as above | copying would be a bug — these are move-only |

Owning, inline — the elements live in the container object, so a local one never touches the allocator:

| type | capacity | spills to heap | reach for it when |
|---|---|---|---|
| `fixed_array<T, N>` | exactly `N`, compile-time | never | a plain aggregate; `T[N]` with bounds checks and the tuple protocol |
| `fixed_vector<T, N>` | at most `N`, size varies at runtime | never — pushing past `N` asserts | `N` is a genuine invariant, such as a hardware or protocol limit |
| `small_vector<T, N>` | at least `N` inline, then unbounded | yes, transparently | usually a handful, but an occasional overflow must still work |

`fixed_vector` and `small_vector` differ in what an overflow *means*.
`fixed_vector` encodes the cap in the type, so going past it is a bug that trips; `small_vector` treats `N` as a hint and allocates instead.
`N` is also a *minimum* for `small_vector` — the inline buffer grows to fill the struct footprint, so `inline_capacity()` is `>= N`.

Queues — a wrap-around slot array, so push and pop are O(1) at **both** ends and nothing ever shifts:

| type | capacity | grows | reach for it when |
|---|---|---|---|
| `ringbuffer<T>` | runtime, always a power of two | at both ends | a FIFO, a work queue, a double-ended queue on the heap |
| `fixed_ringbuffer<T, N>` | exactly `N`, compile-time | never | a fixed history window, or a queue that must not allocate |

The power-of-two capacity is what makes the heap ring's wrap a mask rather than a modulo, so a requested capacity is rounded up.
`fixed_ringbuffer` takes any `N`, since a compile-time capacity wraps with one compare-and-subtract instead.

Both are **not contiguous** once the content wraps, which is what separates them from every other owning container here.
There is no `data()` and no pointer iterator: `segments()` hands out the up to two contiguous runs, and `linearize()` rearranges the content into one.

`push_back_overwriting` / `push_front_overwriting` drop the element at the opposite end instead of growing, which is how a ring spells "keep the newest N".
There is deliberately no `cc::deque`: the storage strategy is load-bearing enough that it splits into separate types, `ringbuffer` here and `devector` for a contiguous double-ended store.

Non-owning views — the viewed storage must outlive them, except where noted:

| type | views | reach for it when |
|---|---|---|
| `span<T>` | a contiguous run, runtime length | the general parameter type for "some contiguous `T`s" |
| `fixed_span<T, N>` | a contiguous run, compile-time length | the length is part of the signature |
| `strided_span<T>` | elements at a constant byte stride | reading one field out of an array of structs |
| `pinned_data<T>` | a contiguous run, plus a shared owner | a view that must keep its own backing memory alive |

`cc::string_view` belongs to the same family — a borrowed range over storage it does not own — but lives in `string/` rather than here.
[strings](strings.md) owns its invalidation and hashing rules, which differ from the ones below only in who holds the bytes.

Associative — separate chaining over power-of-two buckets:

| type | reach for it when |
|---|---|
| `map<K, V>` | keys to values, with reference stability and heterogeneous lookup |
| `set<T>` | membership only; it is a `map<T, unit>` and inherits every property below |

`byte_stream_builder`, `key_value_cache` and `pair` also live in `container/` and are documented in their own headers.
`bitset`, `fixed_bitset` and `disjoint_set` are **empty stubs** — the headers exist so `fwd.hh` can name them, and nothing is implemented.

### Heterogeneous — `tuple` and `variant`

`tuple<Ts...>` is a product type and `variant<Ts...>` a sum type.
`tuple` is **index-based**: it has `get<I>` and no `get<T>`, so duplicate element types are fine and none of `std::`'s type-uniqueness rules apply.
`variant` goes the other way — its alternatives must be **pairwise distinct**, enforced by a static_assert, which is what lets it be keyed on type.

Alongside `visit`, a `variant` alternative can be named directly.
`is<T>()` asks, `as<T>()` asserts and forwards the value category, `try_as<T>()` hands out a pointer that is null on a different alternative.
`take<T>()` and `try_take<T>()` move the alternative out.
A `T` that is not an alternative does not compile, rather than being a `false`.
`take` leaves the variant **valid**, holding a moved-from alternative at the same index — the same state its own move constructor leaves behind.

Both are **trivially copyable and trivially destructible whenever every element or alternative is**.
For `tuple` that costs nothing to maintain: its elements are plain base classes, so every special member stays implicit.
`variant` builds on a recursive union and hand-writes its special members only for the non-trivial case, the way `optional` and `result` do.

`variant` models **no valueless state** — `index()` is always in range.
That holds because assignment destroys the active alternative and constructs the new one in its place, which is not exception-safe: an alternative whose move constructor can throw is not supported.
Default construction takes alternative 0, and the value constructor accepts **exact type matches only**, so `variant<bool, string> v = "hi"` is a compile error rather than a silent `bool`.
Use `create_emplaced<I>` — a prvalue factory, so it works for immovable alternatives — whenever a conversion is intended.

`tuple` and `pair` both **value-initialize** on default construction.

Still open: converting construction between tuples, `tuple_cat`, `variant`'s `operator<=>` and multi-variant visitation.

## What `T` must be

The **owning** containers require an object type that is not `const`: `array`, `vector`, `unique_array`, `unique_vector`, `small_vector`, `fixed_vector`, `ringbuffer`, `fixed_ringbuffer`.
References, functions and `void` are rejected, and so is `T const` — a container that constructs and destroys elements has to be able to write them.
`map` applies the same requirement to `K` and to `V` separately.

The views and `fixed_array` deliberately do not:

* `span`, `fixed_span`, `strided_span` and `pinned_data` take `T const` — that is how a read-only view is spelled;
* `fixed_array<T, N>` is a plain aggregate over `T[N]` and imposes nothing beyond `N >= 0`.

For the owning containers `T` need not be default-constructible, copyable or movable in general, since elements are constructed in place.
Which operations then remain available is per-type.
`map` and `set` accept immovable `K` and `V` because nodes are heap-allocated and never move.
Any container that reallocates needs `T` to be move-constructible before it can grow.

`small_vector` additionally requires `N >= 1`; `fixed_vector`, `fixed_array` and `fixed_ringbuffer` allow `N == 0` as a permanently empty container.

## Indexing and bounds

**Every runtime index is checked with `CC_ASSERT`, which means checked in debug and `relwithdebinfo` and unchecked in `release`** (unless `CC_ENABLE_ASSERT_IN_RELEASE` is defined).
An out-of-range index in a release build is undefined behavior, not a trap — the check is a development aid, not a safety guarantee.
This covers `operator[]`, the `remove_at*` / `remove_from_to*` / subspan ranges, and the `pop_at*` family.
The types that check: `vector`, `array`, `unique_*`, `small_vector`, `fixed_vector`, `ringbuffer`, `fixed_ringbuffer`, `span`, `strided_span`, `pinned_data`.
`front`/`back` assert on those too, but **not** on `fixed_array`, whose accessors are unchecked.

The `*_clamped` variants are the deliberate exception.
`subspan_clamped`, `first_n_clamped`, `last_n_clamped` and `pinned_data::subdata_clamped` clamp to the valid range instead of asserting.

Compile-time indices are different.
`get<I>()` on `fixed_array` and `fixed_span`, and the accessors on their zero-length specializations, are `static_assert`s — checked in every build.

Indices are signed (`cc::isize`).
A negative index is out of range like any other, rather than wrapping to a huge unsigned value.

See [platforms](../../../../docs/platforms.md) for the preset-to-build-type mapping, and `common/macros.hh` for `CC_ASSERT_ENABLED` itself.

## When references and iterators die

Pointers, references and iterators into a container stay valid until something moves the elements.
What moves them differs per family, and the difference is the reason to pick one:

* **`vector` and `unique_vector`** invalidate everything on reallocation.
  Growth past capacity reallocates, and reserving up front is what avoids it.
  Members with the `_stable` suffix never reallocate and never move live objects — they assert that the capacity is already there.
* **`array` and `unique_array`** never grow at all, so their elements move only when the container itself is assigned or destroyed.
* **`small_vector`** has `vector`'s reallocation behavior and its `_stable` members.
  It adds one transition the others do not have: the first spill from the inline buffer to the heap moves every element.
  A reference taken while `is_inline()` is true does not survive the spill.
* **`fixed_vector`** never reallocates, so growth is always safe.
  Removal is not: the order-preserving `remove_at` / `remove_from_to` shift the tail down, and `remove_at_unordered` moves the last element into the hole.
  References to elements after the removal point then refer to a different element.
* **`ringbuffer` and `fixed_ringbuffer`** never shift an element, which is a stronger guarantee than any of the above.
  A reference survives pushes and pops at either end, and dies only when the element itself is removed.
  The heap ring adds exactly one other way to lose one: growth, which the `_stable` members never trigger.
  `linearize()` is the exception on both — it moves whatever it had to move.
* **`map` and `set`** keep nodes heap-allocated and never move them, so element references survive unrelated insertions, erasures and growth — a rehash only relinks node pointers.
  That is `K&` and `V&` for a map, and `T const&` for a set.
  Their **entries and iterators** do not survive: any structural mutation invalidates those.
* **The views** own nothing, so the question belongs to whatever holds the storage.
  `pinned_data` is the exception: it shares ownership of the backing memory, so the view stays valid as long as any copy of it lives.

## Thread safety

None of these types synchronizes.
`map` and `set` state the useful special case: concurrent readers are fine because entries are self-contained, but any mutation needs external synchronization.

## Hashing and equality

The sequence containers define a structural, order-dependent `hash` hidden friend over their elements — the owning ones, and `span` / `fixed_span` too.
`map` and `set` require a well-mixed hash because only the low bits index the bucket array — the default hasher finalizes for you.
[customization-points](customization-points.md) owns that protocol.
