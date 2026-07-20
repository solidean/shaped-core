# Blessed stdlib headers

clean-core sits at the bottom of the library stack and otherwise avoids `std::`
(see the repo [Standard Library & Dependencies](../../../../docs/coding-guidelines.md)
guideline — almost everything has a `cc::` equivalent). A small set of standard
headers is **blessed**: re-creating them is infeasible or pointless because they
are thin wrappers around compiler/runtime machinery, not data structures we want
to own.

Blessing has two tiers, because "we will not reimplement this" and "call it
directly" are different claims:

| Header               | Direct use | Why                                                                     |
|----------------------|-----------|-------------------------------------------------------------------------|
| `<type_traits>`      | yes       | Thin wrappers around compiler intrinsics; no value in re-wrapping.      |
| `<typeinfo>`         | yes       | `typeid` / `std::type_info` are language-level RTTI, not reimplementable.|
| `<typeindex>`        | yes       | `std::type_index` — the hashable/orderable handle over `std::type_info`. |
| `<initializer_list>` | yes       | Required by the language for braced-init-list constructors.             |
| `<chrono>`           | yes       | Wall/monotonic clocks are OS facilities, and the unit-safe `duration`/`time_point` algebra is exactly what we would rewrite. Use `steady_clock` for elapsed time, never `system_clock` (it can jump). A `cc::` time vocabulary may still land later for the *formatting* / serialization side. |
| `<atomic>`           | **no — via [`cc::atomic`](../src/clean-core/thread/atomic.hh)** | `std::atomic` maps to compiler/hardware atomics, so we do not reimplement it — with threads `cc::atomic` *is* `std::atomic`. But a build can have no threads at all (`CC_HAS_THREADS == 0`), and there the counts should be plain loads and stores. Only a `cc::` seam can drop the atomicity; a hand-written `std::atomic` stays a `lock xadd` no flag can reach. |

**Tier 1 — blessed to include and call.** Use them directly.

**Tier 2 — blessed to appear, not to call.** The header may leak through our
public includes (`clean-core/thread/atomic.hh` includes `<atomic>`, and that is
fine), but code outside its `cc::` wrapper must not name the `std::` facility.
`cc::atomic` / `cc::atomic_ref` / `cc::atomic_flag` / `cc::atomic_thread_fence` /
`cc::memory_order` cover every use clean-core has.

This is not enforced by tooling — it is a review rule. The tell is that
`std::atomic` in a diff compiles and passes on every threaded preset, and only
the single-threaded preset (which `dev.py check` runs) would notice, and only if
the type is on a path that build exercises.

The list grows by **targeted addition only**: add a header here (with its
justification, and which tier) when a concrete need arises, not pre-emptively.
Anything not listed should go through a `cc::` equivalent.
