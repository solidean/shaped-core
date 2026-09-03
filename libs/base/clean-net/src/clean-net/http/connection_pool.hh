#pragma once

#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string_view.hh>
#include <clean-net/transport/stream.hh>

/// Connections kept open between requests, keyed on origin.
///
/// **What it is worth**: a TLS handshake is one or two round trips and a request without one is the difference
/// between a page load and a page load nobody notices.
/// Everything else about pooling -- the counts, the timeouts -- exists to keep that from turning into a connection
/// that is dead and nobody noticed.
///
/// **A pooled connection can be closed and look fine.**
/// A server may end an idle connection at any time, and nothing tells the client until a write fails or a read
/// returns nothing.
/// So reuse is always speculative: the client above retries once on a fresh connection when a pooled one fails
/// before any response byte arrived, and that retry is what makes pooling safe rather than the bookkeeping here.
class cnet::connection_pool
{
public:
    struct description
    {
        /// How many idle connections one origin may keep.
        /// Past this the oldest is closed, since a connection nobody is using costs a file descriptor and a server's
        /// patience.
        i32 max_idle_per_origin = 4;

        /// How long an idle connection is offered before it is assumed dead.
        ///
        /// Shorter than most servers' own keep-alive timeout on purpose: losing a usable connection costs one
        /// handshake, and offering a dead one costs a failed request and a retry.
        i64 idle_timeout_ms = 20'000;
    };

    /// Two constructors rather than a defaulted argument: a default of `{}` here would need the description's own
    /// defaults while this class is still being defined, which is one of the few things C++ will not do.
    explicit connection_pool(io_system& io);
    connection_pool(io_system& io, description const& desc);

    /// Take an idle connection for `origin`, or nothing.
    ///
    /// What comes back is ready to carry a request: for an https origin it is the TLS connection rather than the
    /// socket underneath, because that is what was put in.
    [[nodiscard]] cc::shared_ptr<stream_connection> try_take(cc::string_view origin);

    /// Offer a connection back once its response is finished.
    ///
    /// Ignored, and the connection closed, when it is no longer usable -- which is the caller's judgement rather
    /// than this one's: only the parser knows whether the message ended in a way that leaves the stream clean.
    void give_back(cc::string_view origin, cc::shared_ptr<stream_connection> connection, bool reusable);

    /// Close everything held.
    void clear();

    /// How many idle connections are held, for a test and for diagnostics.
    [[nodiscard]] isize idle_count() const;

    connection_pool(connection_pool const&) = delete;
    connection_pool& operator=(connection_pool const&) = delete;
    ~connection_pool();

private:
    struct state;
    cc::unique_ptr<state> _state;
};
