#include "record-test-types.hh"

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/time.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/platform/symbolize.hh>
#include <clean-core/record/hot_functions.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace cc_rec_test;

// A hot-functions report is what makes a sampled recording actionable without a viewer, so what is asserted here is
// that its SHAPE is sound whatever the machine was doing.
//
// Sample counts are statistical and a loaded CI box will not reproduce them, so nothing asserts on a percentage.
// What must hold exactly: self time partitions the samples, recursion is counted once, and the order is deterministic.

namespace
{
[[nodiscard]] bool can_sample()
{
    return cc::stack_capture_from_context_available() && CC_HAS_THREADS;
}

[[nodiscard]] bool build_has_symbols()
{
    if (!cc::symbolizer::is_available())
        return false;

    cc::symbolizer sym;
    return sym.resolve(reinterpret_cast<void const*>(&can_sample)).has_function();
}

/// Recurses to a known depth and logs from the bottom, so the captured stack holds ONE function many times over.
/// That is the case a naive inclusive count reports at several hundred percent.
CC_DONT_INLINE void recurse_then_log(int depth)
{
    if (depth <= 0)
    {
        CC_LOG_ERROR("bottom of the recursion");
        return;
    }

    recurse_then_log(depth - 1);

    u64 volatile sink = u64(depth); // defeats the tail call, which would erase the frames this test is about
    (void)sink;
}

CC_DONT_INLINE void burn_for_secs(f64 secs)
{
    CC_RECORD_MARK("busy");

    auto const start = cc::current_time_steady_secs();
    u64 volatile sink = 0;
    while (cc::current_time_steady_secs() - start < secs)
        for (int i = 0; i < 4096; ++i)
            sink = sink + u64(i);
}

cc::rec::recording capture(cc::function_ref<void()> body, bool with_sampling)
{
    cc::rec::recording_listener rl;
    {
        scoped_listener const reg(rl);
        if (with_sampling)
        {
            cc::rec::sampling_scope const sampling({.rate_hz = 500.0});
            body();
        }
        else
            body();

        cc::rec::flush_blocking();
    }
    return rl.take();
}

/// The entry whose name CONTAINS this, because a mangled name is the compiler business and not a contract.
[[nodiscard]] cc::rec::hot_function const* find_containing(cc::rec::hot_report const& r, cc::string_view part)
{
    for (auto const& f : r.functions)
        if (f.function.contains(part))
            return &f;
    return nullptr;
}
} // namespace

REC_TEST("record/hot - a recording with no stacks reports nothing, and says so")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { CC_RECORD_MARK("nothing to see"); }, false);
    auto const report = cc::rec::hot_functions(r);

    CHECK(report.empty());
    CHECK(report.sample_count == 0);
    CHECK(!report.to_string().empty()); // an empty report still renders rather than returning nothing at all
    CHECK(report.self_ratio_of("anything") == 0);
    CHECK(report.find("anything") == nullptr);
}

REC_TEST("record/hot - every sample lands in exactly one self bucket")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { burn_for_secs(0.25); }, true);
    auto const report = cc::rec::hot_functions(r);

    REQUIRE(report.sample_count > 0);

    // The invariant that makes a percentage mean anything: self time partitions the samples, so the column sums to
    // the sample count exactly and the ratios sum to one.
    isize self_total = 0;
    for (auto const& f : report.functions)
        self_total += f.self_samples;
    CHECK(self_total == report.sample_count);

    auto ratio_sum = 0.0;
    for (auto const& f : report.functions)
        ratio_sum += f.self_ratio;
    CHECK(ratio_sum > 1.0 - 1e-9);
    CHECK(ratio_sum < 1.0 + 1e-9);
}

REC_TEST("record/hot - inclusive counts never exceed the sample count, recursion included")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { recurse_then_log(6); }, false);
    auto const report = cc::rec::hot_functions(r, {.include_stacktrace_events = true});

    REQUIRE(report.sample_count > 0);

    // A function on the stack seven times is still ONE stack: counting per frame would report 700%.
    for (auto const& f : report.functions)
    {
        CHECK(f.total_samples <= report.sample_count);
        CHECK(f.total_samples >= f.self_samples);
        CHECK(f.total_ratio <= 1.0 + 1e-9);
    }

    if (build_has_symbols())
    {
        auto const* const recursed = find_containing(report, "recurse_then_log");
        if (recursed != nullptr)
            CHECK(recursed->total_samples == 1);
    }
}

REC_TEST("record/hot - stacktrace events are folded in only when asked for")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { recurse_then_log(3); }, false);

    // The default answers where the program spent its time, and a stack captured at an error is a different question
    // with a much smaller sample.
    CHECK(cc::rec::hot_functions(r).sample_count == 0);
    CHECK(cc::rec::hot_functions(r, {.include_stacktrace_events = true}).sample_count > 0);
}

REC_TEST("record/hot - the order is the same twice, and sorted by self time")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { recurse_then_log(5); }, false);

    auto const a = cc::rec::hot_functions(r, {.include_stacktrace_events = true});
    auto const b = cc::rec::hot_functions(r, {.include_stacktrace_events = true});

    REQUIRE(!a.empty());
    REQUIRE(a.functions.size() == b.functions.size());

    // Deterministic order is what makes a report assertable: ties break by name rather than by map iteration.
    for (isize i = 0; i < a.functions.size(); ++i)
        CHECK(a.functions[i].function == b.functions[i].function);

    for (isize i = 1; i < a.functions.size(); ++i)
        CHECK(a.functions[i - 1].self_samples >= a.functions[i].self_samples);
}

REC_TEST("record/hot - min_self_ratio drops the tail and nothing else")
{
    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { recurse_then_log(5); }, false);

    auto const all = cc::rec::hot_functions(r, {.include_stacktrace_events = true});
    auto const top = cc::rec::hot_functions(r, {.include_stacktrace_events = true, .min_self_ratio = 0.5});

    REQUIRE(!all.empty());
    CHECK(top.functions.size() <= all.functions.size());

    // Filtering is a display concern: the denominator stays the whole profile, or every surviving row would inflate.
    CHECK(top.sample_count == all.sample_count);
    for (auto const& f : top.functions)
        CHECK(f.self_ratio >= 0.5);
}

REC_TEST("record/hot - a report renders as something a human can read")
{
    if (!can_sample())
        SKIP("this build has no sampler");

    rec_fixture const fixture(deterministic_config());

    auto const r = capture([] { burn_for_secs(0.2); }, true);
    auto const report = cc::rec::hot_functions(r);

    REQUIRE(report.sample_count > 0);

    auto const text = report.to_string(5);
    CHECK(text.contains("sample"));
    CHECK(text.contains("self%"));
}
