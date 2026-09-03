#pragma once

#include <clean-core/thread/atomic.hh>
#include <clean-net/fwd.hh>

/// The monotonic time source the reactor, the timeouts and the rate limits all read.
///
/// A seam rather than a direct call to the OS, and it is here from the first version on purpose: a test that has to
/// sleep to prove a 30-second timeout fires is both slow and flaky, and retrofitting a clock means touching every
/// place that reads one.
///
/// Only *differences* are meaningful.
/// The origin is arbitrary and the scale never jumps backwards, which is what a wall clock cannot promise.
class cnet::clock
{
public:
    [[nodiscard]] virtual i64 now_ns() = 0;

    clock() = default;
    clock(clock const&) = delete;
    clock& operator=(clock const&) = delete;
    virtual ~clock() = default;
};

/// A clock that only moves when a test moves it.
///
/// Safe to read from the reactor thread while a test advances it, which is the whole reason it exists: the thing
/// being tested is usually asleep on a deadline this is about to cross.
class cnet::manual_clock final : public cnet::clock
{
public:
    explicit manual_clock(i64 start_ns = 0) : _now_ns(start_ns) {}

    [[nodiscard]] i64 now_ns() override { return _now_ns.load(); }

    void advance_ms(i64 ms) { advance_ns(ms * 1000 * 1000); }
    void advance_ns(i64 ns) { _now_ns.fetch_add(ns); }

    /// Jump to an absolute reading.
    /// Must not move backwards -- a monotonic clock that does is not one.
    void set_ns(i64 ns);

private:
    cc::atomic<i64> _now_ns;
};
