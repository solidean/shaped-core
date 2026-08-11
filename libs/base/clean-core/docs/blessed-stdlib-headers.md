# Blessed stdlib headers

clean-core sits at the bottom of the library stack and otherwise avoids `std::` — almost everything has a `cc::` equivalent.
The repo guideline is [Standard Library & Dependencies](../../../../docs/coding-guidelines.md).
A small set of standard headers is **blessed**: re-creating them is infeasible or pointless.
They are thin wrappers around compiler/runtime machinery rather than data structures we want to own.

Blessing has two tiers, because "we will not reimplement this" and "call it directly" are different claims:

| Header               | Direct use | Why                                                                     |
|----------------------|-----------|-------------------------------------------------------------------------|
| `<type_traits>`      | yes       | Thin wrappers around compiler intrinsics; no value in re-wrapping.      |
| `<typeinfo>`         | yes       | `typeid` / `std::type_info` are language-level RTTI, not reimplementable.|
| `<typeindex>`        | yes       | `std::type_index` — the hashable/orderable handle over `std::type_info`. |
| `<initializer_list>` | yes       | Required by the language for braced-init-list constructors.             |
| `<compare>`          | yes       | Required by the language for `operator<=>`: the comparison categories are what it returns, and `= default` needs them too. Name `std::strong_ordering` (or `weak_`/`partial_`) in the return type; everything else about ordering stays in `cc::`. |
| `<chrono>`           | yes       | Wall/monotonic clocks are OS facilities, and the unit-safe `duration`/`time_point` algebra is exactly what we would rewrite. Use `steady_clock` for elapsed time, never `system_clock` (it can jump). A `cc::` time vocabulary may still land later for the *formatting* / serialization side. |
| `<atomic>`           | **no — via [`cc::atomic`](../src/clean-core/thread/atomic.hh)** | `std::atomic` maps to compiler/hardware atomics, so we do not reimplement it — with threads `cc::atomic` *is* `std::atomic`. But a build can have no threads at all (`CC_HAS_THREADS == 0`), and there the counts should be plain loads and stores. Only a `cc::` seam can drop the atomicity; a hand-written `std::atomic` stays a `lock xadd` no flag can reach. |

**Tier 1 — blessed to include and call.** Use them directly.

**Tier 2 — blessed to appear, not to call.**
The header may leak through our public includes — `clean-core/thread/atomic.hh` includes `<atomic>`, and that is fine.
But code outside its `cc::` wrapper must not name the `std::` facility.
`cc::atomic` / `cc::atomic_ref` / `cc::atomic_flag` / `cc::atomic_thread_fence` / `cc::memory_order` cover every use clean-core has.

**The include half is enforced.**
[`.shaped-lint.yml`](../.shaped-lint.yml) carries the machine-checked list, and shaped-linter's `blessed-includes` rule reports every angle include nothing above it blesses.
The file format is [configuration](../../../../tools/shaped-linter/docs/configuration.md)'s.
This table is the *argument*; that file is the gate, and the two are kept in step by hand.

**The second tier is still a review rule**, because it is a claim about symbols rather than includes.
`<atomic>` appearing in a header is exactly what the config allows, and no include rule can see a `std::atomic` written below it.
The tell is that such a line compiles and passes on every threaded preset.
Only the single-threaded preset that `dev.py check` runs would notice, and only if the type is on a path that build exercises.

The list grows by **targeted addition only**: add a header here, with its justification and its tier, when a concrete need arises rather than pre-emptively.
Anything not listed should go through a `cc::` equivalent.
A library that genuinely needs one blesses it in its own `.shaped-lint.yml`, which is where the deviation stays visible instead of widening this list.
