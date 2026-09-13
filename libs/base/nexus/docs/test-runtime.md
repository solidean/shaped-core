# Test runtime

`dev.py check` runs the whole suite once per preset, and there are several presets.
So a second a test wastes is paid several times on every commit, by everyone.
This page is what keeps a test's runtime proportional to what it proves.

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
  A test that should not run at all by default belongs in the `manual` bucket instead.
- **Write the thorough version first, then narrow it.**
  Size the thorough run to what is worth waiting for — tens of seconds for an expensive fuzz — and cut the default run down to well under a second.
  Narrowing afterwards keeps the full-strength parameters in one place, readable as the intent.
- **Narrow ground, never checks.** Fewer seeds, lower caps, a smaller image — not a skipped validation layer or a looser tolerance.
  A default run that passes must still mean the property holds on what it covered.
- `nx::is_thorough()` reads the running test's own run, so a nested `nx::execute_tests` answers for itself, and it is false outside a test.

The fuzz narrowing helpers are `cap_seed_count(n)` — each seed is a whole program, so this scales runtime linearly — and `cap_max_executions(n)`, which lowers every operation's at-most.
[fuzz-testing](fuzz-testing.md) has the engine they narrow.

## Finding the slow test

Guessing is slower than looking:

```bash
uv run dev.py test --profile .tmp/dev-profile/test.json --profile-type chrome-tracing --profile-lanes per-type
```

Every test is a slice of the trace, with the thread it ran on — [Profiling a run](../../../../docs/guides/building-and-testing.md#profiling-a-run) says how to read it.
Check the thread first: `main_thread` and `exclusive()` tests run one at a time and never beside the pool.
Every second one of those takes is a second of the binary's wall clock — [parallel-execution](parallel-execution.md) has the phases.
