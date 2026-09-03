#pragma once

#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/address/resolver.hh>
#include <clean-net/common/level.hh>
#include <clean-net/http/connection_pool.hh>
#include <clean-net/http/message.hh>
#include <clean-net/tls/tls.hh>

/// The HTTP client, and the seam its backends implement.
///
/// **HTTP is an interface with backends, and the transport is one of them -- not the layer underneath.**
/// A browser cannot open a socket and does have `fetch`, and wasm is a tier-1 platform, so a client written over our
/// own TCP could not exist on a platform this repo supports.
/// The consequence for this API is that the shared surface stays within what a browser `fetch` can express;
/// `cnet::http_level` is how a caller asks for more.

/// What a request is allowed to take, and to buffer.
struct cnet::request_options
{
    /// One budget for the whole request: the resolve, the connect, the handshake, and every read.
    ///
    /// Per-phase timeouts let a request to a four-address host take four times what the caller asked for, which is
    /// the difference between a bounded request and one that is merely bounded per step.
    deadline timeout = deadline::after_secs(30);

    /// Refuse a body larger than this rather than buffering it.
    ///
    /// **The number is platform-dependent and the concept is not.**
    /// About 3 GiB with 64-bit pointers, which is where somebody should stop and question their pipeline; a few
    /// hundred MiB on wasm32, whose entire linear memory is capped at 4 GiB and whose browsers fail well below that.
    /// The point of a cap is that exceeding it is a clean error a caller can handle, and a number the platform
    /// cannot honour delivers an out-of-memory abort instead.
    i64 max_body_bytes = 0;

    /// Follow 3xx responses, up to `max_redirects`.
    bool follow_redirects = true;
    i32 max_redirects = 5;

    /// What TLS is set up with, for an `https` URL.
    tls_options tls;

    /// Which family to ask the resolver for, and how long one connection attempt gets before the next starts.
    address_family_preference family = address_family_preference::race;
    i32 attempt_delay_ms = 250;

    /// Take a connection from the pool when one is there, and give it back afterwards.
    ///
    /// Off for a request that must not share a connection with anything else -- an upgrade, or one carrying
    /// credentials a caller would rather not leave on a reusable socket.
    bool reuse_connections = true;
};

/// What every HTTP backend implements.
///
/// One request in, one response out, and no promise about what carries it -- our own transport today, a browser's
/// `fetch` or a system stack later.
class cnet::http_client
{
public:
    /// How much of HTTP this backend can do.
    ///
    /// A call needing more than the backend has fails loudly rather than degrading: a dropped header is discovered
    /// in production, a refused call in the first test run.
    [[nodiscard]] virtual http_level level() const = 0;

    /// Send a request, handing the response body to `sink` as it arrives.
    ///
    /// The returned async completes when the WHOLE message is done -- the head is its value, and the body has
    /// already gone to the sink by then.
    /// A redirect that is followed never reaches the sink: its body is discarded, and only the final response's
    /// bytes are delivered.
    ///
    /// **The sink runs on the reactor thread**, and taking fewer bytes than it was offered is how a slow consumer
    /// pushes back all the way down to the socket.
    [[nodiscard]] virtual cc::shared_async<http_response_head> send_streaming(http_request request,
                                                                              body_sink sink,
                                                                              request_options const& options,
                                                                              cancel_token const& token) = 0;

    http_client() = default;
    http_client(http_client const&) = delete;
    http_client& operator=(http_client const&) = delete;
    virtual ~http_client() = default;
};

/// The client over our own transport: resolve, connect, TLS, HTTP/1.1.
///
/// **HTTP/1.1 only.**
/// HPACK, stream multiplexing and flow control are a different order of magnitude, and wanting HTTP/2 is a reason to
/// select a system backend rather than to write one.
/// The cost is throughput on many-small-requests workloads, which is not what this library is for.
///
/// **Connections are pooled**, keyed on origin, so a second request to the same server pays no handshake.
/// Reuse is speculative by nature -- a server may have closed an idle connection without anybody noticing -- so a
/// request that fails on a pooled connection before any response byte arrived is retried once on a fresh one.
/// That retry is what makes pooling safe, and it is why it is not conditional on anything.
class cnet::native_http_client final : public cnet::http_client
{
public:
    /// Over a given transport and resolver, which is how a test puts a virtual network underneath.
    native_http_client(transport& t, resolver& r, connection_pool::description const& pool = {})
      : _transport(t), _resolver(r), _pool(r.io(), pool)
    {
    }

    /// `client`, not `connection`: the socket underneath is real, and nothing here hands it to a caller yet.
    [[nodiscard]] http_level level() const override { return http_level::client; }

    [[nodiscard]] cc::shared_async<http_response_head> send_streaming(http_request request,
                                                                      body_sink sink,
                                                                      request_options const& options,
                                                                      cancel_token const& token) override;

    /// The connections this client is keeping, for a test and for diagnostics.
    [[nodiscard]] connection_pool& pool() { return _pool; }

private:
    transport& _transport;
    resolver& _resolver;
    connection_pool _pool;
};

/// A client over the platform's own sockets that owns the transport and the resolver it needs.
///
/// A named type rather than something hidden behind a factory, because `cc::unique_ptr` has no derived-to-base
/// conversion -- and because a caller holding one can still hand out the `http_client&` underneath.
class cnet::owned_http_client final : public cnet::http_client
{
public:
    /// `cnet::make_http_client` is how one is built; this needs a resolver that already exists.
    owned_http_client(io_system& io, cc::unique_ptr<resolver> r);

    [[nodiscard]] http_level level() const override;

    [[nodiscard]] cc::shared_async<http_response_head> send_streaming(http_request request,
                                                                      body_sink sink,
                                                                      request_options const& options,
                                                                      cancel_token const& token) override;

private:
    native_transport _transport;
    cc::unique_ptr<resolver> _resolver;
    native_http_client _client;
};

namespace cnet
{
/// A client over the platform's own sockets, owning everything it needs.
///
/// The one-line way in; `native_http_client`'s two-reference constructor is for a caller who already has both, and
/// for a test that wants neither to be real.
[[nodiscard]] cc::result<cc::unique_ptr<owned_http_client>, error> make_http_client(io_system& io);

/// Send a request and buffer the response.
///
/// Written over `send_streaming`, which is the primitive: this is the sink that keeps the bytes.
/// Fails with `body_too_large` rather than buffering past `request_options::max_body_bytes`.
[[nodiscard]] cc::shared_async<http_response> http_send(http_client& client,
                                                        http_request request,
                                                        request_options const& options = {},
                                                        cancel_token const& token = {});

/// GET a URL and buffer the response.
/// Fails with `invalid_argument` if the URL is not one this client can fetch.
[[nodiscard]] cc::shared_async<http_response> http_get(http_client& client,
                                                       cc::string_view url,
                                                       request_options const& options = {},
                                                       cancel_token const& token = {});
} // namespace cnet
