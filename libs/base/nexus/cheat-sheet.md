# nexus cheat sheet

Lightweight C++23 test framework, Catch2 v3 CLI–compatible so IDE test integration works out of the box.
Namespace `nx`, depends on clean-core (plus babel-data privately, for the JSON sidecars).

You almost never call `nx::` directly — you write `TEST` / `CHECK` / `SECTION` macros and run them through `dev.py`.
Headers are included by full path: `#include <nexus/...>`.
[readme.md](readme.md) is the library overview and [docs/_index.md](docs/_index.md) the documentation hub.
Format conventions for this sheet live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

**Recording domain:** `nexus`.
Every `CC_LOG_*` and `CC_RECORD_*` site in this library is attributed to it; see [logging](../../base/clean-core/docs/logging.md).

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
{ /* prints, no CHECK */ }               //     exact name or `--manual` (may have zero CHECKs, like a benchmark)
TEST("rng", nx::config::seed(42)) { }    //   nx::config::seed(n)   — fixed RNG seed
// Multiple configs compose: TEST("x", nx::config::disabled, nx::config::seed(7)) { }

// Concurrency configs — see docs/parallel-execution.md. A run is a graph of cc::async nodes, capped by --jobs.
TEST("gpu thing", exclusive("gpu")) { }  //   exclusive(tag)  — never runs beside another holder of `tag`
TEST("mutates env", exclusive()) { }     //   exclusive()     — runs alone, beside nothing at all; a synchronous one is
                                         //     routed into the no-scheduler group, so it costs no barrier and no node
TEST("own scheduler", no_scheduler) { }  //   no_scheduler    — NO ambient scheduler at all: none bound, none
                                         //     installed. For a test standing up its own, and REQUIRED to nest
                                         //     nx::execute_tests. Touching an async without one asserts.
TEST("order matters", singlethreaded) { }//   singlethreaded  — ambient cc::singlethreaded_scheduler: every graph
                                         //     runs inline on the body's thread, in order
TEST("opens a window", main_thread) { }  //   main_thread     — body runs on the process MAIN thread (SDL wants that);
                                         //     a flag, not a mode, so it composes; own_pool / ASYNC_TEST assert
TEST("pool shape", own_pool(2)) { }      //   own_pool(n)     — a private n-worker pool, shared per count
// Exclusion is an ORDERING edge, not a lock: holders run in schedule order, which is reproducible by design.

#include <nexus/async-test.hh>           // separate header: TEST pays nothing for the async templates
ASYNC_TEST("cache - resolves a miss")    // a TEST whose body may co_await; nexus awaits the body
{                                        //   a CHECK at any depth below it still lands on THIS test
    auto const e = co_await cache.acquire_async("shader.hlsl");   // a FAILED await short-circuits + fails the test
    CHECK(e.is_compiled());              //   no SECTION inside an async body; a graph error fails the test by name
}                                        // no co_ keyword? then `return` a COLD cc::shared_async<cc::unit> instead

// Buckets: every test is in one bucket — normal (default), manual, pgo_benchmark, benchmark, or example. A sweep selects
// one bucket; `disabled` is orthogonal and can apply to any. Exact-naming a test runs it regardless of bucket; a
// substring filter never leaves the swept bucket (`test "bench"` won't drag in manual tests — use --manual).
```

## Examples (`EXAMPLE`)

A runnable demonstration of an API **in practice**, in the `example` bucket, run one at a time by `dev.py example`.
Every build compiles them and no sweep runs one; [docs/guides/examples.md](../../../docs/guides/examples.md) is the concept and the CMake side.

```cpp
EXAMPLE("clean-core/vector")             // swept only via `--examples`, or run by its exact name
{                                        //   no CHECK required — a failing one still fails
    auto v = cc::vector<int>::create_filled(5, 1);
    cc::println("{} elements", v.size());
}
// The name is a slash path: it is the CLI argument and the gallery entry, so it is an identifier, not a sentence.
// `main_thread` is baked in, so the body runs on the thread nx::run was entered on, one example at a time.
// The run still installs an ambient async scheduler; EXAMPLE("x", no_scheduler) is how one installs its own.
```

## Benchmarks (`BENCHMARK` + `nx::bench`)

Measures whether one implementation beats another, with statistics rather than a stopwatch.
Full guide: [docs/guides/benchmarking.md](../../../docs/guides/benchmarking.md).

```cpp
BENCHMARK("sort - cc::sort vs std::sort")  // a test in the `benchmark` bucket; always exclusive_global + main_thread,
{                                          //   swept only via `--benchmarks`, or run by exact name
    auto data = make_input();

    nx::bench::run("cc::sort", [&] { cc::sort(data); });   // first loop declared is the table's baseline
    nx::bench::run("std::sort", [&] { std::sort(...); });  // every later one is compared against it
}
```

```cpp
// The three body shapes — the harness picks by signature, no opt-in needed.
nx::bench::run("name", [] { work(); });                       // void()            — the harness owns the loop
nx::bench::run("name", [](nx::bench::iteration& it) { … });   // void(iteration&)  — items, pause/resume, quantities
nx::bench::run("name", [](isize count) { … });                // void(isize)       — the body owns the inner loop

// Inside a void(iteration&) body:
it.items(n);                                   // n elements this iteration -> the items/s column
it.pause(); setup(); it.resume();              // keep setup out of the measurement
it.record("bytes", cc::rec::unit_bytes, n);    // an extra column; the unit says how it aggregates
it.index(); it.is_warmup();                    // where in the run this iteration is

// Guards — without one, the optimizer deletes work nothing observes.
nx::bench::sink(value);                        // the write side: value is now observed
auto v = nx::bench::keep(expr);                // the read side: expr's input cannot be folded away
nx::bench::compiler_barrier();                 // zero instructions, orders the optimizer only
nx::bench::evict_data_caches();                // real milliseconds; a cold-cache measurement wants it

// Config — a designated-initializer literal at the call site, never filled field by field.
nx::bench::run("name", {.min_time_secs = 2.0, .target_relative_error = 0.05}, body);
nx::bench::run_config::standard();             // the defaults
nx::bench::run_config::single_shot();          // one iteration per sample, no batching, no calibration
// .batch = false           -> one sample IS one iteration, which is what makes p95/p99 meaningful
// .no_baseline = true      -> a sweep: no comparison column at all
// .is_baseline = true      -> this loop is the baseline instead of the first declared
```

```cpp
// A result, if you want the numbers rather than the table.
auto const r = nx::bench::run("name", body);
r.time.median; r.time.p95; r.time.ci95_low;    // seconds; p95/p99 only meaningful when .batch = false
r.items_per_second; r.converged;
r.find_warning(nx::bench::warning_kind::body_deleted);   // nullptr if it did not fire
```

Run them: `uv run dev.py benchmark "<match>"` (no arg lists them all).
Defaults to a **release** preset — the only `dev.py` subcommand that does, because `relwithdebinfo-*` compiles `CC_ASSERT` in.
`--json <file>` writes a sidecar carrying every sample; `--rec <file>` writes a `.ccrec` of the whole run.

## PGO benchmarks (PGO metrics)

A **tracking signal**, not a benchmarking framework: a few stable points per subject, consumed by `dev.py pgo`.
To compare two implementations, write a `BENCHMARK` instead — the section above.
The two live happily in one file: a `BENCHMARK` sweeping the space, and a `PGO_BENCHMARK` pinning two stable points out of it.

```cpp
#include <nexus/pgo.hh>

PGO_BENCHMARK("hash - throughput")     // a test in the pgo_benchmark bucket (implies no-CHECK is fine);
{                                        //   swept only via `--pgo-benchmarks`, or run by exact name
    double gbps = measure(...);
    nx::pgo::report("xxh3@8B", bytes_per_s, nx::bench::unit_bytes_per_second);  // the unit carries the orientation
    nx::pgo::report_elements_per_sec("keys", n_per_s);  // unit_items_per_second, higher is better
    nx::pgo::report_time_for("op", seconds);            // unit_seconds,          lower  is better
}
// Recorded metrics print as a table and, with `--pgo-json <file>`, write a sidecar consumed by `dev.py pgo`.
```

## Hardware counters (`nx::bench`)

```cpp
#include <nexus/bench/bench.hh>

auto const m = nx::bench::measure_hw_counters([&] { work(); });   // ONE invocation; loop inside the body yourself
if (auto const ins = m.value_of(nx::bench::hw_counter::instructions_retired); ins.has_value())
    cc::println("retired {} instructions", ins.value());          // nullopt = not requested, or unreadable this run

nx::bench::available_hw_counters();      // what THIS machine can measure right now (the runtime source of truth)
nx::bench::print_hw_counters();          // the same, printed;  `uv run dev.py profiling counters` is the CLI
```

- **Best-effort, never fails as a whole** — elapsed time always comes back, and reference cycles wherever a cheap counter exists (x86).
- An unreadable counter is `nullopt`, so gate on `has_value()` rather than on a machine assumption.
- **Only a few PMU counters fit at once**, and how many cannot be queried.
  `hw_measure_config::measure_all` re-runs the body over subsets, narrowing until they fit, so the budget never leaves a counter unmeasured — only unavailability does.
  The body must be deterministic for that.
- [docs/guides/profiling.md](../../../docs/guides/profiling.md) owns this surface: the counter list, the per-platform rules, and the Windows non-admin setup.
  It also has a worked cache-traversal example.

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
auto v = REQUIRED_VALUE(expr);           // REQUIRE a cc::result/cc::optional holds a value, then evaluate to it
                                         // returns BY VALUE (moves out of an rvalue), so move-only payloads work
FAIL();  FAIL("msg");                    // unconditional hard fail
SKIP();  SKIP("not implemented yet");    // skip the test (not counted as a failure)
```

## Checks off the test's own thread

```cpp
#include <nexus/tests/thread_scope.hh>

\ A check inside a cc::async frame is attributed automatically — the graph carries which test it belongs to.
CHECK(x);                                  \ on a pool worker: counted against the test that scheduled the node

\ A thread you start yourself carries nothing, so wrap its work:
std::thread t(nx::attributed_to_current_test([&] { CHECK(worker_saw_it); }));
auto captured = nx::capture_current_test(); \ for a thread already running: capture here, install there
nx::test_thread_scope const s(captured);    \ ... on that thread
```

- **An unattributable check FAILS THE RUN**, passing or not — it is printed where it happens and reported as `N check(s) ran outside any test context`.
  A check that proved nothing must not look like a pass.
- **Off the test's own thread, `REQUIRE`/`SKIP` abort only where a throw can land.**
  Inside an async frame it terminates that node (cc::async turns it into the node's error); on a bare thread it degrades to a recorded failure.
- **`SECTION` is the test thread's alone** — the body is replayed once per section path, which only that thread does.
  Opening one elsewhere is a recorded failure.
- **Leaving async work running past the end of a test fails that test**, by name: it would otherwise report into whatever runs next.

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
    SECTION("iter {}", i)                // cc::format-style args allowed in the name
    { /* ... */ }
}
```

## Invocable tests (parametrized / data-driven / generator)

```cpp
// An INVOCABLE_TEST takes arguments and is INERT (a sweep never runs it). params are a parenthesized macro
// arg; the body follows with NO trailing ';'. Trailing config items compose as with TEST.
INVOCABLE_TEST("mesh - decimate", (mesh_case const& c), nx::config::seed(3))
{ CHECK(decimate(c).is_manifold()); }

// A driver produces data and invokes it. nx::invoke_tests(name, args...) runs EVERY INVOCABLE_TEST whose
// decayed argument signature matches args..., each as an addressable child under section `name`. Leave the
// template arg to deduce by default.
TEST("sg backend - vulkan")
{
    auto ctx = sg::make_context(sg::backend::vulkan);   // expensive setup happens ONCE
    nx::invoke_tests("vulkan", ctx);                     // every sg test, reusing the one context
}
```

- **Addressable iteration, not sections**: the driver body runs once, and each match is driven to completion internally.
  Don't mix a top-level `invoke_tests` with sibling `SECTION`s — they would replay the driver.
- **Matching is coarse** — decayed signature, `T` == `T const&` — so every `INVOCABLE_TEST(int)` matches any `invoke_tests` of an `int`.
  Use a unique key type: `sg::context_handle`, a `case` struct, a tag.
- **Params must be by value or `const&`** — a mutable lvalue ref is a compile error, since args are shared inputs.
- **Address one instance**: `dev.py test "<driver>" -c <invoke-name> -c <test-name> [-c <section>...]`, one `-c` per path segment.
  `dev.py test` forwards everything after the name to the binary.
- **Run an instance by name**: an `NX_TEST_SETUP(nx::setup& s)` block defines *aliases*.
  `s.define_alias(name, {alias_fragment{driver, section_path}, ...})` binds a name to driver+scope, so `dev.py test "<test-name>"` runs it, one scoped run per fragment.
  `s.invocables_with<Args...>()` / `find_test(name)` help build them.
  Aliases never double-run: a full sweep ignores them, and a fragment whose driver is already name-selected is dropped.
- **Orphan check**: in a full unfiltered normal run, an enabled `INVOCABLE_TEST` that no driver invoked fails the run.
  One an alias can reach is exempt, so a deliberately `disabled` driver parks its invocables (runnable by name) rather than orphaning them.
- Args are boxed by (decayed) value, so prefer cheap-to-copy / handle types.
- Type-parametrized (templated) tests are not implemented; [docs/invocable-tests.md](docs/invocable-tests.md) has the full mechanism and the planned shape.

## Running tests

```bash
# NEVER run the *-test binary directly — always go through the repo driver:
uv run dev.py test "group - what"        # auto-build + run matching test(s); substring match, comma-OR (`\,` is a literal comma)
uv run dev.py test vector-test.cc        # matching no name, the filter is retried as a glob over the tests' source files
uv run dev.py test "libs/base/**/tests/memory/*"   # …so a path or a directory selects everything declared under it
uv run dev.py test                       # build + run the whole suite
# Diagnose a failure with the repo_tools MCP `test_diag` (dev.py prints the exact selector).
```

```cpp
#include <nexus/run.hh>                  // test main is just: int main(int c, char** v){ return nx::run(c, v); }
// The runner's own CLI is declared with nx::args, so `<binary> --help` lists every flag below.
// Catch2-compatible (for IDEs/tooling, not daily use): --list-tests, --reporter xml,
// --junit-xml <file>, -c <section>. See docs/catch2-runner-compat.md.
// --test-args "<line>", or everything after a bare --, is a command line for the SELECTED TEST itself,
//   readable from its body through nx::test_args(). Replaces whatever nx::config::args declared.
// Bucket / perf CLI: --manual (sweep manual bucket), --pgo-benchmarks (sweep pgo-benchmark bucket),
// --benchmarks (sweep benchmark bucket), --examples (sweep example bucket).
// --pgo-json <file> (recorded-metric sidecar), --benchmark-json <file> (full results + every sample),
//   --benchmark-rec <file> (a .ccrec of the whole run), --benchmark-verbose, --benchmark-pin.
// --jobs N / -j N / -jN : cap on tests running at once; 0 means hardware concurrency, and IS THE DEFAULT.
//   -j1 runs them one at a time in schedule order rather than on a pool of one — the reproducible-debugging
//   mode: a -jN failure that survives -j1 is a test bug, one that vanishes is a concurrency bug.
//   See docs/parallel-execution.md.
// --match-files / --match-names : pin how the filters are read, instead of names-then-files. A file match is
//   still just a filter, so the disabled and bucket gates hold — only an exact test NAME opens those.
// --list-tests-json <file|-> : print a JSON listing of every test (name, file:line, bucket, enabled, seed,
//   filter_matches, eligible) plus an "aliases" array, the resolved config (filters, filter_mode, selected_bucket,
//   allow_cross_bucket_naming, run_disabled_tests) and eligible_count / eligible_alias_count, under the rest of
//   the args, then exit 0. Used by `dev.py test` to pre-select binaries. "-" means stdout.
```

## Command-line arguments (`nx::args`)

```cpp
#include <nexus/args/args.hh>

auto jobs = 4;                                   // the variable's initializer IS the default
auto files = cc::vector<cc::string>();

auto args = nx::args({.name = "mytool", .description = "does the thing", .version = "0.1"});
args.arg({"j", "jobs"}, jobs, {.desc = "how many at once"});   // names braced; 1 char = short
args.arg({"v", "verbose"}, verbose, "print more");             // the description-only overload
args.positional("FILES", files, {.desc = "inputs", .min_count = 1});

if (auto const r = args.parse(argc, argv); r.should_exit())    // --help is neither success nor failure
    return r.exit_code();                                      // 0 for help/version, 1 for a usage error
```

```cpp
// Binding:   T& (scalar, last wins) · cc::vector<T>& (accumulates) · .count(...) for -vvv · .action(...)
// Options:   .desc .help .metavar .group .env .default_text .required .negatable .hidden .deprecated
//            .make_default .validate .min_count .max_count .complete
// Builder:   .group(t) (mode setter) .section(t,x) .example(c,d) .document_env(n,d) .rest(v)
//            .allow_unknown(v[, takes_value]) .stop_at_first_positional() .enable_response_files()
//            .no_auto_print() .exact_long_names() .usage_exit_code(n) .full_help_on_error()
//            .validate_setup() — NOT const: checking a default_command means declaring it
// Commands:  .command({"build","b"}, desc, declare_fn)   // declared LAZILY, only when needed
//            .delegate(names, desc, fn)  .global()  .default_command(n)
//            args.selected_command() / .command_path() / .is_command("remote add")
//   default_command gets the tokens THIS level could not account for, and sees the root's options too;
//   a name claimed by both the root and the default command is a setup error.
// Validate:  nx::arg::in_range(lo,hi) at_least at_most one_of non_empty satisfies(desc,pred), composed with &&
//            args.mutually_exclusive({...}) .at_least_one_of({...}) .requires_all(n,{...}) .require(desc,pred)
// Struct:    static void T::declare_args(nx::args_builder&, T&)   -> nx::parse_args<T>(argc, argv)
//            or specialize nx::custom::args_trait<T> to adapt a type you cannot change (checked FIRST)
// Values:    specialize nx::custom::arg_value_trait<T>: parse(sv, T&, cc::string& err), type_name(), values()
// Rendering: args.help_text({.color=false, .width=100}) / .short_help_text() / .usage_line()
//            r.diagnostic_text()  — width and colour are parameters, never sniffed, so goldens are stable
```

```cpp
#include <nexus/args/ambient.hh>          // light: no parser comes with it
nx::test_args();                          // the running test's declared args; empty if it declared none
nx::process_args();                       // the process's own — no fallback between the two, ask by name
nx::program_name();                       // basename of argv[0], no directory, no .exe / .js
nx::has_arg("--trace") / nx::get_arg<int>("budget")   // DEBUG only, over PROCESS args: guesses, never fails
```

```cpp
// A test or example can be given its own command line, which is what makes an EXAMPLE self-demonstrating:
EXAMPLE("mytool/build", nx::config::args("--jobs 8 --verbose")) { /* nx::test_args() */ }
// Override per run: dev.py example mytool/build --test-args "--jobs 1"   (REPLACES the declared line)
// A test that declared nothing sees an empty nx::test_args(), never the process's.
```

```bash
mytool --completion bash    # also zsh, fish, powershell — generated from the same declaration
                            # completes option names, command names and a type's values(); .complete overrides
mytool @response.rsp        # after .enable_response_files(); @@x is a literal @x
```

Gotchas: `-j=8` is rejected for short options (`-j8` or `-j 8`); a bool takes a value only as `--flag=x`;
`--` with no `.rest(...)` declared is an error rather than a silent drop; there is no abbreviation, only did-you-mean.
`.env` is held to `.validate` like a typed value is, and a bound `in_range` the argument's type cannot hold asserts at declaration.
[docs/args.md](docs/args.md) is the full reference and carries the grammar as a spec.

## Fuzz testing (`nx::fuzz`)

API-sequence fuzzing: declare typed *operations*, seed *values* and *invariants*, and the engine composes random type-correct programs.
It finds a failure, shrinks it, and prints copy-pasteable regression code.
[docs/fuzz-testing.md](docs/fuzz-testing.md) is the full mechanism, including the shared-state trap.

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
test->eval_op_to<int>("add1", 41);          // == 42  (also eval_op -> typed_value, eval_op_bool -> bool)
auto res = test->execute_fuzzer(seed);      // one deterministic run; res.failing_run on a finding
auto min = res.failing_run.value().minimize(rng);   // shrink; min.emit_regression("test", dialect)
```

## Recording — what did this test record?

```cpp
#include <nexus/rec.hh>
TEST("x", nx::config::recorded)      // OPT IN — bucketing is per test and paid per test
{
    auto rec = nx::test_recording(); // the running test's bucket, found via the ambient chain
    rec.sync();                      // -> recording, ONLY the new events; the sole blocking call here
    rec.all();                       // -> recording, everything synced so far — the full cc::rec query API
    rec.is_attached();               // false without nx::config::recorded, and under --no-recording
}
TEST("y", nx::config::exclusive(), nx::config::owns_recorder)  // this test drives cc::rec::initialize itself
```

- **`sync()` drains the recorder under a process-wide mutex** — a few per test is nothing, one per check in a loop is not.
- Every `nx::run` installs the console logger, so `CC_LOG_*` prints from tests and examples with zero source changes.
- A **failing** test's recording is written next to the JUnit XML at the end of the run; a passing one's is dropped.
- Opting in is what keeps it free: bucketing EVERY test costs the worst binary in this repo 31%, the handful that ask ~0%.
- `--record` buckets EVERY test and dumps each failing one — a debug flag, normally with a filter, retention unbounded.
- `--no-recording` turns the run's recorder off entirely (console logger and dumps included), and wins over `--record`.

[docs/recording.md](docs/recording.md) has the mechanism and the measurements.

## Gotchas

- **Never run the `*-test` binary directly** — `uv run dev.py test` configures, builds, discovers and records results.
- **`CHECK`/`REQUIRE` capture lhs/rhs only on failure**, so put the interesting expression *inside* the macro: `CHECK(a == b)`, not `bool ok = a == b; CHECK(ok)`.
- **`CHECK_ASSERTS` / `REQUIRE_ASSERTS` report success — and skip executing the expression — when assertions are compiled out** (`CC_ASSERT_ENABLED == 0`, the release presets).
  Run an assert-enabled preset (debug / relwithdebinfo) to actually exercise them.
- **The `_AS` exception checks match subclasses too**: a `std::runtime_error` satisfies `..._THROWS_AS(expr, std::exception)`.
- **`SKIP` does not yet interact cleanly with `SECTION`** (known limitation).
- **Data-driven / generators / matrices:** use `INVOCABLE_TEST` + `nx::invoke_tests` above, not Catch2 generators.
- **Not supported yet:** Catch2 `INFO`/`CAPTURE`, tags, and type-parametrized (templated) tests.
  Use `.context()` / `.note()` / `.dump()` for messages, and `BENCHMARK` + `nx::bench` for benchmarks.

[docs/catch2-runner-compat.md](docs/catch2-runner-compat.md) has the exact CLI subset and how IDE discovery works.
[docs/_index.md](docs/_index.md) indexes the rest, including the hardware counters and the perf-metric workflow.
