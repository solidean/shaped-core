#include "simulated_transport.hh"

#include <clean-core/math/random.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/reactor.hh>

#include <memory> // std::unique_ptr, to own a backend through its interface -- see .shaped-lint.yml

// A simulated link is a wrapper rather than a transport of its own: it delays, drops and cuts on the way through to
// whatever it was given.
//
// HOW A DELAY IS BUILT.
// A reactor `timer` fires at the delayed moment, and only then is the real operation started; its outcome is then
// handed to the promise the caller has been holding all along.
// So a delay is measured on the io_system's clock like every other deadline here, which is what lets a test move it
// by hand instead of sleeping through it.
//
// WHY OUTCOMES ARE FORWARDED RATHER THAN COMPOSED.
// cc::async's compute frames need a scheduler, and this library deliberately has none: the reactor is the only place
// work runs.
// A one-shot completion hook on the underlying handle keeps every step on the reactor thread and needs no pool.

namespace cnet
{
namespace
{
/// Run `fn(source)` once `source` has settled -- at once if it already has.
///
/// `fn` is what pushes into whatever promise the caller is holding, so this is the only piece of async plumbing in
/// this file.
/// It fires on the thread that completed `source`, which for everything in this library is the reactor thread.
template <class T, class F>
void when_ready(cc::shared_async<T> source, F fn)
{
    struct waiter
    {
        cc::shared_async<T> source;
        F fn;

        static void fire(void* ctx)
        {
            auto* const self = static_cast<waiter*>(ctx);
            self->fn(self->source);
            delete self;
        }
    };

    auto* const w = new waiter{cc::move(source), cc::move(fn)};
    if (w->source->install_completion_hook_or_ready(&waiter::fire, w))
        waiter::fire(w);
}

/// Hand one settled async's outcome to another promise, whatever it turned out to be.
template <class T>
void forward_outcome(cc::shared_async<T> const& from, cc::shared_async<T> const& to)
{
    if (from->has_error())
        to->push_error(from->propagate_error());
    else
        to->push_value(from->take_value());
}

template <class T>
[[nodiscard]] cc::shared_async<T> failed_async(error e)
{
    auto promise = cc::make_async_manual<T>();
    promise->push_error(to_async_error(cc::move(e)));
    return promise;
}

[[nodiscard]] error reset_error(cc::string_view what)
{
    return {.code = error_code::connection_reset, .native_code = 0, .message = cc::string(what)};
}

/// How many bytes this connection has taken, against the limit that cuts it.
///
/// Shared rather than a member, because a completion can outlive the connection that started it: a caller is free to
/// drop the last handle while a read is still in flight.
struct link_budget
{
    isize received = 0;
    bool cut = false;
};
} // namespace

/// What every connection and listener from this transport shares.
struct simulated_transport::state
{
    io_system& io;
    transport& under;
    link_conditions conditions;

    /// One stream for the whole link, so a run replays from the seed alone.
    cc::mutex<cc::random> rng;

    state(io_system& s, transport& u, link_conditions const& c)
      : io(s), under(u), conditions(c), rng(cc::random(c.seed))
    {
    }

    /// The delay, in milliseconds, that moving `bytes` costs here.
    [[nodiscard]] i64 delay_ms_for(isize bytes)
    {
        auto delay = f64(conditions.latency_ms);

        if (conditions.jitter_ms > 0)
            delay += rng.lock([&](cc::random& r) { return f64(r.uniform(-conditions.jitter_ms, conditions.jitter_ms)); });

        if (conditions.bandwidth_bytes_per_sec > 0 && bytes > 0)
            delay += 1000.0 * f64(bytes) / f64(conditions.bandwidth_bytes_per_sec);

        return delay <= 0 ? 0 : i64(delay);
    }

    [[nodiscard]] bool draws_loss()
    {
        if (conditions.loss_probability <= 0)
            return false;
        return rng.lock([&](cc::random& r) { return r.uniform(0.0f, 1.0f) < conditions.loss_probability; });
    }
};

namespace
{
using sim_state = simulated_transport::state;

/// A timer that starts an operation when it fires, and forwards what that operation reports.
///
/// `begin` is called on the reactor thread and returns the async it started.
template <class T, class F>
struct deferred_start final : impl::io_operation
{
    cc::unique_ptr<deferred_start> self;
    cc::shared_async<T> target;
    F begin;

    /// The delay itself is cancellable, so cancelling during a 200 ms latency does not wait it out first.
    impl::cancel_registration cancellation;

    deferred_start(cc::shared_async<T> t, F f) : target(cc::move(t)), begin(cc::move(f)) {}

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        if (failure.has_value())
        {
            // A timer fails when it is cancelled, and when the io_system is going away, and then the caller deserves to hear that.
            target->push_error(to_async_error(cc::move(failure.value())));
            return;
        }

        when_ready(begin(), [to = cc::move(target)](cc::shared_async<T> const& source) { forward_outcome(source, to); });
    }
};

/// Start `begin` after `delay_ms` on the link's clock, and hand its outcome to `target`.
template <class T, class F>
void run_delayed(sim_state& s, i64 delay_ms, cc::shared_async<T> target, cancel_token const& token, F begin)
{
    if (delay_ms <= 0)
    {
        when_ready(begin(), [to = cc::move(target)](cc::shared_async<T> const& source) { forward_outcome(source, to); });
        return;
    }

    auto op = cc::make_unique<deferred_start<T, F>>(cc::move(target), cc::move(begin));
    op->kind = impl::io_op_kind::timer;
    op->deadline_ns = s.io.time_source().now_ns() + delay_ms * 1000 * 1000;

    auto* const raw = op.get();
    raw->self = cc::move(op);
    s.io.submit(raw);
    raw->cancellation.attach(token, s.io, raw);
}

/// Wrap a connection that arrived from the transport underneath, so the conditions apply to it too.
[[nodiscard]] cc::shared_ptr<tcp_connection> wrap_connection(cc::shared_ptr<sim_state> s,
                                                             cc::shared_ptr<tcp_connection> under);

/// A connection over a simulated link.
class simulated_connection final : public connection_backend
{
public:
    simulated_connection(cc::shared_ptr<sim_state> s, cc::shared_ptr<tcp_connection> under)
      : _state(cc::move(s)), _under(cc::move(under)), _budget(cc::make_shared<link_budget>())
    {
    }

    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d, cancel_token const& token) override
    {
        if (_budget->cut)
            return failed_async<isize>(reset_error("the simulated link already cut this connection"));

        if (_state->draws_loss())
            return cut_now<isize>("the simulated link dropped this read");

        auto promise = cc::make_async_manual<isize>();

        auto const budget = _budget;
        auto const limit = _state->conditions.reset_after_bytes;
        auto under = _under;
        auto const target = promise;

        run_delayed<isize>(*_state, _state->delay_ms_for(buffer.size()), promise, token,
                           [under, buffer, d, budget, limit, target, token]
                           {
                               auto counted = cc::make_async_manual<isize>();
                               when_ready(under->receive(buffer, d, token),
                                          [counted, budget, limit, under](cc::shared_async<isize> const& inner)
                                          {
                                              if (inner->has_error())
                                              {
                                                  counted->push_error(inner->propagate_error());
                                                  return;
                                              }

                                              auto const n = inner->value();
                                              budget->received += n;

                                              // The budget is counted on what actually arrived, so a caller reading
                                              // in small pieces is cut at the same offset as one reading in large.
                                              if (limit > 0 && budget->received >= limit)
                                              {
                                                  budget->cut = true;
                                                  under->close();
                                                  counted->push_error(to_async_error(reset_error("the simulated link "
                                                                                                 "cut this "
                                                                                                 "connection")));
                                                  return;
                                              }
                                              counted->push_value(n);
                                          });
                               return counted;
                           });

        return promise;
    }

    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes, deadline d, cancel_token const& token) override
    {
        if (_budget->cut)
            return failed_async<cc::unit>(reset_error("the simulated link already cut this connection"));

        if (_state->draws_loss())
            return cut_now<cc::unit>("the simulated link dropped this write");

        auto promise = cc::make_async_manual<cc::unit>();
        auto under = _under;
        run_delayed<cc::unit>(*_state, _state->delay_ms_for(bytes.size()), promise, token,
                              [under, bytes, d, token] { return under->send(bytes, d, token); });
        return promise;
    }

    cc::result<cc::unit, error> shutdown_send() override { return _under->shutdown_send(); }

    [[nodiscard]] endpoint local() const override { return _under->local(); }
    [[nodiscard]] endpoint peer() const override { return _under->peer(); }
    [[nodiscard]] bool is_open() const override { return !_budget->cut && _under->is_open(); }
    void close() override { _under->close(); }

private:
    /// Cut the connection now, and report it the way a peer that vanished would be reported.
    template <class T>
    [[nodiscard]] cc::shared_async<T> cut_now(cc::string_view why)
    {
        _budget->cut = true;
        _under->close();
        return failed_async<T>(reset_error(why));
    }

    cc::shared_ptr<sim_state> _state;
    cc::shared_ptr<tcp_connection> _under;
    cc::shared_ptr<link_budget> _budget;
};

cc::shared_ptr<tcp_connection> wrap_connection(cc::shared_ptr<sim_state> s, cc::shared_ptr<tcp_connection> under)
{
    return cc::make_shared<tcp_connection>(std::make_unique<simulated_connection>(cc::move(s), cc::move(under)));
}

/// A listener whose accepted connections come back wrapped in the same conditions.
class simulated_listener final : public listener_backend
{
public:
    simulated_listener(cc::shared_ptr<sim_state> s, cc::unique_ptr<tcp_listener> under)
      : _state(cc::move(s)), _under(cc::move(under))
    {
    }

    [[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> accept(deadline d, cancel_token const& token) override
    {
        using handle = cc::shared_ptr<tcp_connection>;

        auto promise = cc::make_async_manual<handle>();
        auto const target = promise;
        auto s = _state;

        // An accept itself is not delayed: the wait belongs to whoever has not connected yet, rather than to the
        // link.
        when_ready(_under->accept(d, token),
                   [target, s](cc::shared_async<handle> const& inner)
                   {
                       if (inner->has_error())
                           target->push_error(inner->propagate_error());
                       else
                           target->push_value(wrap_connection(s, inner->take_value()));
                   });

        return promise;
    }

    [[nodiscard]] endpoint local() const override { return _under->local(); }

private:
    cc::shared_ptr<sim_state> _state;
    cc::unique_ptr<tcp_listener> _under;
};
} // namespace

// ---- the transport -------------------------------------------------------------------------------------

simulated_transport::simulated_transport(io_system& io, transport& under, link_conditions const& conditions)
  : _state(cc::make_shared<state>(io, under, conditions))
{
    // The seed is logged rather than merely stored, because a failing run replays from it and nobody can read it out
    // of a crashed process.
    CC_LOG_INFO("simulated link up: seed {}, latency {} ms, loss {}, reset after {} bytes", conditions.seed,
                conditions.latency_ms, conditions.loss_probability, conditions.reset_after_bytes);
}

simulated_transport::~simulated_transport() = default;

bool simulated_transport::is_supported() const
{
    return _state->under.is_supported();
}

cc::result<cc::unique_ptr<tcp_listener>, error> simulated_transport::listen(endpoint const& where,
                                                                            tcp_listen_options const& options)
{
    auto under = _state->under.listen(where, options);
    if (under.has_error())
        return cc::error(cc::move(under).error());

    return cc::make_unique<tcp_listener>(std::make_unique<simulated_listener>(_state, cc::move(under).value()));
}

cc::shared_async<cc::shared_ptr<tcp_connection>> simulated_transport::connect(endpoint const& where,
                                                                              deadline d,
                                                                              tcp_options const& options,
                                                                              cancel_token const& token)
{
    using handle = cc::shared_ptr<tcp_connection>;

    // A lost connect is a refusal rather than a reset: nothing was established to reset.
    if (_state->draws_loss())
        return failed_async<handle>({.code = error_code::connection_refused,
                                     .native_code = 0,
                                     .message = cc::string("the simulated link dropped this connect")});

    auto promise = cc::make_async_manual<handle>();
    auto s = _state;
    auto const target = endpoint(where);

    run_delayed<handle>(*_state, _state->delay_ms_for(0), promise, token,
                        [s, target, d, options, token]
                        {
                            auto wrapped = cc::make_async_manual<handle>();
                            when_ready(s->under.connect(target, d, options, token),
                                       [wrapped, s](cc::shared_async<handle> const& inner)
                                       {
                                           if (inner->has_error())
                                               wrapped->push_error(inner->propagate_error());
                                           else
                                               wrapped->push_value(wrap_connection(s, inner->take_value()));
                                       });
                            return wrapped;
                        });

    return promise;
}
} // namespace cnet
