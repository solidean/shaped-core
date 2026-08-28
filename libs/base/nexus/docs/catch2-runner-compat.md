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

It reaches the output as the `help` text of the CLI declaration in [schedule.cc](../src/nexus/tests/schedule.cc).
[test-cli-test.cc](../tests/test-cli-test.cc) pins that it is still there, because this is the hook every mode below hangs off.

## CLI flags

Arg parsing lives in `test_schedule_config::create_from_args` ([schedule.cc](../src/nexus/tests/schedule.cc)), declared with [nx::args](args.md).
`--help` is generated from that same declaration, so the two cannot describe different programs.

Two properties of the declaration are load-bearing for this compatibility layer:

* **Unknown arguments are captured, not rejected**, via `allow_unknown`.
  That is what keeps "any other arg is a filter" true, and what lets a test whose name begins with a dash be selected at all.
* **`--jobs` is bound to the int, with `nx::arg::at_least(0)` on it**, so a bad count now FAILS the run.
  It used to warn and carry on at the default, which meant a mistyped width ran the whole suite green at a concurrency nobody asked for.

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
| `--pgo-benchmarks` | Selects the *pgo_benchmark* bucket (perf benchmarks; see [perf-results.md](../../../../docs/guides/perf-results.md)). Not a Catch2 flag |
| `--pgo-json <file>` | Writes recorded `nx::pgo` metrics to `<file>` (additive). Not a Catch2 flag |
| `--record` | Buckets every test's `cc::rec` events and writes a recording for each failing one, whatever the tests' own configs say (see [recording.md](recording.md)). Not a Catch2 flag |
| `--no-recording` | Leaves `cc::rec` down for the whole run: no per-test buckets, no console logger, no failure dumps (see [recording.md](recording.md)). Not a Catch2 flag |
| `--list-tests-json <file>` | Writes a JSON listing of every registered test (with eligibility under the other args) to `<file>` — `-` means stdout — then exits 0 without running anything. Used by `dev.py test` to pre-select binaries. Not a Catch2 flag |
| `--match-files` | Reads the filters as globs over the tests' source files, skipping name matching. Not a Catch2 flag |
| `--match-names` | Reads the filters as test names only, without the file fallback. Not a Catch2 flag |
| `--test-args <line>` | A command line for the selected test, tokenized by the [nx::args rules](args.md#the-tokenizer). Not a Catch2 flag |
| `--` | The same, for everything after it, already split. Not a Catch2 flag |
| `-h` / `--help` | Prints the generated help and exits 0. The parse is what recognizes them, so one past a bare `--`, or inside `--test-args`, belongs to the test instead |
| Any other arg | Treated as a filter (see below) |

### Filters

Unrecognized positional args are treated as filters, matched by substring against test names.
Multiple filters may be passed as one comma-separated argument — nexus splits on `,` before storing them, matching the Catch2 convention.

A filter that equals a test name **exactly** can reach past the eligibility gates, which is what runs a disabled test, or one from another bucket.
Crossing a bucket that way additionally requires that no bucket flag was given; the disabled gate has no such condition.
A substring filter never opens either gate.

### The file fallback

When no filter matches any test or alias **name**, the filters are re-read as globs over the tests' source files — the paths `cc::source_location` recorded at registration, so absolute ones.
`function_ref-test.cc` then selects every test declared in that file, and `libs/base/clean-core/tests/memory/*` selects a directory's worth.

The rules are `cc::glob_matches`': `?` is one character, `*` a run not crossing `/`, and `**` one that does.
On top of that a filter that is not already anchored also matches as a path *suffix*, which is what lets a bare filename or a repo-relative fragment reach an absolute path.
A filter naming a directory stands for its subtree.
Separators are normalized (`\` and `/` are the same, and git-bash's `/c/x` is `C:\x`), and matching folds case.

A file match selects exactly like a substring filter does — it is **never** an exact name, so it opens neither the disabled nor the bucket gate.
`dev.py test hash-benchmark.cc` therefore reports that the file's benchmarks are in the `pgo_benchmark` bucket rather than running them; `--pgo-benchmarks hash-benchmark.cc` runs them.

The fallback is decided once per binary, by `resolve_filter_mode`, before anything queries a filter — so the schedule and the `--list-tests-json` listing always agree.
`--match-files` and `--match-names` pin the reading instead of letting it fall back.

### Buckets and disabled tests

Every test lives in exactly one **bucket** — `normal` (the default), `manual`, `pgo_benchmark` or `example` — set via `nx::config::manual` / `pgo_benchmark` / `example`.
A sweep selects exactly one bucket: the default selects `normal`, `--manual` selects `manual`, `--pgo-benchmarks` selects `pgo_benchmark`, `--examples` selects `example`.
The bucket set is intentionally extensible.

`disabled` (`enabled = false`) is **orthogonal** to buckets: it can apply to any bucket, and excludes a test from sweeps until it is named exactly or `run_disabled_tests` is set.
`run_disabled_tests` has no CLI flag today — it is a `test_schedule_config` field, so only an embedder sets it.

- **manual** — tests that open windows, or are otherwise incompatible with unattended runs.
  Swept only under `--manual`.
- **pgo_benchmark** — perf benchmarks that report metrics via `nx::pgo`, covered by [perf-results.md](../../../../docs/guides/perf-results.md).
  Swept only under `--pgo-benchmarks`.
- **example** — `EXAMPLE` declarations demonstrating an API in practice, covered by [examples.md](../../../../docs/guides/examples.md).
  Swept only under `--examples`, and normally reached one at a time by exact name, which is what `dev.py example` sends.

Only the `normal` bucket is subject to the "no CHECK/REQUIRE is a failure" rule, so a benchmark that only prints, or an example that only demonstrates, still passes.

Two ways to reach a test outside the swept bucket, and no others:

- **Its bucket's flag**, which sweeps that bucket by substring — `--manual bench` runs every manual test whose name contains `bench`, while a bare `bench` matches none of them.
- **Its exact name** — `dev.py test "bench-sort - cc::sort vs std::sort"` runs that one benchmark on its own.
  This works only without a bucket flag, since a flag pins the sweep to its bucket.

A substring filter **never** crosses a bucket.
`dev.py test "async"` runs the normal-bucket `async` tests and leaves the manual ones and the benchmarks alone.

The disabled gate works the same way — exact name, or the bulk `run_disabled_tests` — and is checked independently of the bucket gate.
Both live in `test_schedule_config::is_eligible`, shared by `would_run` and the alias expansion, so an alias reaches a manual driver only when the *alias* is named exactly.

In the Catch2 XML compat modes, filter strings are also unescaped: `\[` becomes `[`.
Only the opening bracket — Catch2 escapes `[` because it opens tag syntax, and a `\]` is left as typed.
Catch2 uses `\[` to escape square brackets in tag-filter syntax; nexus has no tags, so it strips the backslash and the literal `[` can still match a test name.

`*` has no special meaning **to name matching**: names are plain substrings, so `bench*` matches a literal `*`.
It is a wildcard only once the filter is read as a file glob.

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
