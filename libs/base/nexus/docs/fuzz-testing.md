# Fuzz testing (`nx::fuzz`)

`nx::fuzz` is an **API-sequence fuzzer** built into nexus. Instead of feeding
random *bytes* into one function, you declare a small vocabulary of typed
**operations**, seed **values**, and **invariants**, and the engine composes them
into random but type-correct *programs* (sequences of calls). When a program
fails, the engine automatically **shrinks** it to a minimal reproducer and prints
**copy-pasteable C++ regression code**.

This is the right tool for stateful APIs — containers, parsers, state machines,
allocators — where bugs hide in *sequences* of operations, not in a single call.

```cpp
#include <nexus/fuzz/fuzz.hh>

TEST("add1 never reaches 7")
{
    auto test = nx::fuzz::test::create();
    test->add_value("3", 3);
    test->add_op("add1", [](int a) { return a + 1; });
    test->add_invariant("is-not-7", [](int i) { return i != 7; });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }
}
```

You build the setup once in an ordinary `TEST`, then drive it from `SECTION`s
(see *the regression workflow* below). The example above fails
(3 → 4 → 5 → 6 → 7) and prints a 6-step reproducer.

## Model

- **Value** — a seed datum, registered with `add_value(name, v)`. Modeled as a
  nullary operation returning a copy, so `v`'s type must be copyable.
- **Operation** — any callable, registered with `add_op(name, fn)`. Its argument
  types are deduced; each argument is drawn from the values produced so far.
  Non-const reference parameters (`T&`) are **mutating** — the engine writes the
  mutation back. A `cc::random&` parameter is special (see *Randomized operations*).
- **Invariant** — a univariate, non-mutating check registered with
  `add_invariant(name, fn)`. It runs automatically after **any** operation that
  produces or mutates a value of its argument type. It may either return `bool`
  (which must be true) or return `void` and use `CHECK(...)` internally.
- **Precondition** — `op->when(pred)` guards when an operation may run. `pred`
  may be nullary (an external gate), single-argument (must hold for every input
  of that type), or exact-arity (the full argument tuple).
- **Builder** — `execute_at_least(n)` / `execute_at_most(n)` / `execute_once()`
  shape how often an operation is scheduled (operations default to at least 50;
  values to at least 1).

State is **SSA-like**: every value lives in a slot, operations communicate only
through slots, and the reachable set grows as operations produce new values.

## Determinism and replay

The engine draws all randomness from `cc::random` (a PCG32 generator in
clean-core), seeded per run. The same seed, the same setup, and the same build
reproduce a run exactly — and because PCG32 uses fixed constants, runs reproduce
across platforms and compilers too. Each step also records a full `cc::random`
state; an operation taking a `cc::random&` is handed a generator rebuilt from it
via `cc::random::from_state(...)`, which is what makes randomized operations
replayable (state/`from_state` is the blessed roundtrip).

## Randomized operations

An operation can pull entropy directly:

```cpp
test.add_op("gen", [](cc::random& r) { return r.uniform(0, 10); });
test.add_op("use", [](int a, int b) { /* ... */ });
```

The `cc::random&` argument is not drawn from a slot; it is synthesized from the
step's recorded state. Emitted regression code reproduces it via
`cc::random::from_state`:

```cpp
auto i0 = test->eval_op("gen", cc::random::from_state(3737ull));
auto i1 = test->eval_op("gen", cc::random::from_state(5313ull));
test->eval_op("use", i1, i0);
```

## Failure detection

Inside an operation, all four of these are detected and stop the run:

- a thrown exception,
- a failed `CHECK` or `REQUIRE`,
- a failed `CC_ASSERT` (rerouted into the engine instead of aborting),
- an invariant returning `false`.

Crucially, the thousands of failing executions the engine probes during
generation and minimization do **not** pollute the host test: they are captured
(via nexus's `scoped_check_capture`) and never recorded against it. The only
result the host test sees is the single `CHECK(test->execute_fuzz_test())`.

> Note: like `CHECK_ASSERTS`, `CC_ASSERT`-based detection only works on
> assert-enabled presets (debug / relwithdebinfo). On `release-*` presets
> assertions are compiled out.

## Minimization and the regression workflow

On a finding, the engine shrinks the failing program to a local minimum that
still fails: it tree-shakes operations that cannot influence the failing step,
then tries single-step removals in randomized order, re-validating every
candidate by replay. It then prints the reproducer as a ready-made `SECTION`
referring to the handle name you pass (`"test"` by default):

```text
[fuzz] found a failing run (seed 1, 12 operations): invariant violated
[fuzz] minimal reproducer (6 operations) - paste as a SECTION next to your fuzz SECTION:

SECTION("regression")
{
    auto i0 = test->eval_op("3");
    auto i1 = test->eval_op("add1", i0);
    auto i2 = test->eval_op("add1", i1);
    auto i3 = test->eval_op("add1", i2);
    auto i4 = test->eval_op("add1", i3);
    CHECK(!test->eval_op_bool("is-not-7", i4));
}
```

Because nexus re-runs the test body once per `SECTION` path, the setup written
in the outer `TEST` is rebuilt fresh for each section. So the workflow is: build
the setup once, fuzz in one `SECTION`, and paste each finding as a sibling
`SECTION` to pin it as a regression — no shared helper needed.

```cpp
TEST("add1 never reaches 7")
{
    auto test = nx::fuzz::test::create();
    test->add_value("3", 3);
    test->add_op("add1", [](int a) { return a + 1; });
    test->add_invariant("is-not-7", [](int i) { return i != 7; });

    SECTION("fuzz")
    {
        CHECK(test->execute_fuzz_test());
    }

    SECTION("regression")
    {
        auto i0 = test->eval_op("3");
        auto i1 = test->eval_op("add1", i0);
        auto i2 = test->eval_op("add1", i1);
        auto i3 = test->eval_op("add1", i2);
        auto i4 = test->eval_op("add1", i3);
        CHECK(!test->eval_op_bool("is-not-7", i4));
    }
}
```

You can also pin specific behaviors directly with `eval_op_to<T>` /
`eval_op_bool` in their own `SECTION`s, independent of any finding.

## Setup errors

If some argument type can never be constructed (no value or operation produces
it), `execute_fuzz_test` reports a setup error rather than a finding — add a
value or a producing operation for that type.

## Out of scope (for now)

Corpus persistence, parallel fuzzing, coverage-guided generation, and
multi-operation removal during minimization are not implemented.

## See also

- [cheat-sheet.md](../cheat-sheet.md) — the `nx::fuzz` quick reference.
- [clean-core `cc::random`](../../clean-core/cheat-sheet.md) — the deterministic RNG.
