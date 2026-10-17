# cc::sequence

`cc::sequence<Source>` is a lazy, single-pass, eval-at-most-once sequence over a *source*, with functional compositions on top.
Containers hand one out through `values()`, and indexed ones `indices()` too.

```cpp
auto const n = v.values().filter([](int x) { return x > 0; }).count();
auto const s = v.values().assert_non_empty().sum();
auto const m = v.values().min_or(0);
if (v.indices().contains(i)) { ... }
```

**Its contract is codegen parity.** A simple use must compile to what the hand-written loop does, beyond inherent noise, or nobody should prefer it.
[tests/sequence/sequence-codegen-test.cc](../tests/sequence/sequence-codegen-test.cc) pins each shape beside its naive loop, for `dev.py assembly show` on a release preset.

This doc is in two halves.
[What exists today](#what-exists-today) is checked against the headers.
[The design](#the-design) records the decisions, including the parts not built yet, and says which those are.

## What exists today

**Entry points.** `values()` and `indices()` on every indexed container.
That is `vector`, `array`, `unique_vector`, `unique_array`, `small_vector`, `fixed_vector` and `fixed_array`.
It is also the views `span`, `fixed_span` and `strided_span`, and the ringbuffers.
`map` has `keys()` and `values()`, and `set` has `values()`.
`values()` on a temporary container moves it into the sequence; on an lvalue it references it.
A ringbuffer is the exception: its elements wrap, so a temporary one has no `values()` rather than a hidden `linearize()`.
`fixed_array<T, N>` and `fixed_span<T, N>` with `N > 0` hand out sequences that are non-empty at compile time.
The bitsets have no entry points yet.
`cc::iota(begin, end)` and `cc::zip(a, b, ...)` are the free factories.

**Reductions.** `count`, `count_if`, `any`, `all`, `none`, `contains`, `index_of` (→ `cc::optional<isize>`), `each`, `consume`.
`find` returns a pointer, so it takes borrowed elements only, and `try_fold` is the general early-out fold.
Over a first element: `sum`, `min`, `max`, `average`, `first`, `last`, `min_by`, `max_by` (non-empty only).
Their counterparts for any sequence: `sum_from`, `min_or`, `max_or`, `average_or`, `first_or`, `try_min`, `try_max`, `try_average`, `try_first`, `try_last`.
`sum(f)`, `min(f)`, `max(f)` and `average(f)` reduce a projection: `ps.values().assert_non_empty().sum(&particle::mass)`.

**Transformations.** `map`, `filter`, `take`, `skip`, `take_while`, `skip_while`, `chain`, `zip`, `enumerate`, `flatten`, `flat_map`, `indices`.

**Non-emptiness.** `assert_non_empty()` and `try_as_non_empty()`.

**Materialization.** `to_container<C>()`, `to_vector()`, `push_to(existing)`.
`to_vector()` needs `<clean-core/container/vector.hh>`, which the sequence header cannot include.
Materializing a zip makes `cc::tuple` elements, which need `<clean-core/container/tuple.hh>`: the sequence header leaves it out of every container's closure.

**Range-for** works on every sequence and consumes it; over a zip it binds `auto&& [a, b]`.

**Laziness skips work, and that includes side effects.**
`count()` over a sized `map` returns the size without calling `f`, and so do `indices()`, `take` and `skip` over a source that knows its positions.
`consume()` is the terminal operation that evaluates every element, for a chain kept for what its callbacks do.

**The headers.** [sequence/source.hh](../src/clean-core/sequence/source.hh) holds the source protocol and the built-in sources: span, iota, strided, owning and non-empty.
[sequence/impl/call.hh](../src/clean-core/sequence/impl/call.hh) is how a callback is called: the optional index, and a zip's items spread.
[sequence/impl/adaptors.hh](../src/clean-core/sequence/impl/adaptors.hh) holds the single-input adaptors.
[sequence/impl/combinators.hh](../src/clean-core/sequence/impl/combinators.hh) holds chain, zip and flatten.
[sequence/sequence.hh](../src/clean-core/sequence/sequence.hh) is `cc::sequence` itself.
The map's sources live in [map.hh](../src/clean-core/container/map.hh), since its nodes are private.

**Examples**, from basic to complex: `clean-core/sequence-basics`, `clean-core/sequence-non-empty` and `clean-core/sequence-pipelines`, in [examples/](../examples/).

## The design

`cc::sequence` borrows ideas from std ranges and Rust iterators, but takes none of their decisions on trust.
Each choice below is argued from what it buys here, and std or Rust doing it differently is not a reason to reopen one.

### Two layers: sources and the sequence

A *source* is the minimal protocol a container or adaptor implements.
`cc::sequence<Source>` is the rich API on top of it, and holds nothing but the source.
A transformation wraps the source in exactly one adaptor source and rewraps that in `sequence`, which is the single-template-layer rule below.
[source.hh](../src/clean-core/sequence/source.hh) states the protocol in full; `span_source` there is the reference implementation.

* **`item_t`** is what one step yields: `T&`, `T const&`, or a prvalue `T`.
  It, not the decayed element type, is the type the fold path forwards.
* **`next()`** is required, and returns `T*` for reference items and `impl::seq_slot<T>` for prvalues, empty at the end.
  External iteration is `next()` rather than an iterator pair because an iterator can be dereferenced twice, and `next()` yields each element once by construction.
  That is what eval-at-most-once rests on: map-then-filter evaluates the map once per element without any discipline.
  The slot is our own rather than `cc::optional`, whose checked `value()` would sit in every step, and whose header the containers cannot include early enough.
* **`try_fold(step)`** is optional, and is the only internal-iteration hook.
  `step(item) -> bool` returns true to stop; `try_fold` returns whether it was stopped.
  It must spell its return type `bool`, because detection reads the declaration and never instantiates the body.
* **The capabilities** are a small, closed list, each with a fallback the sequence layer provides: `size` (counting), `is_empty`, `contains` (`any(==)`), `begin`/`end` (`next()`).
  Three more come with `size`, for a source that knows its positions: `at(i)`, `advance(n)` and `truncate(n)`.
  They are what let `take` and `skip` reshape a contiguous source instead of wrapping it, `last()` answer without a traversal, and a zip run as one index loop.
  Later perhaps reverse iteration.
  An adaptor forwards a capability wherever it can: `filter(p).contains(x)` is `inner.contains(x) && p(x)`, and `map(f).at(i)` is `f(inner.at(i))`.

The calling contract: `next()` any number of times, then `try_fold` at most once, after which the source is spent.
A source therefore need not leave a resume point behind `try_fold`, which is what lets a tree simply recurse inside it.

### Internal iteration

External iteration chains indefinitely across arbitrary adaptors, which is why every source must support it.
But for trees, concatenations, flattened views and adaptor stacks, the iterator model is awkward and often inhibits inlining.
Internal iteration flips control: the source owns the loop and calls back, which matches how such structures are naturally written and maps much more directly to optimal codegen.

Every adaptor composes internally except one side of a zip:

| adaptor           | its `try_fold`                                                                |
|-------------------|-------------------------------------------------------------------------------|
| map, filter, take | wraps `step`                                                                  |
| chain             | `a.try_fold(s) \|\| b.try_fold(s)`                                            |
| flatten           | `outer.try_fold([&](auto&& in) { return in.values().try_fold(s); })`          |
| zip               | `a.try_fold([&](auto&& x) { auto y = b.next(); return !y \|\| s(x, *y); })`   |

Zip drives its first side internally and its second externally, which is the composition that keeps external iteration the universal base.

Rule of thumb: an internal iteration that is just

```cpp
for (auto&& v : *this) callback(v);
```

buys nothing, because it is external iteration.
It is only worth writing when the source can do something structurally simpler than the iterator model.

Folding from the first element is not part of the protocol.
The sequence layer calls `next()` once, initializes the state from that element, then hands the rest to `try_fold`.
For a span that first step is a pointer bump; for a flatten or a tree it is one external step ahead of the fast internal loop.
So no source tracks whether it saw anything: `empty` comes from that one `next()`.

All reductions are written in terms of `try_fold`, and only the sequence layer chooses between it and the `next()` fallback.

### What the codegen probes established

* **A contiguous source folds by index, not by stepping a pointer.** A callback's index counter then fuses with the loop's own; stepping a pointer left two induction variables in `index_of`.
* **`next()` signals the end with null, which LLVM cannot see through.** It cannot prove a pointer from `_p++` non-null, so a loop over `next()` loses its trip count and does not vectorize.
  Rust escapes this with `NonNull` slice pointers; an empty span's null data pointer rules that out here.
  So a source that is itself a pointer range offers `begin()`/`end()`, and range-for uses them directly.
* **A prvalue slot does not have the problem.** Range-for over `map` vectorizes like the naive loop, since its emptiness is a flag the optimizer tracks.
* **A sized zip is one index loop.** Every side's `at(i)` under one counter, so a dot product over two spans is the naive loop, register allocation aside.
  `enumerate` is a zip with an unbounded index range and compiles to the hand-indexed loop.
* **A map's own fold beats its iterator.** Summing `m.values()` walks bucket and chain as a plain double loop, a third the size of iterating the map by hand.
* **Range-for over a filter is the one shape without parity.** It is a tight scalar loop, since external iteration through a filter is a nested loop, while the naive `if` inside a loop vectorizes.
  `.filter(p).each(body)` is the spelling with parity, and the one a hot loop wants.

### Callbacks: the optional index and spread elements

A callback may take a leading `isize` index or omit it; the no-index form is tried first.
The index is the position in the sequence the callback is applied to: a filter's predicate counts its input, and a map after the filter counts the elements that passed.
The sequence layer adds the counter only when `if constexpr` sees the callback take one, so an unused index costs nothing without relying on the optimizer.
A `try_fold` step may return void or bool: void never stops, bool stops on true.
`each` ignores whatever its callback returns, so it never stops early.

A zip spreads its elements into separate arguments, `[](auto& a, auto& b)`, because a lambda cannot destructure a tuple parameter and the spread form inlines best.
Its element type is still a tuple for materialization.
The optional index stays unambiguous, since the indexed form has exactly one parameter more.
Variadic and defaulted lambdas are the known exception, documented rather than designed around.
The forms are tried in order: spread, index then spread, the pack whole, index then the pack.
So a callback taking `(auto&& p)` over a zip gets the pack, which has the tuple protocol.

`chain` needs both sides to share an element type, and merges how they yield it.
`T&` with `T const&` chains as `T const&`, so nothing is copied and pointers still escape.
A prvalue on either side makes the chain yield prvalues, which copies every element of the reference side.
Different element types refuse rather than meet in a common type, since that converts silently: `int` with `unsigned` would be `unsigned`, and a negative `int` a huge one.
A derived and a base class would meet at the base, and slice when materialized.

### Consumption, ownership and lifetime

* **Sequences are move-only.** Non-copyable keeps single-pass honest; movable is what `try_as_non_empty()`, zip's by-value arguments and lambda captures need.
* **Terminal operations are `&&`-qualified.** `v.values().sum()` compiles, `s.sum()` on a named local does not, and `cc::move(s).sum()` makes the consumption visible.
  This turns "never reuse a spent sequence" from a convention into a compile error, and a second pass needs a second sequence.
  Range-for is the exception, since the language calls `begin()` on an lvalue; the loop consumes the sequence all the same.
* **An lvalue container's `values()` references it.** Like `span`, the sequence must not outlive the container and is never stored.
* **An rvalue container's `values() &&` owns it**, moving the container into an owning source.
  A container returned by a function needs no variable: `make_vec().values().filter(p).to_vector()` is safe, and so is naming the result.
  Its elements are stable but not borrowed, so `find()` refuses it, and materializing moves them out rather than copying.

### Non-emptiness

Non-emptiness is a compile-time trait of the source, `is_non_empty`, and the API shape depends on it.
Adaptors propagate it:

| source              | non-empty when       |
|---------------------|----------------------|
| map                 | its input is         |
| filter              | never                |
| `chain(a, b)`       | `a` or `b` is        |
| `zip(a, b)`         | `a` and `b` are      |
| `take`, `skip`      | never                |
| `fixed_span<T, N>`  | `N > 0`              |
| iota, repeat        | always               |

`repeat` is not built; the rest of the table is what the adaptors do.
A non-empty source hides `advance` and `truncate`, since either may empty it, so `take` over one wraps it rather than reshaping it.

The trait describes the source before it is consumed, and the `&&` rule is what keeps it true: nothing can call into a sequence after its first consuming call.

Two wrappers establish it at runtime:

```cpp
v.values().assert_non_empty().sum();

if (auto s = v.values().filter(p).try_as_non_empty(); s.has_value())
    use(cc::move(s.value()).sum());
```

A source with a cheap `is_empty()` costs nothing here: the wrapper checks it and flips the trait.
A source without one (filter, flatten) can only find out by evaluating, so the wrapper pulls the first element and buffers *exactly one*.
That preserves eval-at-most-once and keeps the fold-from-first path a branch-free, hand-written-loop-equivalent hot loop.

A reduction that needs a first element comes in a fixed grammar:

| form                                   | valid on       | on empty          |
|----------------------------------------|----------------|-------------------|
| `min()`, `max()`, `sum()`, `average()` | non-empty only | does not compile  |
| `min_or(x)`                            | any            | returns `x`       |
| `try_min()`                            | any            | `cc::optional`    |
| `sum_from(init)`                       | any            | returns `init`    |

The non-empty-only form static-asserts with a message naming both ways out, `assert_non_empty()` and the `_or` / `_from` / `try_` variants.
`sum_from` is a name rather than a `sum(init)` overload because `sum(f)` is the map-like overload (`sum(&particle::mass)`).
Its `init` picks the accumulator type, `bytes.values().sum_from(i64(0))`, and there is no default-constructed variant: the zero stays explicit.
`acc + x` must have `init`'s type, so `sum_from(0)` over floats does not compile rather than truncating every step.
`sum()` returns `decltype(x + x)`, so `u8` elements sum as `int`.
`sum()` follows `min()` and `max()`, which have no identity at all, rather than special-casing one.
`T{}` is not an identity every type promises: a type may not default-construct, or may default to something other than its additive zero.
`sum_from(init)` makes the zero explicit, at the one call site that chose it.
`average()` is `sum / count` and nothing cleverer, since that is what the hand-written loop does.
Integers sum in 64 bits and divide as `f64`, floating point stays in its own type.
Any other type divides its sum by its `scalar_t`, which every typed-geometry value type names, so `tg::vec3f` averages to a `vec3f`.
`tg::pos` does not average: `pos + pos` is a translation, but a pos does not divide by a scalar.
An empty average has no answer, `0 / 0`, so `average()` is non-empty-only like `min()`, with `average_or` and `try_average` beside it.

### Pointers, references and the traits that guard them

`has_stable_elements` means the sequence yields references to address-stable storage.
It propagates conservatively: filter preserves it, map-by-value breaks it.

`has_borrowed_elements` is the `borrowed_range` analogue: references and pointers stay valid even if the sequence object is a temporary.
It is what keeps a pointer from escaping a sequence that owns its container.
Pointer-returning APIs (`find` today) require it.

**`min()` and `max()` return by value.**
A reference result would track the best element by pointer, and a pointer-tracking minimum does not vectorize where the naive `m = x < m ? x : m` does.
Element identity belongs to pointer-returning variants (`min_ptr`, `min_ptr_by`), gated on `has_borrowed_elements` like `find`; they are not built yet.

Into-style variants — `min_into(T&)`, `min_into(optional<T>&)`, `min_ptr_by_into(T*&)` — update an existing accumulator and compose across several ranges; also not built yet.

The sharp edges stay explicit: underlying container invalidation still applies, trait propagation must stay conservative, and a `materialize()` exists for users who want ownership and safety.

### Containers: values(), indices(), keys()

Every cc container and container view offers its contents as a sequence, and which entry points it has follows from how it is addressed:

| container                         | entry points            |
|-----------------------------------|-------------------------|
| indexed (vector, span, array, …)  | `values()`, `indices()` |
| keyed (maps)                      | `keys()`, `values()`    |
| neither (sets, …)                 | `values()`              |

`indices().contains(i)` is the O(1) range check and `keys().contains(k)` is the container's own lookup, both through the `contains` capability.

`values().indices()` is the index sequence of the value sequence, so it exists on every container through `values()`.
Over a sized source it is `iota(0, size)` and evaluates nothing; otherwise it counts while consuming, evaluating the source as it goes.
Like the callback index, it numbers *this* sequence: `v.values().filter(p).indices()` is `0..k`, and the original positions are `v.indices().filter(…)`.

### Shape of the API

Operations fall into three groups: transformative / sub-sequence, into-container-like (`to_array` / `to_vector`, sorted, grouped, …), and statistical (sum, average, min, max, find, count, …).

Properties propagate along a chain: fixed or bounded size, compile-time size, non-emptiness, indexability.
Sequences *can* be infinite.

Not copyable and not cloneable for now, and multi-pass needs a second `cc::sequence` rather than a rewind.
That is what makes the performance predictable.
A chain wanted twice is a lambda returning it, `auto evens = [&] { return v.values().filter(even); };`.

Design decisions that constrain any future work here:

* each transformation must add only a single template "layer";
* `sequence` is the rich-API layer on top of a simple underlying source;
* it must stay lightweight enough as a header that every container can offer sequence members;
* the compiler must easily desugar everything into basically-optimal assembly, and a new shape gets a probe in the codegen test;
* map-like overloads are included where appropriate, to reduce chaining count.

Known costs, accepted:

* **Layered lambdas are slow at `-O0`.** The relwithdebinfo default optimizes them away; a debug build pays for every layer.
* **A gated member still exists.** `sum()` static-asserts rather than being constrained, so `requires { s.sum(); }` is true on a sequence that may be empty.
  The error message is worth more than the detection.

Ideas not yet decided:

* a "reversible" range trait — every `.last_xyz` is then just `.reversed().xyz()`;
* factories: one for an arbitrary begin/end range, one for an initializer list, one for a single element, one for a generator, and a coroutine adapter.

## Editing the headers

Every container reaches these headers, so be careful about what they depend on.
The include order is a cycle that only works one way round.
`source.hh` is included before the containers are defined and must not reach `optional` or `span`.
`sequence.hh` needs `optional`, and `span.hh` pulls it in at its bottom, beside `optional.hh`.
So a container member body naming a type that is complete only later (`cc::sequence`, `cc::optional<isize>`) spells it through `impl::seq_dependent`.
That defers the lookup to instantiation.
