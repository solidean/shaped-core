#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/error/exception.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>

// Diverting a running test's checks, for tools that drive code expected to fail often across threads — the fuzz engine's awaited ops.
// Its own header rather than check.hh, which every test TU includes and which should not pay for it.

namespace nx::impl
{
struct test_context;

// The sink a divert tallies into: what check_capture_sink records, written from any thread at once.
struct async_check_capture_sink
{
    cc::atomic<int> executed = {0};
    cc::atomic<int> failed = {0};
    cc::atomic<bool> require_failed = {false};
    cc::mutex<cc::string> first_message; // expanded message of the first failure, if any
};

// Diverts every check reported for the RUNNING TEST into `sink` until destroyed, from whichever thread reports it.
// Where scoped_check_capture follows one thread, this follows the test, so it reaches work that hops threads.
// It is per test rather than per strand: anything else reporting for this test meanwhile is diverted too.
// A diverted CC_ASSERT throws captured_assertion instead of aborting, and nothing diverted is logged or counts against the test.
// Must be constructed inside a running test; one at a time per test.
struct scoped_test_check_divert
{
    explicit scoped_test_check_divert(async_check_capture_sink& sink);
    ~scoped_test_check_divert();

    scoped_test_check_divert(scoped_test_check_divert const&) = delete;
    scoped_test_check_divert& operator=(scoped_test_check_divert const&) = delete;
    scoped_test_check_divert(scoped_test_check_divert&&) = delete;
    scoped_test_check_divert& operator=(scoped_test_check_divert&&) = delete;

private:
    test_context* _ctx = nullptr;
};

// Thrown in place of a failing CC_ASSERT under a capture or a divert, so the code that asserted fails instead of the process.
// A std::exception so its message survives becoming an async node's error, which a non-std throw's does not.
struct captured_assertion : std::exception
{
    cc::string message;

    explicit captured_assertion(cc::string m) : message(cc::move(m)) { message.c_str_materialize(); }
    [[nodiscard]] char const* what() const noexcept override { return message.c_str_if_terminated(); }
};
} // namespace nx::impl
