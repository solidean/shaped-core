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
The capture sweep below is the one exception, and it is allowed because it cannot do that: it runs only what a sidecar declares capturable, and every such example runs headless.

### `--background`, for a run that should not take over the screen

`dev.py example <match> --background` asks a graphical example to appear without stealing the foreground.
Its windows are shown without being activated and sunk to the bottom of the z-order, so the run is watchable while you keep working.
Clicking one brings it forward and leaves it there.

The mechanism is an environment variable, `SC_REQUEST_BACKGROUND`.
`sr::window_system_description::background` defaults from it — see [sr::background_request_env_var](../../libs/graphics/shaped-rendering/src/shaped-rendering/window.hh).
An env var rather than an API because `dev.py` launches a program it did not write, and neither nexus nor the example itself is in the conversation.
It is an escape hatch, named in one place so a real API can replace it later without hunting for `getenv` calls.
The shared reader is `cc::is_environment_flag_set` ([clean-core/platform/environment.hh](../../libs/base/clean-core/src/clean-core/platform/environment.hh)).
`SC_CAPTURE` below follows the same convention.

An example that constructs its own `window_system_description` can override it in either direction; one that does not gets the environment's answer for free.

### `--capture`, and why you should use it while writing the example

```bash
uv run dev.py example shaped-viewer/hello-cube --capture   # write an image, open no window
uv run dev.py example <example> --capture <name>          # one the sidecar declares by name
```

**This is the authoring loop for a graphical example, not just a way to refresh a picture.**
Whoever writes one cannot see what they wrote — an agent never can, and a person only sees it by stopping to look.
A run that neither crashes nor asserts routinely shows nothing worth looking at.
The camera is inside the geometry, the light is behind it, the interesting face is turned away, or the whole thing is a speck in the middle of a grey field.
None of that fails anything.

So write the example, capture it, look at the image, and fix the framing.
That loop is what turns `initial_orbit` from a guess into a decision, and it is the difference between an example that teaches the library on sight and one that merely runs.
Iterate on it the way you would iterate on a failing test.

The mechanism is environment variables, the same convention `--background` uses, declared in [capture.hh](../../libs/graphics/shaped-rendering/src/shaped-rendering/capture.hh).
`SC_CAPTURE` implies headless: no window system is brought up, no swapchain is created, and nothing is presented, so a capture runs where there is no display at all.
An example needs **no code for any of this** — `sv::interactive` reads the variables, waits for the image to settle, writes it, and ends the loop.

A capture waits for three things together, because each hides something the others cannot see.
Every traced view must have accumulated enough frames, no resource may still owe post-load work, and a trace must actually have dispatched.
That last one is what stops a routine whose shaders never compiled from writing out a black image at full frame count.
`--capture-accumulate` and `--capture-timeout` move the thresholds when a scene needs longer.

A run that spends its whole timeout writes what it has to `<out>.partial.<ext>` and **nothing to the path it was asked for**.
So the partial image is there to look at, and the capture still counts as failed.
`dev.py` reads a file at the requested path as the run having succeeded, and the exit code alone cannot tell it otherwise.
Without that split a sweep would refresh a half-converged image over the committed reference and report it as captured.

### What the default view owes the reader

An example's committed capture is its documentation, and it is the only part a reader sees before deciding whether to open the source.
So the default view is part of the example rather than a detail of how it was run.

- **It shows everything the example demonstrates.**
  Not most of it: a parameter sweep whose far half is occluded demonstrates the near half.
  Fifteen rows behind one another is the shape that fails here — depth compresses toward the horizon, so laying them out across the frame instead is usually the fix rather than moving the camera back.
- **Nothing the example is about is clipped or crushed.**
  A roughness sweep whose columns are all white is not a sweep.
- **The sidecar's `size` and `accumulate` are part of it**, since they decide whether the image is converged enough to read.
- **Look at it before you commit it.**
  The other three are only checkable that way, and this is the one that keeps failing.

Both examples in the tree were authored by someone reasoning carefully about the camera, and in both the framing defect was obvious the first time anyone looked at the picture.

### An example is capturable when it says so

Capture is opt-in, through a `.capture.json` beside the example source, keyed by example name:

```json
{
    "shaped-viewer/hello-cube": {
        "mechanism": "sv",
        "size": "1280x720",
        "format": "jpg",
        "captures": [
            {}
        ]
    }
}
```

**An example with no entry is never launched by a sweep.**
That is what makes "the sweep opens no window" true by construction rather than by detection, and it is the whole reason the sweep is allowed to exist where `--all` is not.
It also means an example rendering through something other than `sv::interactive` is never launched and left to quietly ignore the request.

`mechanism` says who answers the protocol, and what the run must produce.

- **`sv`** — a run through `sv::interactive`, which answers it for you.
  The example needs no capture code at all.
- **`custom`** — a program that answers the same environment variables itself.
  That is what an application owning its own window, swapchain and loop has to do.
  [cube_app.cc](../../examples/vdoc/cube-editor/cube_app.cc) is the worked example.
  It brings the window system up headless, renders into a texture instead of a back buffer, and writes the image on the last frame.
  Both `vdoc/cube-*` examples are captured that way, imgui panels and all.
- **`transcript`** — a text example, whose artifact is its stdout.

`sv` and `custom` differ only in who implements it: the contract is the same, so an image must appear at the path the run was given.
Declaring the mechanism is what stops a failed image capture from being filed as a successful transcript.

Everything else is optional and is a **default the flags override**, so iterating on framing never means editing the file.
`size`, `format`, `accumulate` and `timeout` may sit on the example or on a single capture, and the latter wins.
That is where a scene needing longer to converge says so, without a line in the source.

Full-line `//` comments are stripped before parsing, so the file reads like configuration despite being JSON.
JSON rather than YAML because `dev.py` declares `dependencies = []` and is worth keeping that way — see [dev-py-driver.md](../dev-py-driver.md).

### Named captures, for an example that shows more than one thing

```cpp
f.register_capture("front", [&](sv::capture_context const& c) { view.camera(front_camera); });
```

The body runs inline, where it is declared, on every frame — and only when that capture is the one being taken.
So it may simply force what it wants, and whatever the example writes after it still wins.
On an interactive run nothing is taken, so it never runs at all.

**The name must appear in both places**, and a disagreement fails rather than falling back.
A sidecar naming a capture the source does not register stops on the first frame and writes nothing; asking for one the sidecar does not declare fails before the binary is even launched.
Without that, renaming a callback would go on producing the default view under the old name's filename, and a sweep would refresh it into the repository reporting success.

**It must be idempotent after its first frame.**
Any change to what the image depends on restarts the accumulation, so a callback writing a slightly different value every frame never converges: the run spends its whole timeout and then fails.
`capture_context::first_frame` is where one-shot setup goes.

### Refreshing the reference images

```bash
uv run dev.py example --update-captures "shaped-viewer/"   # capture, then copy next to the examples
uv run dev.py example --refresh-captures "hello"           # copy only, for a subset of what was captured
```

Capturing and refreshing are two steps on purpose.
A capture writes only under `build/<preset>/captures/`, so it dirties no tracked file and anyone can run it at any time, including CI.
Refreshing is the step that touches the source tree, and it copies only the captures that succeeded.

**The committed images are documentation, not golden tests.**
Nothing compares against them and nothing gates on them; they answer "what is this example supposed to look like" for a reader, a reviewer or an agent.
A real golden test would need four things this does not have.
PNG rather than JPEG, a pinned device — two devices do not agree bit-for-bit — a perceptual metric with a tuned threshold, and somewhere to put a failure diff.
That is a deliberate non-goal rather than an omission.

**So a capture is stable run to run on one machine, and never byte-identical across machines.**
Two sweeps on the same box produce the same bytes, session state left by the first one included.
A different driver or a different adapter produces the same *picture* and different bytes, and it always will.
Re-capturing on your machine therefore dirties the three images whether or not anything changed, which is why refreshing them is a deliberate step rather than something a sweep does on its own.

**What is missing is a similarity score.**
A reviewer looking at a regenerated image has no cheap way to ask "is this roughly the same picture", so today the answer is to open both and look.
A `dev.py` subcommand comparing a capture against its committed reference, and printing one number, is the shape that would fix it; nothing of it is built.

A non-graphical example is captured through the same envelope: the same flag, the same folder, the same report.
What it produces is its transcript rather than an image.
Those are not committed yet, because a committed transcript is only worth something with a CI diff behind it.

**A capture should show a first run.**
An application that restores session state — a camera, a panel layout — has to decide what a reference image reflects.
Restoring it would bake local state into a committed file, so the same refresh would produce a different picture on every machine.
The `vdoc/cube-*` examples skip their saved camera and imgui layout when `is_capturing()`, which is what makes their images reproducible.

## Declaring one

```cpp
#include <nexus/test.hh>

EXAMPLE("clean-core/vector")
{
    auto v = cc::vector<int>::create_filled(5, 1);
    cc::println("{} elements", v.size());
}
```

`EXAMPLE` is a nexus declaration in its own selection bucket, alongside `TEST`, `BENCHMARK` and `PGO_BENCHMARK` ([nexus/test.hh](../../libs/base/nexus/src/nexus/test.hh)).
What follows from the bucket:

* **A sweep never selects it.** Not `dev.py test`, not `--manual`, not a bulk "run disabled too" request.
  Only `--examples` or its exact name.
* **No CHECK is required.** A `CHECK` that fails still fails, so assert where asserting is part of the demonstration — but an example with none is a passing example.
* **`main_thread` is baked in**, so the body runs on the thread `nx::run` was entered on, one example at a time — which is what a window or a device context needs.
* **The run installs an ambient async scheduler**, so an example may use `cc::async` without standing up a pool of its own.
  `EXAMPLE("x", no_scheduler)` is how an example that wants to install its own opts out of that.

**The name is a slash path**, and it is load-bearing.
It is the CLI argument, the gallery entry and the capture slug, so it is written as its own identifier rather than as a sentence.
As the slug it names both `build/<preset>/captures/` and the committed reference image.
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
`--capture` is the half of the answer that exists — it runs an example's whole setup with no window and no display, so CI *could* run one.
What is still missing is the check on the far side: nothing compares the result against anything, so a run that produces a wrong image still passes.

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
