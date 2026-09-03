#include "reactor.hh"

#include <clean-core/common/asserts.hh>

// The half of the reactor that is the same everywhere: the pending list, deadlines, cancellation, and the rule that
// nothing ever completes inline.
// `poll_once`, `drive_socket`, `wake` and `drain_wake` are the other half, in reactor_poll.cc, and they are the only
// code here that knows what a socket is.

namespace cnet::impl
{
namespace
{
[[nodiscard]] error make_error(error_code code, cc::string message)
{
    return {.code = code, .native_code = 0, .message = cc::move(message)};
}
} // namespace

reactor::~reactor()
{
    // Anything still pending is abandoned rather than completed: its owner is going away with us, and calling back
    // into a half-destroyed program is worse than not calling back at all.
    close_socket(_wake_socket);
}

cc::result<cc::unique_ptr<reactor>, error> reactor::try_create(clock& c)
{
    ensure_socket_platform();

    auto wake = create_wake_channel();
    if (wake.has_error())
        return cc::error(cc::move(wake).error());

    return cc::make_unique<reactor>(c, wake.value());
}

void reactor::submit(io_operation* op)
{
    CC_ASSERT(op != nullptr, "submitting a null operation");
    op->transferred = 0;

    auto e = entry{.op = op};

    // A connect is started here rather than on the first wait, because until ::connect runs there is nothing to
    // watch for: an unconnected socket is writable, and would complete instantly and wrongly.
    if (op->kind == io_op_kind::connect)
    {
        auto started = connect_socket(op->socket, op->peer);
        if (started.has_error())
            e.immediate_failure = cc::move(started).error();
    }

    _pending.push_back(cc::move(e));
}

void reactor::cancel(io_operation* op)
{
    for (auto& e : _pending)
        if (e.op == op)
            e.cancelled = true;
}

void reactor::signal(io_operation* op)
{
    for (auto& e : _pending)
        if (e.op == op)
            e.signalled = true;
}

i32 reactor::wait(i32 timeout_ms)
{
    poll_once(clamp_timeout(timeout_ms));
    return complete_ready();
}

i32 reactor::clamp_timeout(i32 timeout_ms) const
{
    // Anything already decided means there is nothing to wait for.
    for (auto const& e : _pending)
        if (e.cancelled || e.signalled || e.immediate_failure.has_value())
            return 0;

    auto const now = _clock.now_ns();
    auto shortest = timeout_ms;
    for (auto const& e : _pending)
    {
        if (e.op->deadline_ns <= 0)
            continue;

        auto const remaining_ms = (e.op->deadline_ns - now) / (1000 * 1000);
        auto const clamped = i32(remaining_ms <= 0 ? 0 : remaining_ms > 0x7FFFFFFF ? 0x7FFFFFFF : remaining_ms);
        if (shortest < 0 || clamped < shortest)
            shortest = clamped;
    }
    return shortest;
}

cc::optional<cc::optional<error>> reactor::drive(entry& e)
{
    switch (e.op->kind)
    {
    case io_op_kind::timer:
        // Nothing to do but wait for the clock, and its deadline is this operation's whole purpose.
        return {};

    case io_op_kind::manual:
        if (!e.signalled)
            return {};
        return cc::optional<error>();

    case io_op_kind::connect:
    case io_op_kind::accept:
    case io_op_kind::receive:
    case io_op_kind::send:
        return drive_socket(e);
    }
    return {};
}

i32 reactor::complete_ready()
{
    auto const now = _clock.now_ns();
    auto completions = cc::vector<completion>();

    for (isize i = 0; i < _pending.size();)
    {
        auto& e = _pending[i];
        auto outcome = cc::optional<cc::optional<error>>();

        if (e.cancelled)
            outcome = cc::optional<error>(make_error(error_code::cancelled, cc::string("the operation was cancelled")));
        else if (e.immediate_failure.has_value())
            outcome = e.immediate_failure;
        else
            outcome = drive(e);

        // A deadline is checked after driving, so an operation that finished in the same wait it expired in reports
        // what actually happened rather than a timeout.
        if (!outcome.has_value() && e.op->deadline_ns > 0 && now >= e.op->deadline_ns)
        {
            // A timer's deadline is the result rather than a failure, which is the one thing that makes it a timer.
            if (e.op->kind == io_op_kind::timer)
                outcome = cc::optional<error>();
            else
                outcome = cc::optional<error>(make_error(error_code::timed_out, cc::string("the operation ran out of "
                                                                                           "time")));
        }

        if (!outcome.has_value())
        {
            e.readable = false;
            e.writable = false;
            e.errored = false;
            ++i;
            continue;
        }

        completions.push_back({.op = e.op, .failure = cc::move(outcome.value())});
        _pending[i] = cc::move(_pending[_pending.size() - 1]);
        _pending.remove_back();
    }

    // Callbacks run only once every completed entry is out of _pending, so a handler is free to submit again -- and
    // free to destroy the operation it was just handed.
    for (auto& c : completions)
        c.op->on_complete(cc::move(c.failure));

    return i32(completions.size());
}
} // namespace cnet::impl
