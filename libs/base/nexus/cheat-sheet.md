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
