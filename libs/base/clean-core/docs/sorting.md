# Sorting

`cc::sort` and its family, in [algorithm/sort.hh](../src/clean-core/algorithm/sort.hh).
The [cheat sheet](../cheat-sheet.md) lists the whole surface; this page is the one idea behind it and how to extend it.

## The one idea: get and swap, never park

Every sort here is driven through two operations on indices:

```cpp
range.element_get(i)        // read the element (or a key derived from it)
range.element_swap(a, b)    // exchange two positions
```

Nothing else.
No element is ever moved into a temporary, not even for a pivot, not even inside the small sort.

That single restriction is what `std::sort` cannot honour and what buys the family its distinguishing feature:
**one sort can permute several ranges at once.**
A move-based sort has to hoist an element into a hole; with three parallel arrays there is no single element to hoist.
A swap-based one just swaps three times.

```cpp
cc::sort_multi(cc::default_less{}, distances, hit_ids, normals);   // one order, three arrays
```

It costs something too, and the cost is real: swapping to shift by one position writes three times where a move-based insertion sort writes once.
[benchmarks/sort-benchmark](benchmarks/sort-benchmark.md) measures exactly that against `std::sort`.
The short version: ~2× slower on random input at n = 16, 2.5× faster on random input at a million, and up to 9.9× faster on the patterned shapes.

## The algorithm

pdqsort — pattern-defeating quicksort — with every part of it, expressed over the two operations above.

* **Insertion sort** below 16 elements, guarded or unguarded depending on whether a bounding element is known to sit before the range.
* **Pivot** from a median of three, or a pseudomedian of nine past 128 elements.
* **Partition** with equal elements going right, classified branchlessly through an offset buffer for small trivially-copyable elements.
  Only offsets are buffered — no element leaves the range.
* **The equal-elements partition**, taken when the element before the range equals the pivot.
  This is what keeps duplicate-heavy input out of quadratic behaviour, and the piece that matters most on real data.
* **Partial insertion sort** on an already-partitioned split, which is where near-linear behaviour on nearly-sorted input comes from.
* **A deterministic partial shuffle** after a badly unbalanced split, so a pattern cannot go on producing them.
* **Heapsort** once the bad-split budget is spent, which bounds a full sort at O(n log n).
  Heapsort is swap-only, so it drops straight in.

`cc::quickselect` runs the same machinery with a subrange predicate that prunes everything not covering the index.
Its fallback differs: a spent budget switches the pivot to a **median of medians**, which keeps a pruned run linear rather than O(n log n).

Deterministic, and **not stable**.
Where stability matters, `cc::sort_indices` gives it: sorting a `0..n-1` index array breaks ties on the index, so equal keys keep their order.

## A comparator must be a strict weak ordering

The partition scans are deliberately unbounded — that is where a good part of the speed comes from.
A comparator that is not a strict weak ordering therefore walks off the range.

Rather than let that corrupt memory, every unbounded scan carries a `CC_ASSERT`, so the failure is a named contract violation in debug and relwithdebinfo builds and costs nothing in release.
If a sort asserts with *comparison function is not a strict weak ordering*, the comparator is the bug, not the sort.

The usual causes: `<=` where `<` was meant, comparing floats that can be NaN, or a comparator whose answer depends on state that changes during the sort.

## Writing your own adapter

`cc::vector` deliberately does **not** model `cc::index_swap_range` — an adapter over it does.
`cc::as_index_swap_range(values)` builds the plain one, and the other factories cover keys and parallel ranges.

Reach past them when your data is not a range at all.
The adapter needs the two members and nothing else — no `size()`, because the subrange being worked on changes constantly and travels as separate arguments.

```cpp
// a structure-of-arrays view: ordered by one array, permuting three
struct particle_range
{
    cc::span<f32> keys;
    cc::span<tg::pos3> positions;
    cc::span<tg::vec3> velocities;

    f32 element_get(isize i) const { return keys[i]; }
    void element_swap(isize a, isize b) const
    {
        cc::swap(keys[a], keys[b]);
        cc::swap(positions[a], positions[b]);
        cc::swap(velocities[a], velocities[b]);
    }
};

cc::sort_ex(0, n, particle_range{keys, positions, velocities}, cc::default_less{}, cc::constant_function<true>{});
```

Three rules bind an adapter:

* **It must be trivially copyable, and cheap.**
  It is copied down the recursion, which is what keeps the base pointers in registers instead of behind an indirection.
  Hold pointers or spans, never containers by value — `sort_ex` static-asserts this.
* **`element_get` may return a reference or a value.**
  A computed key is fine; it is simply recomputed on every comparison, which is why `cc::sort_by_cached_key` exists.
* **`element_swap(a, b)` is never called with `a == b`**, and must permute *every* range the adapter covers, or they fall out of step.

The last argument to `sort_ex` is the subrange predicate.
`cc::constant_function<true>{}` sorts everything; anything else prunes, which is how `quickselect` is built out of the same call.

## What is not here yet

* **`stable_sort`** — it needs an auxiliary buffer, which the no-parking rule forbids.
  `sort_indices` covers most of what it is wanted for.
* **`lower_bound` / `upper_bound` / `binary_search`** — the natural neighbours, planned for an `algorithm/search.hh`.
* **A parallel `sort_async*` family** — the swap-only design makes the fan-out natural: disjoint subranges, no merge step.
  It reshapes the driver rather than wrapping it, so it gets its own header.
