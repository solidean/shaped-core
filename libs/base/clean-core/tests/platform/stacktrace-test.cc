#include <clean-core/error/impl/posix_thread_stacks.hh>
#include <clean-core/platform/stacktrace.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

// What cc::stacktrace can report, which is a property of the toolchain rather than of the code under it.
//
// Three backends answer differently and all three are legitimate: std::stacktrace where <stacktrace> links, the
// Emscripten callstack where it does not but the platform still renders frames, and the empty stub where neither.
// CC_HAS_STACKTRACE is the only thing that separates them, so these assert against it rather than against a platform.

// Rendering a trace symbolizes: on Windows cc::to_string resolves every frame through DbgHelp, under the same
// process-global lock cc::symbolizer takes, so no exclusion tag is needed to keep these apart from symbolize-test.cc.

namespace
{
/// Named, and deliberately not inlined, so a trace taken inside it can be looked for by name.
CC_DONT_INLINE cc::stacktrace capture_here_for_stacktrace_test()
{
    return cc::stacktrace::current();
}
} // namespace

TEST("stacktrace - current() reports frames wherever the toolchain can render them")
{
    auto const trace = capture_here_for_stacktrace_test();

#if CC_HAS_STACKTRACE
    CHECK(!trace.empty());
    CHECK(trace.size() > 0);
    CHECK(!cc::to_string(trace).empty());

    // The trace begins at the CALLER, where std::stacktrace::current begins, rather than inside the capture itself.
    // Vacuous where the toolchain renders no names, which is the Release wasm build; a real check everywhere else.
    CHECK(!cc::string_view(cc::to_string(trace)).contains("cc::stacktrace::current"));
#else
    // The stub is a value type that reports nothing, which is a supported answer rather than a failure.
    CHECK(trace.empty());
    CHECK(trace.size() == 0);
#endif
}

TEST("stacktrace - skip drops frames from the top")
{
#if CC_HAS_STACKTRACE
    auto const all = cc::stacktrace::current();
    auto const skipped = cc::stacktrace::current(1);

    // Skipping cannot invent frames, and on any stack deep enough to matter it removes one.
    CHECK(skipped.size() <= all.size());
    if (all.size() > 1)
        CHECK(skipped.size() < all.size());
#else
    CHECK(cc::stacktrace::current(1).empty());
#endif
}

TEST("stacktrace - max_depth caps what is kept")
{
#if CC_HAS_STACKTRACE
    auto const capped = cc::stacktrace::current(0, 2);
    CHECK(capped.size() <= 2);
#else
    CHECK(cc::stacktrace::current(0, 2).empty());
#endif
}

// Reaching threads other than the calling one, which no platform does the same way.
//
// There is no assertion here about what the stacks CONTAIN: producing one means signalling a thread and waiting,
// which a test cannot arrange without a thread deliberately stuck.
// What is pinned is that the capability reports itself honestly, so an install that silently failed is a red test
// rather than an empty section in a crash report.
TEST("crash handler - the other-thread reporter says whether it is there")
{
    // nx::run installs the crash handler before anything runs, so by the time a test body executes the reporter is
    // either up or has failed to come up.
    auto const available = cc::impl::posix_thread_stacks_available();

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    // The whole point on the platform this exists for.
    // A false here means sigaction or sem_init failed at install time, which is exactly the silent failure that
    // would otherwise surface as "other threads: <not reached>" in a report somebody needed.
    CHECK(available);
#else
    // Everywhere else the answer is no, and saying so is what keeps the crash report honest rather than empty.
    CHECK(!available);
#endif
}
