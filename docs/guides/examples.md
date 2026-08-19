# Examples

An example is a runnable demonstration of an API **in practice**.
It lives beside the tests, is compiled by every build, and is run by nobody automatically — `uv run dev.py example <match>` runs exactly one.

## Why they exist at all, next to a test suite

Tests are biased toward what is *testable*.
Their job is to pin invariants, so they gravitate to the small, assertable, edge-case-shaped corner of an API — which is precisely the corner that does not show how the API feels to use.
A test can be exhaustive about `cc::map::erase` and still leave a reader with no idea how one reaches for a map.

An example inverts that.
It has no obligation to assert anything, is free to be a straight line of ordinary calls, and is judged by whether reading it teaches you the library.
**The cheat sheet plus the examples is the intended way to get a feel for a library**; the test suite is the way to trust it.

## Running one

```bash
uv run dev.py example                       # list every example, grouped by binary
uv run dev.py example clean-core/vector     # build, resolve, run exactly one
uv run dev.py example vector                # a substring is fine, as long as it selects one
```

Resolution is a **name** lookup, not a target lookup.
An example name says nothing about the binary carrying it, so every `*-example` target is built and probed with nexus' `--list-tests-json - --examples`.
Listings are cached per artifact under `build/<preset>/example-listings.json` and revalidated by mtime and size, so only a rebuilt binary is re-probed.

An exact name wins outright; otherwise a case-insensitive substring must select exactly one.
Ambiguity is an error listing the candidates, and a miss is an error with suggestions — because running "the first one that matched" would silently run the wrong example.

`--target` narrows which binaries are probed.
The match never does.

**An example runs in its own source directory**, not in whatever directory `dev.py` was invoked from.
So a relative path in an example — the document it keeps, the assets it loads — resolves next to the example.
The state a run leaves behind is then ignored by that directory's own `.gitignore`, rather than by a rule in the repo root.
A single-file example shares its library's `examples/` folder with its siblings; one with its own directory gets that directory.

Anything `dev.py` does not recognize is forwarded to the example, so `dev.py example x -- --my-flag` reaches its `main`.
There is deliberately **no `--all`**: executing the whole corpus would open every window it has, and nobody wants that.

### `--background`, for a run that should not take over the screen

`dev.py example <match> --background` asks a graphical example to appear without stealing the foreground.
Its windows are shown without being activated and sunk to the bottom of the z-order, so the run is watchable while you keep working.
Clicking one brings it forward and leaves it there.

The mechanism is an environment variable, `SC_REQUEST_BACKGROUND`.
`sr::window_system_description::background` defaults from it — see [sr::background_request_env_var](../../libs/graphics/shaped-rendering/src/shaped-rendering/window.hh).
An env var rather than an API because `dev.py` launches a program it did not write, and neither nexus nor the example itself is in the conversation.
It is an escape hatch, named in one place so a real API can replace it later without hunting for `getenv` calls.
The shared reader is `cc::is_environment_flag_set` ([clean-core/platform/environment.hh](../../libs/base/clean-core/src/clean-core/platform/environment.hh)).
Preview rendering will use the same convention.

An example that constructs its own `window_system_description` can override it in either direction; one that does not gets the environment's answer for free.

## Declaring one

```cpp
#include <nexus/test.hh>

EXAMPLE("clean-core/vector")
{
    auto v = cc::vector<int>::create_filled(5, 1);
    cc::println("{} elements", v.size());
}
```

`EXAMPLE` is a nexus declaration in its own selection bucket, alongside `TEST` and `GUIDE_BENCHMARK` ([nexus/test.hh](../../libs/base/nexus/src/nexus/test.hh)).
What follows from the bucket:

* **A sweep never selects it.** Not `dev.py test`, not `--manual`, not a bulk "run disabled too" request.
  Only `--examples` or its exact name.
* **No CHECK is required.** A `CHECK` that fails still fails, so assert where asserting is part of the demonstration — but an example with none is a passing example.
* **`main_thread` is baked in**, so the body runs on the thread `nx::run` was entered on, one example at a time — which is what a window or a device context needs.
* **The run installs an ambient async scheduler**, so an example may use `cc::async` without standing up a pool of its own.
  `EXAMPLE("x", no_scheduler)` is how an example that wants to install its own opts out of that.

**The name is a slash path**, and it is load-bearing.
It is the CLI argument, the gallery entry and — once preview rendering lands — the screenshot slug, so it is written as its own identifier rather than as a sentence.
By convention the first segment is the library the example is about.

## Where they live

Per library, parallel to `tests/`:

```
libs/base/clean-core/examples/
    CMakeLists.txt
    vector.cc          -> clean-core-vector-example, EXAMPLE("clean-core/vector")
    string.cc          -> clean-core-string-example
```

Cross-library — an example that demonstrates several libraries together, and so belongs to none of them:

```
examples/<category>/<example>/
    CMakeLists.txt
    ...
```

Targets are always named `*-example`.
That suffix is what tells `dev.py` an example binary from a test binary, exactly as `*-test` does for tests.

## Declaring the binaries

Two helpers, from [nexus/cmake/Examples.cmake](../../libs/base/nexus/cmake/Examples.cmake).
Both are no-ops when `SC_BUILD_EXAMPLES` is off, so a call site never needs an `if()` around it.

```cmake
# One binary per file, each with its own generated main. The common case.
sc_add_single_file_examples(clean-core
    FILES vector.cc string.cc result.cc map.cc
    LINK clean-core
    PCH CC_STD NEXUS
)

# One binary from several sources, for an example with real internal structure.
sc_add_example(vdoc-cube-editor-example
    SOURCES cube_app.cc cube_renderer.cc example-viewer.cc example-editor.cc
    MAIN main.cc        # optional: without it, a main.cc calling nx::run is generated
    LINK versioned-document shaped-rendering
)
```

The generated `main.cc` is the same three lines every hand-written `tests/main.cc` holds.
It is written with `file(CONFIGURE)` into the build directory, so it is copy-if-different and invisible to clang-format and shaped-linter.

**One `EXAMPLE` per file is the convention, not a rule.**
A binary with several is exactly right when related examples want to share setup headers and show different aspects over them — a viewer and a full editor over one scene, say.
Such a binary may also carry ordinary `TEST`s, for the machinery it grew along the way.
`dev.py test` runs them: an example binary joins the sweep exactly when it reports tests of its own, and is skipped when it has none.
Its `EXAMPLE`s are in another bucket and never run there.

## What binds an example, and what does not

Examples are first-class source.
clang-format, shaped-linter, the clang-tidy gates and the cross-reference check all apply exactly as they do to `libs/` and `tests/`.
A library's `.shaped-lint.yml` scopes its blessings with `files: examples/**`, mirroring the `files: tests/**` entries beside it.

Two exemptions:

* **Coverage.** Example sources are excluded from the `dev.py coverage` report — they never run in a sweep, so they would sit at 0% and dilute every library's number.
* **Execution.** CI builds every example and runs none.

Building without running is a real gap: an example can compile against an API it no longer uses correctly.
That is accepted for now.
A dry-run mode, which would exercise an example's setup without opening its window, is the eventual answer.

## Platform-gated examples

An example that needs a platform-only backend gates its own target, the way a library does:

```cmake
if(WIN32 AND SC_HAS_DXC_COMPILER AND SC_HAS_SDL3)
    sc_add_example(...)
endif()
```

Every CI leg builds all targets, wasm and cross-compile legs included, so an ungated graphical example breaks builds that could never have run it.

## Related

* [building-and-testing.md](building-and-testing.md) — the `dev.py` reference this command sits in.
* [cheat-sheets.md](cheat-sheets.md) — the other half of getting a feel for a library.
* [nexus cheat-sheet](../../libs/base/nexus/cheat-sheet.md) — `EXAMPLE` beside the rest of the macro surface.
