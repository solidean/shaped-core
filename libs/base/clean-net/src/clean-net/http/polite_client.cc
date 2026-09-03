#include "polite_client.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/async_glue.hh>

// Admission, then attempts.
//
// ADMISSION is a token bucket and a concurrency count, per host.
// A request that cannot go yet waits on a reactor timer and asks again -- so waiting costs a timer rather than a
// thread, and the wait is measured on the same clock as every deadline.
//
// RETRIES only happen where repeating the request means what the caller meant.
// That is: an idempotent method, no response byte delivered yet, retries left, the token not cancelled, and budget
// remaining.
// Anything else is a second request wearing a retry's clothes.

namespace cnet
{
namespace
{
/// What one remote host is currently owed and doing.
struct host_state
{
    cc::string host;

    /// Tokens available now, as a float because they refill continuously.
    f32 tokens = 0;
    i64 last_refill_ns = 0;

    i32 in_flight = 0;
};

/// How long to wait before asking again, when a host is not ready.
///
/// Short enough that a freed slot is taken promptly, long enough that waiting costs almost nothing: the request is
/// parked on a timer either way.
constexpr i64 k_admission_poll_ms = 5;

/// The `Retry-After` value in milliseconds, or nothing.
///
/// Only the delta-seconds form is read.
/// The HTTP-date form is legal and rare, and a client that guesses at a date it cannot parse waits for the wrong
/// length of time -- so an unparsable value falls back to the ordinary backoff instead.
[[nodiscard]] cc::optional<i64> retry_after_ms(http_response_head const& head)
{
    auto const value = head.headers.get("Retry-After");
    if (!value.has_value() || value.value().empty())
        return {};

    auto seconds = i64(0);
    for (auto const c : value.value())
    {
        if (c < '0' || c > '9')
            return {};

        seconds = seconds * 10 + (c - '0');
        if (seconds > 3600)
            return 3600 * 1000; // an hour is longer than any request here is willing to wait for
    }

    return seconds * 1000;
}

/// Whether the status is one where trying again is the right answer rather than a louder failure.
[[nodiscard]] bool is_worth_retrying(i32 status)
{
    // 429 says explicitly "later"; 502, 503 and 504 are a server or a proxy having a moment.
    return status == 429 || status == 502 || status == 503 || status == 504;
}
} // namespace

struct polite_http_client::state
{
    http_client& under;
    io_system& io;
    host_policy policy;

    cc::mutex<cc::vector<host_state>> hosts;

    /// One stream for the jitter, so a run replays from nothing at all -- the jitter is there to spread clients
    /// apart, and a fixed seed would put every process in this program back in step.
    cc::mutex<cc::random> jitter;

    state(http_client& u, io_system& s, host_policy const& p)
      : under(u), io(s), policy(p), jitter(cc::random(u64(s.time_source().now_ns())))
    {
    }

    /// Take a token and a concurrency slot for `host`, or say how long to wait before asking again.
    [[nodiscard]] cc::optional<i64> try_admit(cc::string_view host)
    {
        auto const now = io.time_source().now_ns();

        return hosts.lock(
            [&](cc::vector<host_state>& all) -> cc::optional<i64>
            {
                host_state* entry = nullptr;
                for (auto& h : all)
                    if (cc::string_view(h.host) == host)
                    {
                        entry = &h;
                        break;
                    }

                if (entry == nullptr)
                {
                    all.push_back(
                        {.host = cc::string(host), .tokens = f32(policy.burst), .last_refill_ns = now, .in_flight = 0});
                    entry = &all[all.size() - 1];
                }

                if (policy.max_concurrent_requests > 0 && entry->in_flight >= policy.max_concurrent_requests)
                    return k_admission_poll_ms;

                if (policy.requests_per_second > 0)
                {
                    auto const elapsed_secs = f32(f64(now - entry->last_refill_ns) / 1e9);
                    entry->tokens += elapsed_secs * policy.requests_per_second;
                    entry->last_refill_ns = now;

                    if (entry->tokens > f32(policy.burst))
                        entry->tokens = f32(policy.burst);

                    if (entry->tokens < 1)
                    {
                        // Exactly as long as the missing fraction of a token takes to arrive.
                        auto const missing = 1.0f - entry->tokens;
                        auto const wait_ms = i64(1000.0f * missing / policy.requests_per_second) + 1;
                        return wait_ms;
                    }

                    entry->tokens -= 1;
                }

                ++entry->in_flight;
                return {};
            });
    }

    void release(cc::string_view host)
    {
        hosts.lock(
            [&](cc::vector<host_state>& all)
            {
                for (auto& h : all)
                    if (cc::string_view(h.host) == host && h.in_flight > 0)
                    {
                        --h.in_flight;
                        return;
                    }
            });
    }

    [[nodiscard]] i64 backoff_ms(i32 attempt)
    {
        auto delay = f64(policy.backoff_base_ms);
        for (auto i = 0; i < attempt && i < 10; ++i)
            delay *= 2;

        if (policy.backoff_jitter > 0)
        {
            auto const spread = jitter.lock([&](cc::random& r)
                                            { return f64(r.uniform(-policy.backoff_jitter, policy.backoff_jitter)); });
            delay *= 1.0 + spread;
        }

        return delay < 0 ? 0 : i64(delay);
    }
};

namespace
{
/// One request as this client runs it: admission, an attempt, and possibly another.
struct polite_request
{
    polite_http_client::state* owner = nullptr;

    http_request request;
    request_options options;
    cancel_token token;

    /// The caller's sink, called through a fresh forwarding one on every attempt.
    cc::shared_ptr<body_sink> sink;

    cc::shared_async<http_response_head> promise;

    i64 deadline_ns = 0;
    i32 attempt = 0;
    bool holding_slot = false;

    /// Whether anything reached the caller's sink, which is what makes a retry a repeat rather than a retry.
    bool delivered_body = false;

    [[nodiscard]] deadline remaining() const
    {
        if (deadline_ns <= 0)
            return deadline::never();

        auto const left_ms = (deadline_ns - owner->io.time_source().now_ns()) / (1000 * 1000);
        return deadline::after_ms(left_ms > 0 ? left_ms : 0);
    }

    [[nodiscard]] bool out_of_time() const
    {
        return deadline_ns > 0 && owner->io.time_source().now_ns() >= deadline_ns;
    }
};

void admit_then_send(cc::shared_ptr<polite_request> const& rq);

void release_slot(cc::shared_ptr<polite_request> const& rq)
{
    if (!rq->holding_slot)
        return;
    rq->holding_slot = false;
    rq->owner->release(rq->request.target.host);
}

void finish_with_head(cc::shared_ptr<polite_request> const& rq, cc::shared_async<http_response_head> const& settled)
{
    release_slot(rq);

    if (settled->has_error())
        rq->promise->push_error(settled->propagate_error());
    else
        rq->promise->push_value(settled->take_value());
}

void fail_now(cc::shared_ptr<polite_request> const& rq, error e)
{
    release_slot(rq);
    rq->promise->push_error(to_async_error(cc::move(e)));
}

/// Whether this outcome may be tried again at all.
[[nodiscard]] bool may_retry(cc::shared_ptr<polite_request> const& rq)
{
    if (rq->attempt >= rq->owner->policy.max_retries)
        return false;
    if (rq->delivered_body || rq->token.is_cancelled() || rq->out_of_time())
        return false;

    // Idempotence is the whole question: repeating a POST is a second order rather than the same one.
    return rq->owner->policy.retry_non_idempotent || is_idempotent(rq->request.method);
}

void retry_after_delay(cc::shared_ptr<polite_request> const& rq, i64 delay_ms)
{
    ++rq->attempt;
    release_slot(rq);

    CC_LOG_TRACE("retrying {} in {} ms (attempt {})", rq->request.target.origin(), delay_ms, rq->attempt);

    impl::run_after(rq->owner->io, delay_ms, [rq] { admit_then_send(rq); });
}

void send_now(cc::shared_ptr<polite_request> const& rq)
{
    auto attempt_options = rq->options;
    attempt_options.timeout = rq->remaining();

    // A fresh forwarding sink per attempt, over the caller's one: a retry that has delivered nothing can start
    // again, and one that has delivered something never gets here.
    auto sink = rq->sink;
    auto* const flag = &rq->delivered_body;

    auto sent = rq->owner->under.send_streaming(
        rq->request,
        [sink, flag](cc::span<byte const> chunk) -> isize
        {
            auto const taken = (*sink)(chunk);
            if (taken > 0)
                *flag = true;
            return taken;
        },
        attempt_options, rq->token);

    impl::when_ready(sent,
                     [rq](cc::shared_async<http_response_head> const& settled)
                     {
                         if (settled->has_error())
                         {
                             if (!may_retry(rq))
                             {
                                 finish_with_head(rq, settled);
                                 return;
                             }
                             retry_after_delay(rq, rq->owner->backoff_ms(rq->attempt));
                             return;
                         }

                         auto const& head = settled->value();
                         if (!is_worth_retrying(head.status) || !may_retry(rq))
                         {
                             finish_with_head(rq, settled);
                             return;
                         }

                         // A 429 means wait, not retry harder, and the server is the one who knows how long.
                         auto const asked
                             = rq->owner->policy.honor_retry_after ? retry_after_ms(head) : cc::optional<i64>();
                         retry_after_delay(rq, asked.has_value() ? asked.value() : rq->owner->backoff_ms(rq->attempt));
                     });
}

void admit_then_send(cc::shared_ptr<polite_request> const& rq)
{
    if (rq->token.is_cancelled())
    {
        fail_now(
            rq, {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the operation was cancelled")});
        return;
    }

    // The budget covers the queueing: a request that spent it all waiting for a rate limit failed, and sending it
    // late would be answering a question nobody is still asking.
    if (rq->out_of_time())
    {
        fail_now(rq, {.code = error_code::timed_out,
                      .native_code = 0,
                      .message = cc::string("the request ran out of time waiting for its turn")});
        return;
    }

    auto const wait_ms = rq->owner->try_admit(rq->request.target.host);
    if (wait_ms.has_value())
    {
        impl::run_after(rq->owner->io, wait_ms.value(), [rq] { admit_then_send(rq); });
        return;
    }

    rq->holding_slot = true;
    send_now(rq);
}
} // namespace

polite_http_client::polite_http_client(http_client& under, io_system& io) : polite_http_client(under, io, host_policy())
{
}

polite_http_client::polite_http_client(http_client& under, io_system& io, host_policy const& policy)
  : _state(cc::make_unique<state>(under, io, policy))
{
}

polite_http_client::~polite_http_client() = default;

http_level polite_http_client::level() const
{
    return _state->under.level();
}

i32 polite_http_client::in_flight(cc::string_view host) const
{
    return _state->hosts.lock(
        [&](cc::vector<host_state> const& all)
        {
            for (auto const& h : all)
                if (cc::string_view(h.host) == host)
                    return h.in_flight;
            return 0;
        });
}

cc::shared_async<http_response_head> polite_http_client::send_streaming(http_request request,
                                                                        body_sink sink,
                                                                        request_options const& options,
                                                                        cancel_token const& token)
{
    auto rq = cc::make_shared<polite_request>();
    rq->owner = _state.get();
    rq->request = cc::move(request);
    rq->options = options;
    rq->token = token;
    rq->sink = cc::make_shared<body_sink>(cc::move(sink));
    rq->promise = cc::make_async_manual<http_response_head>();
    rq->deadline_ns = deadline_to_absolute(_state->io, options.timeout);

    admit_then_send(rq);
    return rq->promise;
}
} // namespace cnet
