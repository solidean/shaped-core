# Test runtime

`dev.py check` runs the whole suite once per preset, and there are several presets.
So a second a test wastes is paid several times on every commit, by everyone.
This page is what keeps a test's runtime proportional to what it proves.

## No deliberate waiting

A test runs as fast as the machine lets it, and never waits a fixed amount of wall-clock time.

- **Wait for the condition, not for a duration.**
  A test that needs a sampler to have fired, a worker to have drained or a file to have landed waits until that is true, under a cap only a broken build reaches.
  A fixed window is either too short on a loaded machine, which makes it flaky, or too long everywhere else, which makes it slow — usually both, on different runs.
- **A timeout is tested by injecting a clock, never by waiting it out.**
  Timeout behaviour is worth testing, and the code under test takes a time source the test advances instantly.
  A test that sleeps two seconds to see a two-second timeout fire is wrong, however well it passes.
- **A negative check sizes its window from a measured positive run.**
  "Nothing arrives" has no condition to wait on, so the test first times the same thing succeeding, then waits a multiple of that.
  [record-sampling-test.cc](../../clean-core/tests/record/record-sampling-test.cc) is the model: a stopped sampler is watched for `busy_for_secs(2 * sampled_secs)`, twice what the running one needed.
- **A green test hits no timeout.** A deadline in a test is a guard that turns a hang into a message, so reaching it is a failure by definition, and a passing run never pays for it.

## Thorough runs: `nx::is_thorough()`

Some tests get better the longer they run — a fuzz search over more seeds, a sweep over bigger inputs, a statistical check at a tighter margin.
Their full strength is worth having, and not worth paying on every commit.

`nx::is_thorough()` is how one test serves both.
The **thorough** run is the test as it is meant to be, and a default run narrows it:

```cpp
auto test = nx::fuzz::test::create();
test->add_op(...);   // declare everything at full strength
test->add_op(...);

if (!nx::is_thorough())
    test->cap_seed_count(24);   // or cap_max_executions(n), smaller inputs, fewer frames

SECTION("fuzz") { CHECK(test->execute_fuzz_test()); }
```

```bash
uv run dev.py test "sg vulkan backend" --thorough   # the full-strength version, forwarded as --thorough
```

- **A flag, not a bucket.** The test runs in both modes and asserts the same things; only how much ground it covers changes.
  The `manual` bucket is not where a thorough test goes: it holds one-off tools and the rare machine-dependent test, and no sweep runs it.
  A test that only makes sense at full strength is a normal test marked `nx::config::thorough_only` — see below.
  The WARP entry drivers are a different case again.
  They skip by default only when a hardware adapter covers the same assertions, and run by default on a host without one.
  So they branch on `nx::is_thorough()` in the body rather than being `thorough_only`.
- **Write the thorough version first, then narrow it.**
  Size the thorough run to what is worth waiting for — tens of seconds for an expensive fuzz — and cut the default run down to well under a second.
  Narrowing afterwards keeps the full-strength parameters in one place, readable as the intent.
- **Narrow ground, never checks.** Fewer seeds, lower caps, a smaller image — not a skipped validation layer or a looser tolerance.
  A default run that passes must still mean the property holds on what it covered.
- `nx::is_thorough()` reads the running test's own run, so a nested `nx::execute_tests` answers for itself, and it is false outside a test.

The fuzz narrowing helpers are `cap_seed_count(n)` — each seed is a whole program, so this scales runtime linearly — and `cap_max_executions(n)`, which lowers every operation's at-most.
[fuzz-testing](fuzz-testing.md) has the engine they narrow.

## Tests with no narrow version: `thorough_only`

Some tests have nothing worth running in a default run — a soak, a sweep whose only point is its size.
Declare those `thorough_only`, and a default run skips them rather than paying for a version that proves nothing:

```cpp
TEST("mesh - decimation holds on every model in the corpus", thorough_only)
{
    for (auto const& f : list_dir("data/mesh-corpus"))
        CHECK(decimate(load(f)).is_manifold());
}
```

- **Skipped, not left out.** The test is still selected and scheduled, and its body is replaced by a `SKIP("runs only under --thorough")` that passes.
  A filter naming it in a default run therefore reports it green without running it; add `--thorough` to run it.
- **Prefer narrowing when a narrow version exists.** A test that proves something on a smaller input keeps running on every commit, which is worth more than a skip.
- **It holds at dispatch too.** An `INVOCABLE_TEST` marked `thorough_only` is skipped by `nx::invoke_tests` in a default run, whatever its driver carries.

**Not yet: a skip is reported as a pass.**
nexus has no skipped state, so `SKIP` counts as a passing check and the JUnit writer always writes `skipped="0"`.
A default run therefore shows every `thorough_only` test as passed, and the reason it did not run appears in no report.
The fix is a real skipped outcome carried through the result, the console summary and JUnit.

## Finding the slow test

Guessing is slower than looking:

```bash
uv run dev.py test --profile .tmp/dev-profile/test.json --profile-type chrome-tracing --profile-lanes per-type
```

Every test is a slice of the trace, with the thread it ran on — [Profiling a run](../../../../docs/guides/building-and-testing.md#profiling-a-run) says how to read it.
Check the exclusion first: an `exclusive()` test holds its phase's lock alone, so nothing else in the phase runs while it does.
Every second one of those takes is a second of the binary's wall clock — [parallel-execution](parallel-execution.md) has the locks.
The `serial` column of `dev.py test` adds that up per binary: what ran alone, plus the largest tag group.
A `main_thread` test runs beside the pool, but main-thread bodies share one thread, so a slow one delays the next.
