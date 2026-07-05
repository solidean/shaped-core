# Invocable tests (parametrized / data-driven / generator)

`INVOCABLE_TEST` declares a test that takes **arguments**; `nx::invoke_tests` runs
every invocable test matching a signature, feeding it data a *driver* test produced.
The two names go together: an `INVOCABLE_TEST` is inert until you `invoke_tests` it.

This one mechanism covers what other frameworks split into several features:

- **parametrized tests** — one body, many argument sets;
- **data-driven tests** — the arguments are a "case" loaded from a file / JSON / dir;
- **generator tests** — a driver generates a matrix of configs/enums and feeds each.

The point is to **separate the test from its data**: one body, data driven however
you like. Namespace `nx`; include `<nexus/test.hh>` (pulls in `nx::invoke_tests`).

## The two pieces

```cpp
#include <nexus/test.hh>

// 1. An invocable test. It takes arguments and is INERT — a normal sweep never runs
//    it. `params` is a parenthesized parameter list; the body follows with no
//    trailing ';'. Trailing config items compose as with TEST.
INVOCABLE_TEST("mesh - decimate", (mesh_case const& c), nx::config::seed(3))
{
    CHECK(decimate(c).is_manifold());
}

// 2. A driver: an ordinary test that produces data and invokes it. Every
//    INVOCABLE_TEST whose (decayed) argument signature matches runs as a child.
TEST("mesh cases")
{
    for (auto const& f : list_dir("data/mesh"))
        nx::invoke_tests(f.name, load_case(f));   // one child run per file
}
```

`nx::invoke_tests(name, args...)`:

- Runs every `INVOCABLE_TEST` whose signature matches `args...` (see *matching*
  below), passing them. Each match runs as an **addressable child** under the
  section segment `name`, with the test's own name and sections nested below.
- Returns an `invocation_result { matched, executed }`.
- **Leave the template argument to deduce** by default (`invoke_tests("case", x)`);
  the key is the decayed types of `args...`. Spell it explicitly
  (`invoke_tests<sg::context_handle>(...)`) only to pin a type deduction can't reach.
- `name` is **authored**, never derived from a value — so output and addresses stay
  stable even when an argument (e.g. a handle) has no meaningful string form.

## Mental model: addressable iteration, not sections

`SECTION` is *parallel exploration* — the body re-runs once per leaf. `invoke_tests`
is *linear iteration*: the driver body runs **once**, and each matched test is driven
to completion internally (its own sections replayed inside a nested run). That is what
lets a driver do expensive setup **once**, run everything, then tear down:

```cpp
TEST("sg backend - vulkan")
{
    auto ctx = sg::make_context(sg::backend::vulkan);   // once
    nx::invoke_tests("vulkan", ctx);                     // every sg test, one context
}                                                        // ctx destroyed once
```

Put the setup at the top of the driver body and invoke there. **Don't** combine a
top-level `invoke_tests` with sibling `SECTION`s in the same driver — the sections
force the driver body to replay, which re-runs the invocation (and the setup) per pass.

## Matching: prefer a unique key type

The join key is the **decayed** argument signature (`T` and `T const&` are the same
key; matching is exact on the decayed types otherwise). This means **every**
`INVOCABLE_TEST(int)` is matched by **any** `invoke_tests(int)` — matching is
deliberately coarse.

- **Do** use a domain-specific type as the key (`sg::context_handle`, a `mesh_case`,
  or an invented tag type). That makes matches unambiguous and orphan detection exact.
- Matching on a bare primitive (`int`, `bool`) with no other discriminator is
  **discouraged** — you will co-invoke unrelated tests.

Parameters must be **by value or `const&`** — a mutable lvalue-reference parameter is
rejected at compile time (arguments are boxed by decayed value and shared read-only
across the matched tests, so a `T&` would silently share/mutate one box). Prefer
cheap-to-copy / handle types; pass large data behind a handle or `case const*`.

## Addressing a single instance

An instance's address is its section path: `<driver name>` (matched by the test-name
filter) then `<invoke name> / <test name> / <its sections>` (matched by `-c`):

```bash
uv run dev.py test "sg backend - vulkan" -c vulkan "sg - clears backbuffer"
```

runs just that one instance on just that backend. Nesting composes: an invoked test
can itself invoke tests, and the paths stack.

> Bare-name addressing of an instance (`dev.py test "sg - clears backbuffer"` with no
> driver) is **not** supported yet — it needs a driver→instance map that only exists
> at run time. Use the `driver … -c …` form above. (A future alias mechanism may add
> the shorthand.)

## Orphan safety net

In a **full, unfiltered normal run**, every enabled `INVOCABLE_TEST` must be invoked by
some driver. Any that isn't fails the run:

```
Orphan invocable tests (declared but never invoked):
  sg - clears backbuffer at .../sg_tests.cc:42
```

So you can't declare an invocable test and forget to wire it into a driver. The check
is silent under any filter (`dev.py test "<pattern>"`, `-c`, `--manual`, …).

## Patterns

- **Backend matrix** — one driver per backend, each invoking on the backend handle
  (above). Setup/teardown happens once per backend.
- **Data-driven** — one driver loads cases and invokes each; write several
  `INVOCABLE_TEST`s on the same case type to check different aspects, and one
  `invoke_tests` feeds them all.
- **Config/enum matrix** — a driver loops over configs and invokes each; use a
  distinct config/tag type as the key.

## Templated tests (not yet implemented)

A *type*-parametrized test is a compile-time expansion (`name<int>`, `name<float>`),
not a registry query: an instantiation can't be type-erased across a TU that doesn't
see the template, so an open type set only works where the template is visible. The
planned shape registers one concrete (value-parametrized) declaration per type in a
list — separable declaration and instantiation so long type lists live apart from the
body — each then driven by `nx::invoke_tests` like any other. Value parametrization
(this document) is the part that composes across the registry today.

## Gotchas

- **Inert by default.** An `INVOCABLE_TEST` never runs on its own; it needs a driver.
- **No trailing `;`** after an `INVOCABLE_TEST` body — it is a function definition (the
  params are a macro argument, which is why they are parenthesized).
- **Coarse matching.** Use a unique key type; a bare `int`/`bool` key co-invokes.
- **By value or `const&` only** — mutable lvalue-ref parameters are a compile error.
- **Setup-once requires no sibling `SECTION`s** around a top-level `invoke_tests`.
- A driver that only invokes tests needs no `CHECK` of its own (it's exempt from the
  no-assertion rule); but if a call matches **nothing**, the driver has neither checks
  nor children and is flagged.
