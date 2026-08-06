# Catch2 Runner Compatibility

> This doc covers the CLI and discovery layer.
> For the day-to-day test-writing API (`TEST` / `CHECK` / `SECTION`) see the [nexus cheat-sheet](../cheat-sheet.md).

Nexus implements a subset of the Catch2 v3 CLI protocol, so Catch2-aware IDE extensions discover, run and display nexus tests with no special configuration.
[C++ TestMate](https://github.com/matepek/vscode-catch2-test-adapter) is the extension this is calibrated against.

## How detection works

TestMate identifies a test binary as Catch2-compatible by running it with `--help` and scanning the output for a version string.
Nexus prints:

```
Compatible with Catch2 v3.11.0 in some args
```

([run.cc, `print_help`](../src/nexus/run.cc)) — the hook every mode below hangs off.

## CLI flags

Arg parsing lives in `test_schedule_config::create_from_args` ([schedule.cc](../src/nexus/tests/schedule.cc)).

| Flag | Nexus behavior |
|---|---|
| `--list-tests` | Sets internal `has_list_tests` flag; triggers discovery mode when combined with `--reporter` |
| `--reporter <type>` | Sets internal `has_xml_reporter` flag; the reporter value is consumed and otherwise ignored |
| `--verbosity <level>` | Consumed and ignored (accepted so invocations don't error) |
| `--durations <yes/no>` | Consumed and ignored |
| `--junit-xml <file>` | Writes a JUnit XML report to `<file>` (additive — see [JUnit XML output](#junit-xml-output)) |
| `-v` | Enables verbose schedule printing |
| `-c <name>` | Adds a section filter |
| `--manual` | Selects the *manual* bucket for sweeps (see below). Not a Catch2 flag |
| `--guide-benchmarks` | Selects the *guide_benchmark* bucket (perf benchmarks; see [perf-results.md](../../../../docs/guides/perf-results.md)). Not a Catch2 flag |
| `--perf-json <file>` | Writes recorded `nx::guide` metrics to `<file>` (additive). Not a Catch2 flag |
| `--list-tests-json <file>` | Writes a JSON listing of every registered test (with eligibility under the other args) to `<file>` — `-` means stdout — then exits 0 without running anything. Used by `dev.py test` to pre-select binaries. Not a Catch2 flag |
| Any other arg | Treated as a test name filter (see below) |

### Test name filters

Unrecognized positional args are treated as test name filters, matched by substring against test names.
Multiple filters may be passed as one comma-separated argument — nexus splits on `,` before storing them, matching the Catch2 convention.

A filter that equals a test name **exactly** can reach past the eligibility gates, which is what runs a disabled test, or one from another bucket.
Crossing a bucket that way additionally requires that no bucket flag was given; the disabled gate has no such condition.
A substring filter never opens either gate.

### Buckets and disabled tests

Every test lives in exactly one **bucket** — `normal` (the default), `manual`, or `guide_benchmark` — set via `nx::config::manual` / `nx::config::guide_benchmark`.
A sweep selects exactly one bucket: the default selects `normal`, `--manual` selects `manual`, `--guide-benchmarks` selects `guide_benchmark`.
The bucket set is intentionally extensible.

`disabled` (`enabled = false`) is **orthogonal** to buckets: it can apply to any bucket, and excludes a test from sweeps until it is named exactly or `run_disabled_tests` is set.
`run_disabled_tests` has no CLI flag today — it is a `test_schedule_config` field, so only an embedder sets it.

- **manual** — tests that open windows, or are otherwise incompatible with unattended runs.
  Swept only under `--manual`.
- **guide_benchmark** — perf benchmarks that report metrics via `nx::guide`, covered by [perf-results.md](../../../../docs/guides/perf-results.md).
  Swept only under `--guide-benchmarks`.

Tests in the `manual` and `guide_benchmark` buckets are exempt from the "no CHECK/REQUIRE is a failure" rule, so a benchmark that only prints (or only records metrics) still passes.

Two ways to reach a test outside the swept bucket, and no others:

- **Its bucket's flag**, which sweeps that bucket by substring — `--manual bench` runs every manual test whose name contains `bench`, while a bare `bench` matches none of them.
- **Its exact name** — `dev.py test "bench-async-grain (sweep)"` runs that manual test on its own.
  This works only without a bucket flag, since a flag pins the sweep to its bucket.

A substring filter **never** crosses a bucket.
`dev.py test "async"` runs the normal-bucket `async` tests and leaves the manual ones and the benchmarks alone.

The disabled gate works the same way — exact name, or the bulk `run_disabled_tests` — and is checked independently of the bucket gate.
Both live in `test_schedule_config::is_eligible`, shared by `would_run` and the alias expansion, so an alias reaches a manual driver only when the *alias* is named exactly.

In the Catch2 XML compat modes, filter strings are also unescaped: `\[` becomes `[`.
Catch2 uses `\[` to escape square brackets in tag-filter syntax; nexus has no tags, so it strips the backslash and the literal `[` can still match a test name.

`*` has no special meaning: filters are plain substrings, so `bench*` matches a literal `*`.

(`test_schedule_config`'s filter and eligibility predicates, in [schedule.cc](../src/nexus/tests/schedule.cc))

## Modes

Two compat modes are activated based on which flags are present, in `create_from_args` ([schedule.cc](../src/nexus/tests/schedule.cc)):

| Flags present | Mode |
|---|---|
| `--list-tests` + `--reporter` | **Discovery** — print XML test list, exit 0 |
| `--reporter` only (no `--list-tests`) | **Results** — run tests, print XML results |

## Discovery mode

TestMate calls the binary with `--list-tests --reporter xml [filters]` to enumerate tests.
Nexus responds with a `<MatchingTests>` document listing every scheduled test, after applying any name filters:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<MatchingTests>
  <TestCase>
    <Name>my test name</Name>
    <ClassName/>
    <Tags></Tags>
    <SourceInfo>
      <File>src/tests/foo.cc</File>
      <Line>42</Line>
    </SourceInfo>
  </TestCase>
  ...
</MatchingTests>
```

`<ClassName/>` and `<Tags></Tags>` are always empty — nexus has no class or tag concept, but the elements are required for schema conformance.
All text content is XML-escaped.

([export/catch2.cc, `write_catch2_discovery_xml`](../src/nexus/tests/export/catch2.cc))

## Results mode

TestMate calls the binary with `--reporter xml [--durations yes] [filters]` to run a subset of tests and get structured results.
Nexus runs the selected tests, then prints a `<TestRun>` document:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<TestRun>
  <TestCase name="my test name" filename="src/tests/foo.cc" line="42">

    <Section name="edge case" filename="src/tests/foo.cc" line="55">
      <Expression success="false" filename="src/tests/foo.cc" line="58">
        <Original>a == b</Original>
        <Expanded>1 == 2</Expanded>
      </Expression>
      <OverallResults successes="3" failures="1" expectedFailures="0"
                      durationInSeconds="0.0012"/>
    </Section>

    <OverallResult success="false" durationInSeconds="0.0015"/>
  </TestCase>
  ...
</TestRun>
```

Sections are emitted recursively — a section may contain `<Expression>` elements (for failed checks) followed by nested `<Section>` elements, each with their own `<OverallResults>`.

Two implementation details worth knowing:

- **Minimum-1-failure rule.** A section marked failing with zero recorded failed checks — `is_considered_failing`, but it threw before any `CHECK` ran — reports `failures="1"`.
  Otherwise TestMate displays the section green while the test is red.

- **Error cap.** At most 50 `<Expression>` elements are emitted per test case to keep the XML output bounded.

Exit code is 0 if all tests passed, 1 if any failed.

([export/catch2.cc, `write_catch2_results_xml`](../src/nexus/tests/export/catch2.cc))

## JUnit XML output

Independently of the Catch2 compat modes, `--junit-xml <file>` writes a [JUnit](https://github.com/testmoapp/junitxml)-style report to `<file>`.
This is **additive**: the tests run normally and the usual console summary still prints, so the file is a pure side-output.

The report models each nexus test as one `<testcase>` under a single `<testsuite>`, named after the binary.
A failing test carries a `<failure>` element whose body lists the failed expressions and their source locations:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="nexus-test" tests="2" failures="1" errors="0" skipped="0" assertions="5" time="0.0031">
  <testsuite name="nexus-test" tests="2" failures="1" errors="0" skipped="0" assertions="5" time="0.0031">
    <testcase classname="nexus-test" name="my passing test" time="0.0009"/>
    <testcase classname="nexus-test" name="my failing test" time="0.0022">
      <failure message="src/tests/foo.cc:58">a == b =&gt; 1 == 2 at src/tests/foo.cc:58
</failure>
    </testcase>
  </testsuite>
</testsuites>
```

The aggregate `<testsuite>` / `<testsuites>` attributes (`tests`, `failures`, `time`, …) match the schema `dev.py` parses.
`parse_junit` / `merge_junit` in [tools/dev/lib/core/logs.py](../../../../tools/dev/lib/core/logs.py) are the readers.
`assertions` — total checks evaluated — is outside the base JUnit schema but understood by common tooling, and `dev.py` reads it for the check counts it prints.
`dev.py test` passes `--junit-xml` to every nexus binary and prefers this report over its own synthesized single-case sidecar, so `test_diag` and CI see one result per test.

([export/junit.cc, `write_junit_xml`](../src/nexus/tests/export/junit.cc))

## Known gaps

The following Catch2 XML features are not yet implemented (tracked in the `export/catch2.cc` TODO comment):

- `<StdOut>` / `<StdErr>` capture inside test cases
- `INFO` / `CAPTURE` contextual messages
- Partial test-case runs (section re-entry / `partNumber`)
- Benchmark result reporting
- Run metadata (run name, RNG seed)
- `expectedFailures` — currently always `0`
- `<Tags>` population from test declarations
- Per-section stderr progress lines for live IDE feedback
