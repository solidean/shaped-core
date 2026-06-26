# nexus cheat sheet

Lightweight C++23 test framework, Catch2 v3 CLI–compatible (so IDE test
integration works out of the box). Namespace `nx`. Depends on clean-core.

You almost never call `nx::` directly — you write `TEST` / `CHECK` / `SECTION`
macros and run them through `dev.py`. Headers are included by full path:
`#include <nexus/...>`. Format conventions for this sheet live in
[docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

## Writing a test

```cpp
#include <nexus/test.hh>                 // pulls in check, section, config

TEST("group - what it does")             // registered at static-init; name is matched by substring
{
    CHECK(1 + 2 == 3);
}

TEST("slow thing", nx::config::disabled) // trailing configs (variadic):
{ /* ... */ }                            //   nx::config::disabled  — skipped unless explicitly named
TEST("bench x", nx::config::manual)      //   nx::config::manual    — never swept automatically; run via an
{ /* prints, no CHECK */ }               //     exact name or `--manual` (may have zero CHECKs, e.g. benchmarks)
TEST("rng", nx::config::seed(42)) { }    //   nx::config::seed(n)   — fixed RNG seed
// Multiple configs compose: TEST("x", nx::config::disabled, nx::config::seed(7)) { }
```

## Checks

```cpp
#include <nexus/tests/check.hh>          // (already included via <nexus/test.hh>)

// soft — failure is recorded, the test keeps running:
CHECK(expr);                             // bool expr OR a comparison (lhs/rhs auto-captured on failure)
CHECK(a == b);  CHECK(x < y);            // operators: ==  !=  <  <=  >  >=
CHECK_THROWS(expr);                      // passes if expr throws anything
CHECK_THROWS_AS(expr, ExceptionType);    // passes if expr throws that type (or a subclass)
CHECK_ASSERTS(expr);                     // passes if expr trips a CC_ASSERT
SUCCEED();  SUCCEED("msg");              // unconditional soft pass

// hard — failure aborts the current test:
REQUIRE(expr);
REQUIRE_THROWS(expr);  REQUIRE_THROWS_AS(expr, ExceptionType);  REQUIRE_ASSERTS(expr);
FAIL();  FAIL("msg");                    // unconditional hard fail
SKIP();  SKIP("not implemented yet");    // skip the test (not counted as a failure)
```

## Chaining diagnostics (on the check_handle)

```cpp
CHECK(result == expected)
    .context("during parse phase")       // context line shown on failure
    .note("expected the cached value")   // descriptive note
    .dump("result", result)              // labeled value dump (uses cc::to_debug_string)
    .dump(expected);                     // unlabeled dump
```

## Sections (nested test paths)

```cpp
#include <nexus/tests/section.hh>        // (already included via <nexus/test.hh>)

TEST("string ops")
{
    SECTION("upper")                     // body runs in its own path; nest freely
    {
        CHECK(to_upper("hi") == "HI");
        SECTION("idempotent") { /* ... */ }
    }
    SECTION("iter {}", i)                // std::format-style args allowed in the name
    { /* ... */ }
}
```

## Running tests

```bash
# NEVER run the *-test binary directly — always go through the repo driver:
uv run dev.py test "group - what"        # auto-build + run matching test(s); substring match, comma-OR
uv run dev.py test                       # build + run the whole suite
# Diagnose a failure with the repo_tools MCP `test_diag` (dev.py prints the exact selector).
```

```cpp
#include <nexus/run.hh>                  // test main is just: int main(int c, char** v){ return nx::run(c, v); }
// Catch2-compatible CLI (for IDEs/tooling, not daily use): --list-tests, --reporter xml,
// --junit-xml <file>, -c <section>. See docs/catch2-runner-compat.md.
```

## Fuzz testing (`nx::fuzz`)

API-sequence fuzzing: declare typed *operations*, seed *values*, and *invariants*;
the engine composes random type-correct programs, finds a failure, shrinks it, and
prints copy-pasteable regression code. See [docs/fuzz-testing.md](docs/fuzz-testing.md).

```cpp
#include <nexus/fuzz/fuzz.hh>

TEST("add1 never reaches 7")                 // ordinary TEST: build setup once, drive it from SECTIONs
{
    auto test = nx::fuzz::test::create();
    test->add_value("3", 3);                 // seed value (copyable); produced on demand
    test->add_op("add1", [](int a){ return a + 1; });   // operation: any callable, args sourced from slots
    test->add_invariant("is-not-7", [](int i){ return i != 7; }); // checked after each int is produced/mutated

    SECTION("fuzz") { CHECK(test->execute_fuzz_test()); }          // generate + shrink + print a reproducer
    SECTION("regression") { /* paste the emitted SECTION body here */ }
}

// builder (chainable):   ->execute_at_least(n) / ->execute_at_most(n) / ->execute_once()
//                        ->when(pred)   // precondition: nullary, single-arg (per matching input), or exact-arity
// invariant flavors:     return bool (must be true)  OR  return void and CHECK(...) inside
// rng-driven ops:        add_op("gen", [](cc::random& r){ return r.uniform(0,10); });  // seeded + replayable

// eval (what emitted regression SECTIONs call; chain results to mutate/feed forward):
test->eval_op_to<int>("add1", 41);          // == 42  (also eval_op -> fuzz_value, eval_op_bool -> bool)
auto res = test->execute_fuzzer(seed);      // one deterministic run; res.failing_run on a finding
auto min = res.failing_run.value().minimize(rng);   // shrink; min.emit_regression("test", dialect)
```

## Gotchas

- **Never run the `*-test` binary directly** — `uv run dev.py test` configures,
  builds, discovers, and records results. Test binaries are named `<lib>-test`.
- **`CHECK`/`REQUIRE` capture lhs/rhs only on failure**, so put the interesting
  expression *inside* the macro (`CHECK(a == b)`, not `bool ok = a == b; CHECK(ok)`).
- **`CHECK_ASSERTS` / `REQUIRE_ASSERTS` report success — and skip executing the
  expression — when assertions are compiled out** (`CC_ASSERT_ENABLED == 0`, i.e.
  release presets). Run an assert-enabled preset (debug / relwithdebinfo) to
  actually exercise them.
- **The `_AS` exception checks match subclasses too** (a `std::runtime_error`
  satisfies `..._THROWS_AS(expr, std::exception)`).
- **`SKIP` does not yet interact cleanly with `SECTION`** (known limitation).
- **Not supported yet:** Catch2 `INFO`/`CAPTURE`, tags, generators, benchmarks.
  Use `.context()` / `.note()` / `.dump()` for messages.

See [docs/catch2-runner-compat.md](docs/catch2-runner-compat.md) for the exact
CLI subset and how IDE discovery works.
