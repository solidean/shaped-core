# Blessed stdlib headers

clean-core sits at the bottom of the library stack and otherwise avoids `std::`
(see the repo [Standard Library & Dependencies](../../../../docs/coding-guidelines.md)
guideline — almost everything has a `cc::` equivalent). A small set of standard
headers is **blessed**: re-creating them is infeasible or pointless because they
are thin wrappers around compiler/runtime machinery, not data structures we want
to own. These may be included directly in clean-core.

| Header               | Why it's blessed                                                        |
|----------------------|-------------------------------------------------------------------------|
| `<type_traits>`      | Thin wrappers around compiler intrinsics; no value in re-wrapping.      |
| `<typeinfo>`         | `typeid` / `std::type_info` are language-level RTTI, not reimplementable.|
| `<typeindex>`        | `std::type_index` — the hashable/orderable handle over `std::type_info`. |
| `<atomic>`           | `std::atomic` maps to compiler/hardware atomics; re-creating it is unsafe.|
| `<initializer_list>` | Required by the language for braced-init-list constructors.             |

The list grows by **targeted addition only**: add a header here (with its
justification) when a concrete need arises, not pre-emptively. Anything not listed
should go through a `cc::` equivalent.
