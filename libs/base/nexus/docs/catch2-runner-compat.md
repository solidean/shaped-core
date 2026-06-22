# Catch2 Runner Compatibility

Nexus implements a subset of the Catch2 v3 CLI protocol so that Catch2-aware IDE extensions — in particular [C++ TestMate](https://github.com/matepek/vscode-catch2-test-adapter) — can discover, run, and display nexus tests without any special configuration.

## How detection works

TestMate identifies a test binary as Catch2-compatible by running it with `--help` and scanning the output for a version string. Nexus prints:

```
Compatible with Catch2 v3.11.0 in some args
```

([run.cc:38](../src/nexus/run.cc))

That line is the hook. Everything else flows from it.

## CLI flags

Arg parsing lives in `test_schedule_config::create_from_args` ([schedule.cc:7](../src/nexus/tests/schedule.cc)).

| Flag | Nexus behavior |
|---|---|
| `--list-tests` | Sets internal `has_list_tests` flag; triggers discovery mode when combined with `--reporter` |
| `--reporter <type>` | Sets internal `has_xml_reporter` flag; the reporter value is consumed and otherwise ignored |
| `--verbosity <level>` | Consumed and ignored (accepted so invocations don't error) |
| `--durations <yes/no>` | Consumed and ignored |
| `-v` | Enables verbose schedule printing |
| `-c <name>` | Adds a section filter |
| Any other arg | Treated as a test name filter (see below) |

### Test name filters

Unrecognized positional args are treated as test name filters. Filters are matched by substring against test names. Multiple filters can be passed as a single comma-separated argument — nexus splits on `,` before storing them, matching the Catch2 convention.

When any filter contains no `*` wildcard, nexus also enables `run_disabled_tests` so that explicitly targeted disabled tests can be run.

In Catch2 XML compat modes (discovery or results), filter strings are also unescaped: `\[` → `[`. Catch2 uses `\[` to escape square brackets in tag-filter syntax; since nexus doesn't have tags, it strips the backslash so the literal `[` can still match test names.

([schedule.cc:70–120](../src/nexus/tests/schedule.cc))

## Modes

Two compat modes are activated based on which flags are present ([schedule.cc:87–91](../src/nexus/tests/schedule.cc)):

| Flags present | Mode |
|---|---|
| `--list-tests` + `--reporter` | **Discovery** — print XML test list, exit 0 |
| `--reporter` only (no `--list-tests`) | **Results** — run tests, print XML results |

## Discovery mode

TestMate calls the binary with `--list-tests --reporter xml [filters]` to enumerate tests. Nexus responds with a `<MatchingTests>` document listing every scheduled test (after applying any name filters):

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

`<ClassName/>` and `<Tags></Tags>` are always empty — nexus has no class or tag concepts but the elements are required for schema conformance. All text content is XML-escaped.

([run.cc:44–63](../src/nexus/run.cc))

## Results mode

TestMate calls the binary with `--reporter xml [--durations yes] [filters]` to run a subset of tests and get structured results. Nexus runs the selected tests and then prints a `<TestRun>` document:

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

- **Minimum-1-failure rule.** If a section is marked as failing (`is_considered_failing`) but has zero recorded failed checks — e.g. it threw an exception before any `CHECK` ran — nexus reports `failures="1"` instead of `failures="0"`. Without this, TestMate would display the section as green despite the test being red. ([run.cc:108](../src/nexus/run.cc))

- **Error cap.** At most 50 `<Expression>` elements are emitted per test case to keep the XML output bounded. ([run.cc:145](../src/nexus/run.cc))

Exit code is 0 if all tests passed, 1 if any failed.

([run.cc:119–156, 205–209](../src/nexus/run.cc))

## Known gaps

The following Catch2 XML features are not yet implemented (tracked in `run.cc` TODO comment):

- `<StdOut>` / `<StdErr>` capture inside test cases
- `INFO` / `CAPTURE` contextual messages
- Partial test-case runs (section re-entry / `partNumber`)
- Benchmark result reporting
- Run metadata (run name, RNG seed)
- `expectedFailures` — currently always `0`
- `<Tags>` population from test declarations
- Per-section stderr progress lines for live IDE feedback
