# nexus

Lightweight C++23 test framework, Catch2 v3 CLI–compatible so IDE test integration works out of the box.
Namespace `nx`. Depends on clean-core, plus babel-serializer privately for the JSON sidecars.
Nothing links nexus but test binaries, so the harness is a leaf and may depend on any library.
Every `<lib>-test` binary in shaped-core is built on it.

```cpp
#include <nexus/test.hh>

TEST("string - to_upper")
{
    CHECK(to_upper("hi") == "HI");
}
```

Headers are included by their full path from `src/`, e.g. `#include <nexus/tests/check.hh>`.
`<nexus/test.hh>` is the only header a test file normally needs — it pulls in the check, section, config and invocable machinery.

**Never run the `nexus-test` binary directly** — go through `uv run dev.py test`, which configures, builds, discovers and records results.
[building-and-testing](../../../docs/guides/building-and-testing.md) is the full workflow.

## Beyond TEST and CHECK

Four capabilities are easy to miss from the macros alone:

| capability | entry point | doc |
|---|---|---|
| parametrized / data-driven tests | `INVOCABLE_TEST` + `nx::invoke_tests` | [docs/invocable-tests.md](docs/invocable-tests.md) |
| API-sequence fuzzing with shrinking and emitted reproducers | `nx::fuzz::test` | [docs/fuzz-testing.md](docs/fuzz-testing.md) |
| recorded perf metrics, consumed by `dev.py pgo` | `GUIDE_BENCHMARK` + `nx::guide` | [docs/guides/perf-results.md](../../../docs/guides/perf-results.md) |
| runnable demonstrations of an API in practice | `EXAMPLE` + `dev.py example` | [docs/guides/examples.md](../../../docs/guides/examples.md) |
| a command line, with help, validation and completion | `nx::args` | [docs/args.md](docs/args.md) |
| hardware performance counters around a workload | `nx::bench::measure_hw_counters` | [docs/guides/profiling.md](../../../docs/guides/profiling.md) |
| asserting on what a test logged, recorded or measured | `nx::test_recording()` | [docs/recording.md](docs/recording.md) |

## File organization

Source lives in `src/nexus/`, grouped by responsibility:

| Folder | What's in it |
|---|---|
| *(root)* | the macro surface (`test.hh`), the runner entry point (`run.hh`), and guide-benchmark reporting (`guide.hh`) |
| `tests/` | the runner itself — registry, config, scheduling, execution, checks, sections, aliases, and the invocable machinery |
| `tests/export/` | output formats: Catch2 XML, JUnit XML, the JSON test listing `dev.py test` pre-selects binaries with, and the perf sidecar |
| `bench/` | hardware performance counters, with a per-OS backend under `impl/` |
| `fuzz/` | the API-sequence fuzzing engine: operations, the state machine, shrinking, and regression emission |
| `args/` | `nx::args`: the grammar, subcommands, validation, help rendering, completion, and the ambient accessors |
| `web/` | the Emscripten browser-runner ABI, wired up by `cmake/NexusWebRunner.cmake`'s `sc_add_nexus_web_runner` |

`bench/impl/` and `args/impl/` are private implementation details — don't include from them directly.

`examples/` holds runnable demonstrations of `nx::args`, one binary per file.

## More

- [cheat-sheet.md](cheat-sheet.md) — the API at a glance: every macro, the check vocabulary, and the gotchas.
- [docs/_index.md](docs/_index.md) — nexus' documentation hub.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows.
