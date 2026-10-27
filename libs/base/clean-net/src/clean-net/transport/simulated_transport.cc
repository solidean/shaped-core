#include "simulated_transport.hh"

#include <clean-core/math/random.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/reactor.hh>
#include <clean-net/transport/datagram.hh>

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
using impl::failed_async;
using impl::forward_outcome;
using impl::when_ready;

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

    [[nodiscard]] bool draws_loss() { return draws(conditions.loss_probability); }

    /// Whether an event of probability `p` happens this time; never draws for a condition that is off, so a link with
    /// it off replays the same stream as one that never had it.
    [[nodiscard]] bool draws(f32 p)
    {
        if (p <= 0)
            return false;
        return rng.lock([&](cc::random& r) { return r.uniform(0.0f, 1.0f) < p; });
    }

    /// The delay, in nanoseconds, a datagram of `bytes` takes: `delay_ms_for` without its rounding, since a datagram's
    /// release time is compared against the clock directly.
    [[nodiscard]] i64 delay_ns_for(isize bytes)
    {
        auto delay = f64(conditions.latency_ms);

        if (conditions.jitter_ms > 0)
            delay += rng.lock([&](cc::random& r) { return f64(r.uniform(-conditions.jitter_ms, conditions.jitter_ms)); });

        if (conditions.bandwidth_bytes_per_sec > 0 && bytes > 0)
            delay += 1000.0 * f64(bytes) / f64(conditions.bandwidth_bytes_per_sec);

        return delay <= 0 ? 0 : i64(delay * 1000.0 * 1000.0);
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
    raw->cancellation.attach(s.io.submit(raw), token);
}

/// Wrap a connection that arrived from the transport underneath, so the conditions apply to it too.
[[nodiscard]] cc::shared_ptr<stream_connection> wrap_connection(cc::shared_ptr<sim_state> s,
                                                                cc::shared_ptr<stream_connection> under);

/// A connection over a simulated link.
class simulated_connection final : public connection_backend
{
public:
    simulated_connection(cc::shared_ptr<sim_state> s, cc::shared_ptr<stream_connection> under)
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
    [[nodiscard]] cc::string_view negotiated_alpn() const override { return _under->negotiated_alpn(); }
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
    cc::shared_ptr<stream_connection> _under;
    cc::shared_ptr<link_budget> _budget;
};

cc::shared_ptr<stream_connection> wrap_connection(cc::shared_ptr<sim_state> s, cc::shared_ptr<stream_connection> under)
{
    return cc::make_shared<stream_connection>(std::make_unique<simulated_connection>(cc::move(s), cc::move(under)));
}

/// A listener whose accepted connections come back wrapped in the same conditions.
class simulated_listener final : public listener_backend
{
public:
    simulated_listener(cc::shared_ptr<sim_state> s, cc::unique_ptr<stream_listener> under)
      : _state(cc::move(s)), _under(cc::move(under))
    {
    }

    [[nodiscard]] cc::shared_async<cc::shared_ptr<stream_connection>> accept(deadline d, cancel_token const& token) override
    {
        using handle = cc::shared_ptr<stream_connection>;

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
    cc::unique_ptr<stream_listener> _under;
};
} // namespace

// ---- datagrams -----------------------------------------------------------------------------------------
//
// WHERE A DATAGRAM IS IMPAIRED: where it is received, and nowhere else.
// Each arrival is decided once -- dropped, duplicated, delayed, or delayed further so later ones overtake it -- and then
// held until its release time, which `receive_batch` and the readiness wait both read.
// So nothing per datagram needs a timer, and in a test where every socket comes from one simulated transport each
// datagram crosses exactly one impairment.
// Over the native transport that means only what arrives here is impaired, which [datagrams.md](../../../docs/datagrams.md) records.

namespace
{
/// A datagram the link has decided to deliver, waiting for its moment.
struct held_datagram
{
    endpoint peer;
    cc::vector<byte> bytes;
    bool truncated = false;

    /// When it arrives, as far as the caller can tell, on the io_system's clock.
    i64 release_ns = 0;
};

struct held_queue
{
    /// Ordered by release time, earliest first, so what is due is always a prefix.
    cc::vector<held_datagram> items;

    /// The one readiness wait on this socket.
    impl::io_operation* parked = nullptr;

    /// When the timer that will next look at the queue fires; 0 when none is pending.
    i64 timer_at_ns = 0;

    [[nodiscard]] bool has_due(i64 now) const { return !items.empty() && items[0].release_ns <= now; }
};

/// What `receive_batch` reads the wrapped socket into, before each datagram is copied into the held queue.
struct pull_scratch
{
    cc::vector<byte> bytes;
    cc::vector<datagram> records;
};

/// The largest datagram anything carries, so the scratch slots never cut one the wrapped socket received whole.
constexpr isize k_scratch_slot = 65536;
constexpr isize k_scratch_slots = 8;

/// A readiness wait on a simulated datagram socket, parked until something is due.
struct sim_readable_op final : impl::io_operation
{
    cc::shared_async<cc::unit> promise;
    cc::unique_ptr<sim_readable_op> self;
    impl::cancel_registration cancellation;
    cc::shared_ptr<cc::mutex<held_queue>> queue;

    void on_complete(cc::optional<error> failure) override
    {
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        queue->lock(
            [this](held_queue& q)
            {
                if (q.parked == this)
                    q.parked = nullptr;
            });

        if (failure.has_value())
            promise->push_error(to_async_error(cc::move(failure.value())));
        else
            promise->push_value(cc::unit{});
    }
};

/// What a simulated datagram socket and its timers share; a timer can fire after the socket is gone.
struct sim_datagram_state
{
    cc::shared_ptr<sim_state> link;
    cc::shared_ptr<cc::mutex<held_queue>> queue = cc::make_shared<cc::mutex<held_queue>>();
    cc::mutex<pull_scratch> scratch;

    explicit sim_datagram_state(cc::shared_ptr<sim_state> l) : link(cc::move(l))
    {
        scratch.lock(
            [](pull_scratch& p)
            {
                p.bytes.resize_to_defaulted(k_scratch_slot * k_scratch_slots);
                p.records.resize_to_defaulted(k_scratch_slots);
                for (isize i = 0; i < k_scratch_slots; ++i)
                    p.records[i].buffer = cc::span<byte>(p.bytes).subspan(i * k_scratch_slot).first_n(k_scratch_slot);
            });
    }

    /// Decide the fate of one arrival, and queue what survives it.
    void impair(held_queue& q, datagram const& d)
    {
        auto const& c = link->conditions;
        if (link->draws_loss())
            return;

        auto const copies = link->draws(c.duplicate_probability) ? 2 : 1;
        for (auto k = 0; k < copies; ++k)
        {
            auto delay = link->delay_ns_for(d.size);

            // Reordering is one datagram taking a slower path: an extra delay of one to two times its own, and at least
            // a millisecond so a link with no latency still reorders.
            if (link->draws(c.reorder_probability))
            {
                auto const factor = link->rng.lock([](cc::random& r) { return f64(r.uniform(1.0f, 2.0f)); });
                auto const extra = i64(f64(delay) * factor);
                delay += extra > 1000 * 1000 ? extra : 1000 * 1000;
            }

            auto held = held_datagram{.peer = d.peer,
                                      .bytes = {},
                                      .truncated = d.truncated,
                                      .release_ns = d.received_at_ns + delay};
            held.bytes.resize_to_defaulted(d.size);
            for (isize i = 0; i < d.size; ++i)
                held.bytes[i] = d.buffer[i];

            // Insertion keeps the queue ordered; a test link holds a handful, so a shift is cheaper than a heap.
            auto at = q.items.size();
            while (at > 0 && q.items[at - 1].release_ns > held.release_ns)
                --at;
            q.items.push_back({});
            for (auto i = q.items.size() - 1; i > at; --i)
                q.items[i] = cc::move(q.items[i - 1]);
            q.items[at] = cc::move(held);
        }
    }

    /// Move everything the wrapped socket has into the held queue, deciding each one's fate on the way.
    [[nodiscard]] cc::result<cc::unit, error> pull(datagram_socket& under)
    {
        return scratch.lock(
            [&](pull_scratch& p) -> cc::result<cc::unit, error>
            {
                while (true)
                {
                    auto got = under.receive_batch(p.records);
                    if (got.has_error())
                        return cc::error(cc::move(got).error());

                    auto const n = got.value();
                    if (n > 0)
                        queue->lock(
                            [&](held_queue& q)
                            {
                                for (isize i = 0; i < n; ++i)
                                    impair(q, p.records[i]);
                            });

                    if (n < p.records.size())
                        return cc::unit{};
                }
            });
    }

    /// Wake a parked wait if something is due, and otherwise make sure a timer will look again when something is.
    static void settle(cc::shared_ptr<sim_datagram_state> const& self)
    {
        auto& io = self->link->io;
        auto const now = io.time_source().now_ns();
        auto timer_in_ms = i64(-1);

        auto* const waiting = self->queue->lock(
            [&](held_queue& q) -> impl::io_operation*
            {
                if (q.parked == nullptr)
                    return nullptr;

                if (q.has_due(now))
                {
                    auto* const op = q.parked;
                    q.parked = nullptr;
                    return op;
                }

                // Only a waiter needs a timer: a caller that polls reads the release times itself.
                if (!q.items.empty() && (q.timer_at_ns == 0 || q.items[0].release_ns < q.timer_at_ns))
                {
                    q.timer_at_ns = q.items[0].release_ns;
                    timer_in_ms = (q.timer_at_ns - now + 999'999) / 1'000'000;
                }
                return nullptr;
            });

        if (waiting != nullptr)
            io.signal(waiting);

        if (timer_in_ms >= 0)
            impl::run_after(io, timer_in_ms,
                            [self]
                            {
                                auto const now = self->link->io.time_source().now_ns();
                                self->queue->lock(
                                    [now](held_queue& q)
                                    {
                                        // Only the timer the queue is counting on clears it; an older one that was
                                        // overtaken by an earlier release leaves the newer one in place.
                                        if (q.timer_at_ns != 0 && q.timer_at_ns <= now)
                                            q.timer_at_ns = 0;
                                    });
                                settle(self);
                            });
    }
};

/// A datagram socket over a simulated link.
class simulated_datagram_backend final : public datagram_backend
{
public:
    simulated_datagram_backend(cc::shared_ptr<sim_datagram_state> s, cc::unique_ptr<datagram_socket> under)
      : _state(cc::move(s)), _under(cc::move(under))
    {
        // Arrivals are pulled as they happen, so a release time counts from when the datagram really arrived rather
        // than from whenever the caller next polled.
        // No cycle: the state never owns the wrapped socket, and the wrapped socket's destructor clears this callback.
        // A failed pull is left for the caller's next `receive_batch` to report.
        _under->on_arrival(
            [s = _state](datagram_socket& u)
            {
                (void)s->pull(u);
                sim_datagram_state::settle(s);
            });
    }

    [[nodiscard]] cc::result<isize, error> receive_batch(cc::span<datagram> records) override
    {
        if (auto pulled = _state->pull(*_under); pulled.has_error())
            return cc::error(cc::move(pulled).error());

        auto const now = _state->link->io.time_source().now_ns();
        return _state->queue->lock(
            [&](held_queue& q) -> isize
            {
                auto filled = isize(0);
                while (filled < records.size() && filled < q.items.size() && q.items[filled].release_ns <= now)
                {
                    auto const& h = q.items[filled];
                    auto& r = records[filled];
                    auto const n = cc::min(h.bytes.size(), r.buffer.size());
                    for (isize i = 0; i < n; ++i)
                        r.buffer[i] = h.bytes[i];
                    r.size = n;
                    r.peer = h.peer;
                    r.received_at_ns = h.release_ns;
                    r.truncated = h.truncated || n < h.bytes.size();
                    ++filled;
                }

                for (isize i = filled; i < q.items.size(); ++i)
                    q.items[i - filled] = cc::move(q.items[i]);
                for (isize i = 0; i < filled; ++i)
                    q.items.remove_back();
                return filled;
            });
    }

    [[nodiscard]] cc::result<isize, error> send_batch(cc::span<outgoing_datagram const> packets) override
    {
        return _under->send_batch(packets);
    }

    [[nodiscard]] cc::shared_async<cc::unit> wait_readable(cancel_token const& token) override
    {
        auto& io = _state->link->io;

        auto op = cc::make_unique<sim_readable_op>();
        op->kind = impl::io_op_kind::manual;
        op->queue = _state->queue;

        auto promise = cc::make_async_manual<cc::unit>();
        op->promise = promise;

        auto* const raw = op.get();
        raw->self = cc::move(op);

        // Submit, then park, then let `settle` decide between signalling now and a timer -- the virtual receive's order.
        auto in_flight = io.submit(raw);
        _state->queue->lock([raw](held_queue& q) { q.parked = raw; });
        sim_datagram_state::settle(_state);

        raw->cancellation.attach(cc::move(in_flight), token);
        return promise;
    }

    [[nodiscard]] endpoint local() const override { return _under->local(); }
    [[nodiscard]] io_system& io() const override { return _state->link->io; }

    ~simulated_datagram_backend() override
    {
        // First, so no arrival callback is still running into the state below.
        _under = {};
    }

private:
    cc::shared_ptr<sim_datagram_state> _state;
    cc::unique_ptr<datagram_socket> _under;
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

io_system& simulated_transport::io() const
{
    return _state->io;
}

cc::result<cc::unique_ptr<stream_listener>, error> simulated_transport::listen(endpoint const& where,
                                                                               tcp_listen_options const& options)
{
    auto under = _state->under.listen(where, options);
    if (under.has_error())
        return cc::error(cc::move(under).error());

    return cc::make_unique<stream_listener>(std::make_unique<simulated_listener>(_state, cc::move(under).value()));
}

cc::shared_async<cc::shared_ptr<stream_connection>> simulated_transport::connect(endpoint const& where,
                                                                                 deadline d,
                                                                                 tcp_options const& options,
                                                                                 cancel_token const& token)
{
    using handle = cc::shared_ptr<stream_connection>;

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
cc::result<cc::unique_ptr<datagram_socket>, error> simulated_transport::bind_datagram(endpoint const& where,
                                                                                      datagram_options const& options)
{
    auto under = _state->under.bind_datagram(where, options);
    if (under.has_error())
        return cc::error(cc::move(under).error());

    return cc::make_unique<datagram_socket>(std::make_unique<simulated_datagram_backend>(
        cc::make_shared<sim_datagram_state>(_state), cc::move(under).value()));
}
} // namespace cnet
