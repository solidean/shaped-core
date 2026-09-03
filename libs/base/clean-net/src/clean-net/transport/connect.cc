#include "connect.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/impl/reactor.hh>

// Happy eyeballs, RFC 8305, out of the pieces this library already has.
//
// WHAT THE RACE IS FOR.
// A machine whose IPv6 route is advertised and does not work is common, and nothing about an address says so.
// Trying one family and falling back when it times out costs the user the whole timeout; trying both costs
// milliseconds.
//
// HOW IT RUNS.
// The addresses are interleaved by family, so the first two attempts are one of each.
// Attempt zero starts at once, and a `timer` operation starts the next after the attempt delay -- the earlier attempt
// keeps running, because this is a stagger rather than a timeout.
// A failure also starts the next attempt immediately, which is what RFC 8305 asks for; the pending timer may then
// start one more, so the delay is a floor on the spacing rather than a ceiling on how many run at once.
//
// EVERY ATTEMPT GETS A CHILD TOKEN.
// Cancelling the caller's token cancels the race; settling the race cancels its own attempts and nothing above them.

namespace cnet
{
namespace
{
using handle = cc::shared_ptr<tcp_connection>;

/// What every callback of one race shares.
struct race_data
{
    isize next_index = 0;
    isize outstanding = 0;
    bool settled = false;

    /// The first attempt that failed, kept whole.
    ///
    /// The handle rather than a `cnet::error`, because `cc::any_error` erases our code into a string and the async
    /// can be propagated without losing either its message or its cancellation.
    /// The first rather than the last, because it is the one about the address the OS thought best.
    cc::shared_async<handle> first_failed;
};

struct race_state
{
    /// Set only by the io_system overload, which has no transport of its own to point at.
    cc::unique_ptr<native_transport> owned_transport;

    transport& t;
    io_system& io;
    cc::vector<endpoint> targets;
    connect_options options;
    cancel_token attempts;
    cc::shared_async<handle> promise;

    /// When the whole thing runs out, as one absolute reading -- so a resolve that took two seconds leaves two
    /// seconds less for the connecting.
    i64 deadline_ns = 0;

    cc::mutex<race_data> data;

    race_state(cc::unique_ptr<native_transport> owned,
               transport& transport_ref,
               io_system& io_ref,
               connect_options const& o,
               cancel_token const& token)
      : owned_transport(cc::move(owned)), t(transport_ref), io(io_ref), options(o), attempts(token.create_child())
    {
    }

    /// What is left of the budget, as a deadline an attempt can be given.
    [[nodiscard]] deadline remaining() const
    {
        if (deadline_ns <= 0)
            return deadline::never();

        auto const left_ms = (deadline_ns - io.time_source().now_ns()) / (1000 * 1000);
        return deadline::after_ms(left_ms > 0 ? left_ms : 0);
    }
};

/// Interleave the addresses by family, so the first two attempts are one of each.
///
/// The order within a family is the OS's, which already reflects RFC 6724 sorting that knows more about this
/// machine's routes than we do.
[[nodiscard]] cc::vector<endpoint> interleave_by_family(cc::vector<ip_address> const& addresses, i32 port)
{
    auto v6 = cc::vector<ip_address>();
    auto v4 = cc::vector<ip_address>();
    for (auto const& a : addresses)
        (a.family() == ip_family::v6 ? v6 : v4).push_back(a);

    // IPv6 leads, as RFC 8305 says: it is the family that is supposed to win, and the race exists for when it
    // silently cannot.
    auto ordered = cc::vector<endpoint>();
    for (isize i = 0; i < v6.size() || i < v4.size(); ++i)
    {
        if (i < v6.size())
            ordered.push_back(endpoint(v6[i], port));
        if (i < v4.size())
            ordered.push_back(endpoint(v4[i], port));
    }
    return ordered;
}

void start_next_attempt(cc::shared_ptr<race_state> const& state);

/// Claim the race for this outcome, or report that somebody else already won it.
[[nodiscard]] bool claim(cc::shared_ptr<race_state> const& state)
{
    return state->data.lock(
        [](race_data& d)
        {
            if (d.settled)
                return false;
            d.settled = true;
            return true;
        });
}

void finish_with_connection(cc::shared_ptr<race_state> const& state, handle won)
{
    if (!claim(state))
    {
        // A connection that arrived after the race was decided is closed rather than leaked: nobody is ever going to
        // be handed it.
        if (won.is_valid())
            won->close();
        return;
    }

    state->promise->push_value(cc::move(won));

    // The losers stop here, and only the losers: the attempts token is a child of the caller's.
    state->attempts.cancel();
}

void finish_with_failure(cc::shared_ptr<race_state> const& state, cc::shared_async<handle> const& failed)
{
    if (!claim(state))
        return;

    if (failed.is_valid())
        state->promise->push_error(failed->propagate_error());
    else
        state->promise->push_error(to_async_error({.code = error_code::host_unreachable,
                                                   .native_code = 0,
                                                   .message = cc::string("no address could be connected to")}));

    state->attempts.cancel();
}

void on_attempt_settled(cc::shared_ptr<race_state> const& state, cc::shared_async<handle> const& attempt)
{
    if (!attempt->has_error())
    {
        finish_with_connection(state, attempt->take_value());
        return;
    }

    auto const exhausted = state->data.lock(
        [&](race_data& d)
        {
            --d.outstanding;
            if (!d.first_failed.is_valid())
                d.first_failed = attempt;

            // Nothing left to try and nothing still running means the race is over.
            return d.outstanding == 0 && d.next_index >= state->targets.size();
        });

    if (exhausted)
    {
        auto const first = state->data.lock([](race_data const& d) { return d.first_failed; });
        finish_with_failure(state, first);
        return;
    }

    // A failed attempt does not wait for the stagger: the next address is worth trying now.
    start_next_attempt(state);
}

void start_next_attempt(cc::shared_ptr<race_state> const& state)
{
    auto const index = state->data.lock(
        [&](race_data& d) -> isize
        {
            if (d.settled || d.next_index >= state->targets.size())
                return -1;
            ++d.outstanding;
            return d.next_index++;
        });

    if (index < 0)
        return;

    auto const target = state->targets[index];
    CC_LOG_TRACE("racing attempt {} to {}", index, target);

    impl::when_ready(state->t.connect(target, state->remaining(), state->options.socket, state->attempts),
                     [state](cc::shared_async<handle> const& attempt) { on_attempt_settled(state, attempt); });

    // The stagger, which is a floor on the spacing rather than a promise that only one runs at a time.
    auto const more_to_try = state->data.lock([&](race_data const& d) { return d.next_index < state->targets.size(); });
    if (more_to_try)
        impl::run_after(state->io, state->options.attempt_delay_ms, [state] { start_next_attempt(state); });
}

[[nodiscard]] cc::shared_async<handle> race(cc::unique_ptr<native_transport> owned,
                                            transport& t,
                                            resolver& r,
                                            cc::string_view host,
                                            i32 port,
                                            connect_options const& options,
                                            cancel_token const& token)
{
    if (port <= 0 || port > 0xFFFF)
        return impl::failed_async<handle>(
            {.code = error_code::invalid_argument, .native_code = 0, .message = cc::format("{} is not a port", port)});

    auto state = cc::make_shared<race_state>(cc::move(owned), t, r.io(), options, token);
    state->promise = cc::make_async_manual<handle>();
    state->deadline_ns = deadline_to_absolute(r.io(), options.timeout);

    // The resolve spends the same budget the attempts will, and answers to the same child token.
    impl::when_ready(r.resolve(host, {.family = options.family, .timeout = options.timeout}, state->attempts),
                     [state, port](cc::shared_async<cc::vector<ip_address>> const& resolved)
                     {
                         if (resolved->has_error())
                         {
                             if (claim(state))
                                 state->promise->push_error(resolved->propagate_error());
                             return;
                         }

                         state->targets = interleave_by_family(resolved->value(), port);
                         if (state->targets.empty())
                         {
                             finish_with_failure(state, {});
                             return;
                         }

                         start_next_attempt(state);
                     });

    return state->promise;
}
} // namespace

cc::shared_async<handle> connect_to_host(transport& t,
                                         resolver& r,
                                         cc::string_view host,
                                         i32 port,
                                         connect_options const& options,
                                         cancel_token const& token)
{
    return race({}, t, r, host, port, options, token);
}

cc::shared_async<handle> connect_to_host(io_system& io,
                                         resolver& r,
                                         cc::string_view host,
                                         i32 port,
                                         connect_options const& options,
                                         cancel_token const& token)
{
    // Owned rather than a temporary: the race outlives this call, and a transport reference into a dead temporary is
    // the obvious way to get this wrong.
    auto owned = cc::make_unique<native_transport>(io);
    auto& t = *owned;
    return race(cc::move(owned), t, r, host, port, options, token);
}
} // namespace cnet
