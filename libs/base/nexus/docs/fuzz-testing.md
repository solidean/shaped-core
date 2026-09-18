# Fuzz testing (`nx::fuzz`)

`nx::fuzz` is an **API-sequence fuzzer** built into nexus.
Instead of feeding random *bytes* into one function, you declare a small vocabulary of typed **operations**, seed **values** and **invariants**.
The engine composes them into random but type-correct *programs* — sequences of calls.
When a program fails, the engine **shrinks** it to a minimal reproducer and prints **copy-pasteable C++ regression code**.

This is the right tool for stateful APIs — containers, parsers, state machines, allocators — where bugs hide in *sequences* of operations rather than in a single call.

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

You build the setup once in an ordinary `TEST`, then drive it from `SECTION`s; *the regression workflow* below is why.
The example above fails (3 → 4 → 5 → 6 → 7) and prints a 6-step reproducer.

## Model

- **Value** — a seed datum, registered with `add_value(name, v)`.
  Modeled as a nullary operation returning a copy, so `v`'s type must be copyable.
- **Operation** — any callable, registered with `add_op(name, fn)`; its argument types are deduced, and each argument is drawn from the values produced so far.
  Non-const reference parameters (`T&`) are **mutating**: the engine writes the mutation back.
  A `cc::random&` parameter is special — see *Randomized operations*.
  An operation returning `cc::shared_async<T>` is awaited, and produces `T` — see *Async operations*.
- **Invariant** — a univariate, non-mutating check registered with `add_invariant(name, fn)`.
  It runs automatically after **any** operation that produces or mutates a value of its argument type.
  It may return `bool`, which must be true, or return `void` and use `CHECK(...)` internally.
- **Precondition** — `op->when(pred)` guards when an operation may run.
  `pred` may be nullary (an external gate), single-argument (must hold for every input of that type), or exact-arity (the full argument tuple).
- **Builder** — `execute_at_least(n)` / `execute_at_most(n)` / `execute_once()` shape how often an operation is scheduled.
  Operations default to at least 50 runs, values to at least 1.

State is **SSA-like**: every value lives in a slot, operations communicate only through slots, and the reachable set grows as operations produce new values.

## Determinism and replay

The engine draws all randomness from `cc::random`, clean-core's PCG32 generator, seeded per run.
The same seed, setup and build reproduce a run exactly, and because PCG32 uses fixed constants, runs reproduce across platforms and compilers too.
Each step also records a full `cc::random` state, and an operation taking a `cc::random&` is handed a generator rebuilt from it via `cc::random::from_state`.
That roundtrip — `state` out, `from_state` in — is what makes a randomized operation replayable.

## Randomized operations

An operation can pull entropy directly:

```cpp
test.add_op("gen", [](cc::random& r) { return r.uniform(0, 10); });
test.add_op("use", [](int a, int b) { /* ... */ });
```

The `cc::random&` argument is not drawn from a slot; it is synthesized from the step's recorded state.
Emitted regression code reproduces it via `cc::random::from_state`:

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

The thousands of failing executions the engine probes during generation and minimization do **not** pollute the host test.
They are captured via nexus' `scoped_check_capture` and never recorded against it, so the only result the host test sees is the single `CHECK(test->execute_fuzz_test())`.
An async operation leaves its thread, so its failures are diverted per test instead — see *Async operations*.

> Like `CHECK_ASSERTS`, `CC_ASSERT`-based detection only works on assert-enabled presets (debug / relwithdebinfo).
> On a `release-*` preset assertions are compiled out.

## Minimization and the regression workflow

On a finding, the engine shrinks the failing program to a local minimum that still fails.
It tree-shakes operations that cannot influence the failing step, then tries single-step removals in randomized order, re-validating every candidate by replay.
It then prints the reproducer as a ready-made `SECTION`, referring to the handle name you pass (`"test"` by default):

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

Because nexus re-runs the test body once per `SECTION` path, the setup written in the outer `TEST` is rebuilt fresh for each section.
So the workflow is: build the setup once, fuzz in one `SECTION`, and paste each finding as a sibling `SECTION` to pin it as a regression — no shared helper needed.

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

You can also pin specific behaviors directly with `eval_op_to<T>` / `eval_op_bool` in their own `SECTION`s, independent of any finding.

## Fuzzing over external, shared state

The engine drives your operations against whatever state they close over.
When that state is an external, **shared** resource — a GPU context, a database handle, a global allocator — two properties of the engine become load-bearing:

- **It runs thousands of programs against the same shared object.** Generation tries many random sequences, and minimization *replays* candidate after candidate.
  They all hit the one resource your operations captured.
- **It discards partial states constantly.** Each replay builds a fresh SSA state, runs some operations, then destroys it.
  That includes any values your operations produced that are still "open".

So every value threaded through the fuzz state must **clean up after itself on destruction** (RAII).
A value that leaks a resource when its state is discarded corrupts the *shared* object — a command list never submitted, a transaction left open, a lock not released.
The corruption then surfaces **later, on an unrelated operation**, often as a minimal reproducer that does **not** reproduce in isolation.

> Worked example: a fuzz over an `sg::context` shrank to the reproducer `mk_trace; advance_epoch`, which passes when run alone.
> `mk_trace` submits its list, so the advance sees no open lists.
> It only "failed" because *earlier* discarded replays had left command lists open on the shared context, and the leaked open-list count made a later `advance_epoch` assert.
> Two lessons.
> Make fuzz-state values self-cleaning, dropping an open resource in the destructor *and* in move-assignment, so a discarded replay never leaks onto the shared object.
> And when a minimal reproducer does not reproduce standalone, suspect shared-state pollution from the churn around it rather than the printed steps.

## Narrowing for a default run

`execute_fuzz_test` searches 256 seeds, and each seed is a whole program, so a fuzz over something slow — a GPU context, a database — gets expensive fast.
Declare it at full strength, then narrow it for the default run after the last operation:

```cpp
if (!nx::is_thorough())
    test->cap_seed_count(24);       // programs searched; scales runtime linearly
// test->cap_max_executions(n);     // lowers every operation's at-most (and an at-least above it)
```

`dev.py test --thorough` runs the full search.
[test-runtime](test-runtime.md) has the rules for what a narrowing may cut.

## Async operations

An op whose callable returns `cc::shared_async<T>` is an **async op**: the engine awaits it, and `T` is what reaches its slot.
`cc::shared_async<cc::unit>` is an async op with no result.
Registering one needs `<nexus/fuzz/async.hh>`, the one fuzz header that carries the coroutine machinery.

```cpp
#include <nexus/async-test.hh>
#include <nexus/fuzz/async.hh>

ASYNC_TEST("sg - transfers survive random op sequences")
{
    auto test = nx::fuzz::test::create();
    test->add_op("mk_trace", [&] { return make_trace(ctx); })->execute_once(); // sync
    test->add_op("advance epoch + wait",
                 [&](trace& t) -> cc::shared_async<cc::unit>                   // async, no result
                 {
                     t.ensure_submitted_cmd();
                     ctx->advance_epoch();
                     (void)co_await ctx->idle_completion();
                 });

    SECTION("fuzz") { CHECK(co_await test->execute_fuzz_test_async()); }
}
```

**Execution stays serial.**
Each async op is awaited before the next step starts, and no two ops ever overlap.
That is what makes a `T&` parameter safe across the op's suspends: nothing else touches the state while it is parked.
Sync and async ops mix freely, and a sync op still runs inline.

**Two entry points.**
`execute_fuzz_test_async()` runs any mix of ops and returns a `cc::shared_async<bool>` for an async test to await.
`execute_fuzz_test()` on a test holding even one async op is a setup error naming the ops, since it cannot await; it is a setup error rather than an assert so it holds on a `release-*` preset too.
`execute_fuzzer_async(seed)`, `fuzz_run::replay_async` and `fuzz_run::minimize_async` are the awaited forms of the single-program calls.

**An async op's failures are the step's, from any thread.**
While an op is awaited, every check reported for the running test is diverted to that step, from whichever thread reports it.
That covers the op's own coroutine and work it awaits on a pool worker — and anything else reporting for the test meanwhile.
That last part matters when an earlier step started work it never awaited: its failure lands on whichever step is running, which is the shared-state rule above in another form.
A diverted `CC_ASSERT` in a node being polled fails that node rather than the process, and a node that fails, by an escaped exception or `cc::async_fail`, is a failing step carrying the message.

**The engine takes the value.**
It moves `T` out of the op's node, so a handle the op kept for itself reads a moved-from value afterwards.
To keep a pending handle in a slot — a "start download" op and a separate "finish download" op — wrap it in a type of your own and return that.
Returning `cc::shared_async<T>` itself would be awaited, not stored.

**Which kinds may be async.**
Ops and invariants: an invariant returns `cc::shared_async<bool>`, or `cc::shared_async<cc::unit>` and checks inside.
Preconditions are evaluated while the next step is chosen and seed values are constants, so an async one does not compile.
Nor does a `cc::async_scheduled<T>` return, which would start before the engine awaits it; return the cold `cc::shared_async<T>`.

**Where ops run.**
By default an async op's body goes wherever cc places a coroutine nobody homed, which is the compute pool.
`test->set_inherit_home(true)` runs the whole awaited fuzz where `execute_fuzz_test_async` is called from, when that caller runs in a home — every sync op and every async op's body.
That is what a thread-bound device needs, and a caller in no home is unaffected, so one invocable body serves homed and unhomed sweeps alike.
A coroutine an op awaits for itself still follows cc's usual placement.

**The reproducer awaits exactly the async steps.**
A sync step stays `test->eval_op(...)`, and an async one becomes `co_await test->eval_op_async(...)`, so the printed section belongs in an async test.

```cpp
SECTION("regression")
{
    auto t0 = test->eval_op("mk_trace");
    test->eval_op("upload", cc::random::from_state(3737ull), t0);
    co_await test->eval_op_async("advance epoch + wait", t0);
    co_await test->eval_op_async("download + check", cc::random::from_state(5313ull), t0); // <-- fails here
}
```

Each spelling refuses the other kind of op, failing the test with the call to use instead.
`eval_op_to_async<T>` and `eval_op_bool_async` mirror their synchronous forms.

## Setup errors

If some argument type can never be constructed — no value or operation produces it — `execute_fuzz_test` reports a setup error rather than a finding.
Add a value or a producing operation for that type.
The synchronous entry points also report one for a test holding an async op.

## Out of scope (for now)

Corpus persistence, parallel fuzzing, coverage-guided generation, and multi-operation removal during minimization are not implemented.
Nor is running async ops concurrently: the awaited engine interleaves nothing.
A bug that needs two ops in flight at once is reachable only through an op that starts work and a later one that finishes it.

## See also

- [cheat-sheet.md](../cheat-sheet.md) — the `nx::fuzz` quick reference.
- [clean-core `cc::random`](../../clean-core/cheat-sheet.md) — the deterministic RNG.
