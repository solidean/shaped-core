# logging — `CC_LOG_*`, and where a message goes

Logging in shaped-core is one vocabulary over the recording stream rather than a system of its own.
That is what makes a message cheap enough to leave in, and what makes it reachable from a test, a crash dump and a terminal without anyone choosing between them up front.

The mechanism is [systems/recording](systems/recording.md); this is what you need to write one.

```cpp
#include <clean-core/common/log.hh>

CC_LOG_INFO("shader cache warmed");
CC_LOG_WARNING("fell back to {} after {}", name, reason);
```

## The five levels

| level | on by default | what it is for |
|---|---|---|
| `CC_LOG_TRACE` | no | per-iteration detail nobody wants until they do |
| `CC_LOG_DEBUG` | no | developer detail, for when something is being worked on |
| `CC_LOG_INFO` | yes | what happened, for someone reading afterwards |
| `CC_LOG_WARNING` | yes | something is wrong and the program continues |
| `CC_LOG_ERROR` | yes | something failed |

`trace` and `debug` are opt-in because a build that records them by default teaches everyone to turn logging off, which costs more than it ever saved.

**An error captures a stack**, which costs orders of magnitude more than the message.
That is the right default for a failure and the wrong one for a warning, so warnings do not — see [what a stack costs](systems/recording.md#what-the-recorder-costs).

## The format string is the site's name

A message with no arguments costs the stream **nothing**: the text lives in the site's descriptor, so the event is a header and no payload at all.
A message with arguments is formatted directly into the chunk, with no temporary buffer and no allocation.

The format string doubles as the site's name, which is what lets every message from one site group under one string whatever it formatted to.
That is what makes "how often does this fire" a question anyone can answer.

**So never bake varying text into the format string.**

```cpp
CC_LOG_WARNING("texture {} failed to load", name);              // one site, however many textures
CC_LOG_WARNING(cc::format("texture {} failed to load", name));  // NO — a new site per texture, and grouping is gone
```

## A library never prints

Diagnostics are logged; only a binary's own top layer prints.

`cc::print` is for a program's OUTPUT — a CLI's result, a test runner's report, an example demonstrating something.
Anything a library noticed while doing its job is a log message.
A library does not know whether anyone is watching a terminal, and must not be the one deciding that a message deserves one.

A log message reaches whatever the program installed: a console, a file, a crash dump, a test's recording.
A print reaches exactly one place and is gone.

## Domains

Which domain a message belongs to comes from `cc_rec_domain()`, resolved by **ordinary unqualified name lookup**, so a site never names one.

A library declares one in its `fwd.hh` and defines it in exactly one `.cc`:

```cpp
// fwd.hh
#include <clean-core/record/domain_fwd.hh>
namespace sg { CC_REC_DECLARE_DOMAIN(g_rec_domain); }

// one .cc
namespace sg { CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg"); }
```

Lookup from inside `sg::impl` walks out to `sg`, finds sg's, and stops.
A **nested namespace shadows the enclosing one**, which is how `babel::png` and `sg::backend::dx12` gate separately without a single call site mentioning either.

That also means attribution is a property of the namespace a file's code sits in.
Moving a function to another namespace re-attributes every site in it, silently — which is why `record-domain-test.cc` pins the resolution rules.

A domain is the one word every site gates on, and reconfiguring it is a single relaxed store:

```cpp
#include <clean-core/record/domain.hh>
cc::rec::find_domain("sg")->set_enabled(cc::rec::level::debug, true);
cc::rec::find_domain("babel.json")->set_enabled(cc::rec::category::profiling, false);
```

## Getting messages onto a terminal

**Nothing is recorded before `cc::rec::initialize()`**, and a library must never call it: how many megabytes recording may cost is the program's decision.

An application wants one line:

```cpp
cc::rec::install_default_console_listener();   // initializes if needed, then registers a console
```

Test and example binaries get theirs from `nx::run` and need no setup at all.

It returns a handle, and a program that later calls `cc::rec::shutdown()` **must unregister it first**.
Shutdown frees the pool every listener's callback reads out of, and asserts that none are left.
An application that simply runs until the process ends can ignore the handle.
Calling it again after that unregister re-registers the same listener, so bringing the recorder back up gets the console back rather than a handle to nothing.

A line looks like this:

```text
[14:23:07.882] warn  {t3} sg: a texture is bound as both a sampled and a storage view
```

Local wall-clock time by default, because it lines a log up against everything else that happened on the machine and makes a stale terminal obvious at a glance.
A test run wants `console_time::elapsed` instead, and nexus asks for it as the base its environment overrides apply over.

Source locations are **off** by default, warnings and errors included.
A `.ccrec` carries them for every event, offline and exactly, which is a better answer than a suffix on the lines that happened to be printed.

### Configuring it

Options passed explicitly are taken **verbatim**, so an application that configured its logging cannot have that overridden out from under it:

```cpp
auto listener = cc::rec::console_listener({
    .min_level = cc::rec::level::debug,
    .time = cc::rec::console_time::elapsed,
    .show_site = true,
    .color = cc::console::color_mode::never,
});
```

A **default-constructed** listener resolves its options from the environment instead, which is what lets someone debug a program they cannot rebuild:

| variable | values |
|---|---|
| `CC_LOG_LEVEL` | `trace` `debug` `info` `warning` `error` |
| `CC_LOG_TIME` | `none` `elapsed` `time` `datetime` |
| `CC_LOG_COLOR` | `auto` `always` `never` |
| `CC_LOG_THREAD` `CC_LOG_DOMAIN` `CC_LOG_SITE` | `0`/`false`/`no`/`off` for no, anything else for yes |

`NO_COLOR` and `FORCE_COLOR` are honored too, by the same `cc::console` policy every other tool in the repo uses.
An unparseable value is ignored rather than diagnosed: a misspelled log setting must never be why a program refuses to start.

`from_environment(base)` applies the same overrides over defaults of your own, which is how a program moves one setting without giving up the rest.
That is what nexus does: test binaries stamp `elapsed` because a run's question is "how far in", and `CC_LOG_LEVEL=debug uv run dev.py test "..."` still reaches them.

**Neither form may run during static initialization.**
Reading the environment and asking whether stdout is a terminal are questions with no answer that early, so a process-wide listener is built on first use rather than as a global.

## Logs in tests

nexus stands a recorder up for every run and buckets each test's events, so a test can ask what its subject logged:

```cpp
cc::rec::recording_listener rl;
{
    auto const h = cc::rec::register_listener(rl);
    do_the_thing();
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(h);
}
auto const r = rl.take();
```

**Declare the recorder fixture before the recording**, so it is destroyed after it.
A recording holds chunk references, and letting `shutdown()` free the pool underneath one is a use-after-free into the heap rather than a diagnostic.

This is what makes a warning testable at all.
`shaped-graphics`'s "more than 64 concurrent command lists" is asserted on in `libs/graphics/shaped-graphics/tests/barrier/slot-recording-test.cc`.
Both that it fires and that it fires **once**, which is the whole point of the guard around it.

A failing test's recording is written out as a `.ccrec`, so a CI failure arrives with its log inline.
Assertion failures and failed `CHECK`s are recorded too, which is what puts them in that file next to whatever the code recorded around them.
A check reported from a thread the test started is recorded the same way, and says so — nexus cannot attribute it to a section, so it never reaches the section tree.

Past thirty failures in one test a `CHECK` behaves as a `REQUIRE` and the test ends.
It is a circuit breaker against a wall of output nobody reads, not a resource limit, and it is measured per test rather than per section-replay pass.

## What it costs

A disabled site is one relaxed load and a test — that is the whole bet, and it is what makes leaving annotation in affordable.
`cc::rec::measure_overhead()` measures the rest on the machine you care about, rather than on someone else's.
