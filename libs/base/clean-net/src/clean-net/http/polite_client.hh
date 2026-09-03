#pragma once

#include <clean-core/memory/unique_ptr.hh>
#include <clean-net/http/http_client.hh>

/// Not hammering somebody else's server, and trying again when it is worth trying again.
///
/// **"Rate limiting" names three unrelated mechanisms, and only one of them is here.**
/// *Politeness* is not overwhelming a remote host, per host, at request admission -- this.
/// *Shaping* is not saturating a link, per transfer, at the socket; not in scope.
/// *Protection* is refusing work that arrives too fast, per inbound peer, server-side; not in scope either.
///
/// **Retries travel with the rate limit, and that is why they are in the same object.**
/// A retry policy without one is how a transient failure becomes a self-inflicted denial of service: the server that
/// failed because it was overloaded is the one that now gets three times the traffic.

/// How much traffic one remote host is allowed, and what happens when it says no.
struct cnet::host_policy
{
    /// Requests per second, over a token bucket; 0 means no limit.
    f32 requests_per_second = 0;

    /// How many requests may go out at once after an idle period.
    i32 burst = 8;

    /// How many requests may be in flight to one host.
    ///
    /// Usually what actually protects a remote server: a concurrency cap bounds the work it is doing, where a rate
    /// limit only bounds how often it is asked to start more.
    i32 max_concurrent_requests = 6;

    /// Wait as long as a `429` or `503` asks before trying again.
    /// A 429 means wait, not retry harder.
    bool honor_retry_after = true;

    /// How many times a failed request is tried again.
    ///
    /// **Only idempotent methods by default**: sending GET twice is what the caller asked for, and sending POST
    /// twice is a second order.
    i32 max_retries = 3;

    /// Only ever retried before a single response byte reached the caller's sink -- after that, repeating the
    /// request would deliver the body twice rather than once.
    f32 backoff_base_ms = 250;

    /// How much the backoff varies, as a fraction.
    /// Without it a hundred clients that failed together retry together, which is the thundering herd the backoff
    /// was supposed to prevent.
    f32 backoff_jitter = 0.3f;

    /// Retry a non-idempotent method too.
    /// Off by default, and a caller who knows their POST is safe to repeat can say so.
    bool retry_non_idempotent = false;
};

/// Rate limiting and retries over any other client.
///
/// A decorator rather than something inside the native client, because it is policy rather than protocol: the same
/// rules apply to a request that goes out over a browser's `fetch`.
class cnet::polite_http_client final : public cnet::http_client
{
public:
    /// The buckets and counters behind the policy, named here only because the request state machine holds one.
    struct state;

    polite_http_client(http_client& under, io_system& io);
    polite_http_client(http_client& under, io_system& io, host_policy const& policy);

    /// Whatever the client underneath can do: waiting before a request does not change what the request can be.
    [[nodiscard]] http_level level() const override;

    /// Wait until this host may be asked, send, and try again if it is worth trying again.
    ///
    /// The request's deadline covers the waiting too -- a request that spends its whole budget queued behind a rate
    /// limit fails rather than being sent late.
    [[nodiscard]] cc::shared_async<http_response_head> send_streaming(http_request request,
                                                                      body_sink sink,
                                                                      request_options const& options,
                                                                      cancel_token const& token) override;

    /// How many requests are in flight to `host`, for a test and for diagnostics.
    [[nodiscard]] i32 in_flight(cc::string_view host) const;

    polite_http_client(polite_http_client const&) = delete;
    polite_http_client& operator=(polite_http_client const&) = delete;
    ~polite_http_client() override;

private:
    cc::unique_ptr<state> _state;
};
