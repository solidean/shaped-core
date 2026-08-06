# cc::sequence

`cc::sequence<RangeT>` is a lazy, eval-at-most-once forward cursor over a range, with functional compositions on top.

**It is an early prototype.**
A small set of reductions and materializers works; the transformations that would make it a ranges API — `map`, `filter`, `take`, `zip`, `flatten` — do not exist yet, and neither do the factories.
Nothing in the repo uses it, and it has no tests.

This doc is in two halves, and the split is the point.
[What exists today](#what-exists-today) is checked against the header.
[The intended design](#the-intended-design) is design intent — a record of decisions already taken, not a description of code.
Do not read the second half as a list of things you can call.

## What exists today

```cpp
#include <clean-core/sequence/sequence.hh>

auto const n = cc::sequence{v}.count_if([](auto x) { return x > 0; });

// Careful: that spelling COPIES v.
// The only deduction guide is the implicit one from the by-value constructor parameter, so RangeT decays to cc::vector<int>.
// Spell cc::sequence<cc::vector<int>&>{v} to wrap the range by reference instead.
```

The wrapper is constructed explicitly from a range and consumed in place.
It is neither copyable nor movable *for now*, so it lives as a temporary or a named local and terminal operations consume the underlying range.

**Traits.** `element_t` is the element value type, `element_ptr_t` the pointer to it with constness preserved.
`has_stable_elements` is true when the range's iterator hands out references rather than values, which is what makes pointers into it meaningful.

**Reductions.** `count`, `count_if`, `any`, `all`, `index_of` (→ `cc::optional<isize>`), `find` (→ `element_ptr_t`), `accumulate`, `each`.
`find` static-asserts on `has_stable_elements`, which is not sufficient.
It builds its result in an `element_t*`, so over a `const` range it fails to compile on the `element_t const*` it actually has.
Every predicate and callback may take a leading `isize` index or omit it.
`count_if`, `any`, `all`, `index_of` and `find` dispatch through `cc::regular_invoke_with_optional_idx`.
That one maps a `void` step to `cc::unit`, which is how the fold tells "no early-out" from a `bool`.
`accumulate` and `each` use plain `cc::invoke_with_optional_idx`, which does not.

**Materialization.** `to_container<C>()`, `to_container<C>(map)`, `to_vector()`, `push_to(existing)`.
The header forward-declares `cc::vector` and no more, so a caller of `to_vector()` must include `<clean-core/container/vector.hh>` itself.

**The operational basis.** `try_fold_first(init, step)` and `try_fold(step)`, both returning `sequence_fold_result`.
Every reduction above is written in terms of `try_fold`; `try_fold_first` has no callers at all today.
Both use external iteration only: the dispatch to a range's own fold, which is the whole reason the protocol is shaped this way, is not implemented.

Two members are declared but not implemented:

* `sum()` returns `isize` with no return statement at all, so calling it is UB.
* `to_array()` deduces `void` from a bare `return;`, so calling it is well-defined and does nothing.

`sequence_fold_result` is declared at global scope rather than in `cc`, unlike everything else the header defines.

## The intended design

### Reduction and lifetime model

External iteration (range-for / cursor-next) is the universal fallback.
It is needed for composition like zip, and is always available.

Internal iteration is an *optional optimization protocol*.
Reductions dispatch to it when supported; otherwise they fall back to external.

The core internal primitive is an early-outable fold.
No wrapper types; control flow is expressed directly in C++.

`fold(init, step)` returns a fold outcome.
`init(idx, first_elem)` initializes state from the first element; `step(idx, state, elem) -> bool` advances it, returning true to stop early.

`fold_from_first(state, step)` assumes state is already initialized from the first element.
Non-empty adaptors use it to avoid heterogeneous loops.

The outcome separates three concerns: `empty` (no elements), `stopped` (step returned true), `completed` (full traversal, no early-out).

`step` may or may not accept an index.
The index is always available, and unused indices are optimized away.

`step` may return void (unit) or bool: void never early-outs, bool stops on true.
Dispatch is `if constexpr` on the return type — `regular_invoke_with_optional_idx`, then unit-vs-bool.

All reductions (min/max/sum/find/any/all/…) are implemented in terms of fold.
Only fold itself needs to choose internal vs external iteration.

Non-empty sequences created at runtime must buffer *exactly one* element.
This preserves eval-at-most-once and enables the `fold_from_first` fast paths, which keep hot loops branch-free and hand-written-loop-equivalent.

### Pointers, references and the traits that guard them

`min`/`max`/`sum` come in two styles: value-returning (generic, safe-ish) and into-style, which updates an existing accumulator.
The `*_into` APIs — `min_into(T&)`, `min_into(optional<T>&)`, `min_ptr_by_into(T*&)` — compose well across multiple ranges.

Pointer-based `*_ptr` / `*_ptr_by` APIs expose element identity, so they are only available when elements have stable addresses.

`has_stable_elements` means the sequence yields references to address-stable storage.
It propagates conservatively: filter preserves it, map-by-value breaks it.

`borrowed_elements` is the `borrowed_range` analogue: references and pointers stay valid even if the sequence object is a temporary.
It is what prevents pointers escaping from views over temporary owners.

Pointer-returning APIs require both `has_stable_elements && borrowed_elements` — a strong guard against dangling pointers.

Reference-returning `min()` may be allowed with only `has_stable_elements`.
That is safe for immediate use and may dangle if the reference escapes the full-expression, which matches how sharp `span` and `string_view` already are.

Return-type shape-shifting is intentional: `min()` returns `T&` for stable elements and `T` for non-stable.
`auto` / `auto&&` always works; `auto&` only when the result is truly a mutable reference.

The sharp edges stay explicit: underlying container invalidation still applies, trait propagation must stay conservative, and a `materialize()` exists for users who want ownership and safety.

### Internal vs external iteration, in full

Every wrapped range must support *external* iteration (begin/end).
This is the universal composition model: it chains indefinitely and works across arbitrary adaptors (zip, filter, map, …).

However, for many non-trivial ranges — trees, concatenations, flattened views, adaptor stacks — the iterator model becomes awkward and often inhibits inlining and optimization.

*Internal* iteration flips control flow: the range owns the loop and invokes a callback instead of yielding iterators.
This matches how such data structures are naturally implemented, and typically maps much more directly to optimal codegen.

Most sequence operations are meant to be implemented in terms of internal iteration (fold-style).
These internal layers compose well and are easier for the compiler to inline and collapse than deeply nested iterator machinery.

Rule of thumb: if an "internal iteration" is just

```cpp
for (auto&& v : *this) callback(v);
```

then it buys nothing, because it is equivalent to external iteration.
Internal iteration is only worthwhile if the range can do something structurally simpler or more direct than the iterator model.

### Shape of the API

Operations fall into three groups: transformative / sub-sequence, into-container-like (`to_array` / `to_vector`, sorted, grouped, …), and statistical (sum, average, min, max, find, count, …).

Properties propagate along a chain: fixed or bounded size, compile-time size, non-emptiness, indexability.
Sequences *can* be infinite.

Not copyable and not cloneable for now, and multi-pass needs a second `cc::sequence` rather than a rewind.
That is what makes the performance predictable.

Design decisions that constrain any future work here:

* each transformation must add only a single template "layer";
* `sequence` is the rich-API layer on top of a simple underlying range;
* it must stay lightweight enough as a header that every container can offer sequence members;
* the compiler must easily desugar everything into basically-optimal assembly;
* map-like overloads are included where appropriate, to reduce chaining count.

For authors: `RangeT` can be a reference, and that is encouraged.

Ideas not yet decided:

* a "reversible" range trait — every `.last_xyz` is then just `.reversed().xyz()`;
* factories: `make_sequence(container)`, `make_sequence(init-list)`, one for a single element, one for a generator, and a coroutine adapter.

## Editing the header

`sequence.hh` is included by all containers and a lot of other headers, so be careful about what it depends on.
