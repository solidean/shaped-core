#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/record/recording.hh>

namespace nx
{
struct test_recorder;
} // namespace nx

// Reading back what the running test recorded.
//
// Tests run asynchronously and in parallel, so "what did this test record" cannot be answered by filtering on a
// thread: a logical test runs on whichever workers pick it up, and several are in flight at once.
// The answer is the AMBIENT context — nexus opens a trace scope per test, and the run buckets every event by the trace
// id its ambient delta named.
//
// **`sync()` is the only call here that talks to the recorder**, and it drains the recorder's actor to do it.
// That is why this is a handle you hold rather than a comparison you write inline: one drain per question would put a
// process-wide mutex inside a loop of CHECKs, where one drain per test is nothing.
//
//     auto rec = nx::test_recording();
//     do_the_work();
//     rec.sync();
//     CHECK(rec.all().count("cache-miss") == 1);
//
//     do_more_work();
//     auto const since = rec.sync();          // only what arrived in between
//     CHECK(since.contains("flush"));
//
// A test declared `nx::config::no_recording` is not bucketed, so its recorder stays empty.

/// Everything the running test has recorded, and the delta since the last sync.
///
/// Cheap to hold and cheap to read out of: a recording is a set of chunk references, so neither `sync()` nor `all()`
/// copies any event bytes.
struct nx::test_recorder
{
    test_recorder() = default;
    explicit test_recorder(cc::rec::trace_id trace) : _trace(trace) {}

    /// Pulls in everything recorded under this test since the last sync, and returns just that.
    ///
    /// Blocking: it drains the recorder's actor under a process-wide mutex.
    /// A few times per test is nothing; once per check in a loop is not.
    cc::rec::recording sync();

    /// Everything this recorder has pulled in so far, across every sync.
    [[nodiscard]] cc::rec::recording const& all() const { return _all; }

    /// The trace id this recorder is bound to, or `none` outside a bucketed test.
    [[nodiscard]] cc::rec::trace_id trace() const { return _trace; }

    /// Whether this recorder is attached to anything at all.
    /// False outside a test, and false for one declared `nx::config::no_recording`.
    [[nodiscard]] bool is_attached() const { return _trace != cc::rec::trace_id::none; }

private:
    cc::rec::trace_id _trace = cc::rec::trace_id::none;
    cc::rec::recording _all;
};

namespace nx
{
/// A recorder for the test running on this thread, found through the ambient chain.
///
/// Correct on a pool worker driving this test's nodes, and equally correct when that worker is driving some other
/// test's stolen node — which is exactly why this reads the ambient rather than a thread-local.
[[nodiscard]] nx::test_recorder test_recording();
} // namespace nx
