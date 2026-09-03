#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/http/message.hh>
#include <clean-net/transport/stream.hh>

/// A loopback HTTP server, for a debug UI this process serves to a browser.
///
/// **It is not hardened for hostile input, and that is a design decision rather than an omission.**
/// What separates a web server from this is almost entirely work about hostility: slow-read attacks, request
/// smuggling between a proxy and a backend, connection exhaustion, path traversal, compression bombs, and a threat
/// model where every byte is attacker-controlled.
/// A server bound to `127.0.0.1` faces none of that, because the only thing that can reach it is a process already
/// running as the same user.
///
/// **A server bound to `0.0.0.0` faces all of it**, and the difference between the two is one line of configuration
/// that somebody will change -- so binding beyond loopback is a named boolean that logs a warning, rather than a
/// bind address that happens to say `0.0.0.0`.
///
/// **No server-side TLS.**
/// Browsers treat `http://localhost` as a secure context, so a local debug UI needs no certificate, no self-signed
/// trust prompt, and none of the code that would go with them.
/// That removes the single largest chunk of server work.
///
/// What it refuses to grow into: virtual hosts, TLS termination, HTTP/2, proxying, authentication frameworks, a
/// plugin architecture.
/// Each arrives as a small reasonable request, and together they are a web server -- which would be a different
/// library rather than a bigger version of this one.

/// How a server is built.
struct cnet::http_server_description
{
    /// 0 asks the OS for a free one, and `local()` says which it got.
    i32 port = 0;

    /// Bind every interface rather than loopback.
    ///
    /// **This server is not hardened for hostile input.**
    /// A distinct field rather than a bind address, because the point is that the choice is visible; setting it logs
    /// a warning through `cc::rec` at startup.
    bool bind_all_interfaces = false;

    /// All of one request's headers together.
    isize max_header_bytes = 16 * 1024;

    isize max_header_count = 100;

    /// A request body larger than this is refused with `413` rather than buffered.
    isize max_body_bytes = 8 * 1024 * 1024;

    /// How many connections may be open at once; one that arrives past the limit is closed immediately.
    i32 max_connections = 64;

    /// How long one read of a request may take.
    ///
    /// This is what makes slow-read attacks uninteresting: a client that sends a request one byte per minute is
    /// closed rather than held.
    i32 request_timeout_ms = 5'000;

    /// How long a kept-alive connection may sit idle before it is closed.
    i32 idle_timeout_ms = 30'000;

    /// The largest WebSocket message an upgraded connection will reassemble.
    isize max_websocket_message_bytes = 8 * 1024 * 1024;

    /// The largest file `serve_directory` will read.
    /// A bigger one is a 500 and a warning, because reading it would block the reactor for as long as it takes.
    isize max_static_file_bytes = 16 * 1024 * 1024;
};

/// A request, as a handler sees it.
struct cnet::http_server_request
{
    http_method method = http_method::get;

    /// The target exactly as it arrived, still percent-encoded.
    cc::string target;

    /// The part before `?`, still percent-encoded.
    /// This is what routes are matched against.
    cc::string path;

    /// The part after `?`, without it.
    cc::string query;

    http_headers headers;
    cc::vector<byte> body;

    /// Who sent it.
    endpoint peer;

    [[nodiscard]] cc::string_view body_text() const
    {
        return cc::string_view(reinterpret_cast<char const*>(body.data()), body.size());
    }
};

/// A response body written over time, for something whose length nobody knows when the handler returns.
///
/// The bytes go out as HTTP/1.1 chunks, which is what lets the connection stay alive afterwards and what lets a
/// client tell a finished stream from a truncated one.
/// An HTTP/1.0 client gets the same bytes without the chunk framing, delimited by the close -- that is all HTTP/1.0
/// has, and it is why chunked exists.
///
/// **Keep the `shared_ptr` for as long as you are writing.**
/// Dropping the last one finishes the stream cleanly, which is the right end for a handler that decides it has
/// nothing more to say.
class cnet::http_response_stream
{
public:
    /// Append one chunk.
    ///
    /// Chunks queue rather than interleaving on the wire, and an empty one is dropped: a zero-length chunk is what
    /// ENDS a chunked body, so writing one would truncate the response.
    [[nodiscard]] cc::shared_async<cc::unit> write(cc::span<byte const> chunk);

    [[nodiscard]] cc::shared_async<cc::unit> write_text(cc::string_view chunk);

    /// End the body, after everything already queued has gone out.
    /// The destructor does this too, so an abandoned stream ends rather than hangs.
    void finish();

    [[nodiscard]] bool is_open() const;

    explicit http_response_stream(cc::shared_ptr<struct response_stream_state> state);
    http_response_stream(http_response_stream const&) = delete;
    http_response_stream& operator=(http_response_stream const&) = delete;
    ~http_response_stream();

private:
    cc::shared_ptr<struct response_stream_state> _state;
};

namespace cnet
{
/// What a streaming route does once the response head is out.
using response_stream_handler = cc::unique_function<void(cc::shared_ptr<http_response_stream> body)>;
} // namespace cnet

/// What a handler answers with.
struct cnet::http_server_response
{
    i32 status = 200;
    http_headers headers;
    cc::vector<byte> body;

    /// A text response, with the content type set.
    [[nodiscard]] static http_server_response text(cc::string_view body,
                                                   cc::string_view content_type = "text/plain; charset=utf-8",
                                                   i32 status = 200);

    /// Bytes with a content type of their own.
    [[nodiscard]] static http_server_response bytes(cc::vector<byte> body, cc::string_view content_type, i32 status = 200);

    /// A status and nothing else.
    [[nodiscard]] static http_server_response empty(i32 status);

    /// A body written over time instead of carried here.
    ///
    /// `on_open` is called once the head has gone out, with the stream to write into.
    /// A `HEAD` request gets the head and no call at all, since there is nothing for the body to be.
    [[nodiscard]] static http_server_response stream(cc::string_view content_type,
                                                     response_stream_handler on_open,
                                                     i32 status = 200);

    /// Set by `stream`, and what makes this response a streaming one.
    response_stream_handler on_open;
};

namespace cnet
{
/// What a route does with a request.
///
/// **It runs on the reactor thread**, like every completion in this library: do no blocking work here, and hand
/// anything slow to somewhere else.
using route_handler = cc::unique_function<http_server_response(http_server_request const&)>;

/// What a WebSocket route does with a connection that finished upgrading.
///
/// **The handler must keep the WebSocket alive**: the server holds no reference to it, so one that is dropped closes,
/// which is the right behaviour for a route that decides it does not want the connection after all.
using websocket_handler
    = cc::unique_function<void(cc::shared_ptr<websocket> connection, http_server_request const& request)>;
} // namespace cnet

/// The server itself.
class cnet::http_server
{
public:
    /// Listen on the platform's own sockets.
    /// Fails with `unsupported` on wasm, where a program cannot listen at all.
    [[nodiscard]] static cc::result<cc::unique_ptr<http_server>, error> try_create(io_system& io,
                                                                                   http_server_description const& desc);
    [[nodiscard]] static cc::result<cc::unique_ptr<http_server>, error> try_create(io_system& io);

    /// Listen on a given transport, which is how a test puts a virtual network underneath.
    [[nodiscard]] static cc::result<cc::unique_ptr<http_server>, error> try_create(transport& t,
                                                                                   http_server_description const& desc);
    [[nodiscard]] static cc::result<cc::unique_ptr<http_server>, error> try_create(transport& t);

    /// What it actually bound to, port included.
    [[nodiscard]] endpoint local() const;

    /// Answer `method` requests for `pattern`.
    ///
    /// A pattern ending in `*` matches any path that begins with the rest of it; anything else is an exact match.
    /// Routes are tried in the order they were added, so a specific one added before a wildcard wins.
    ///
    /// A path nothing matches is a 404; a path that matches only under another method is a 405, because those are
    /// different facts and a client can act on the difference.
    void route(http_method method, cc::string_view pattern, route_handler handler);

    /// Upgrade `pattern` to a WebSocket instead of answering it.
    ///
    /// Matched exactly like `route`, and checked before the ordinary routes -- so a path that is both is a WebSocket
    /// when the request asks to upgrade and an ordinary response when it does not.
    /// A request that matches and is not a well-formed upgrade gets a 400 rather than falling through, because a
    /// client that meant to upgrade learns nothing from a 404.
    ///
    /// No subprotocol is ever selected: this server answers without a `Sec-WebSocket-Protocol`, which every client
    /// must accept.
    void websocket_route(cc::string_view pattern, websocket_handler handler);

    /// Serve the files under `root` beneath `url_prefix`, for `GET` and `HEAD`.
    ///
    /// **The confinement is the feature.**
    /// A request path is percent-decoded and then refused outright if it carries `..`, `.`, an empty segment, a
    /// backslash, a colon or a NUL -- rather than being resolved and checked afterwards.
    /// Refusing the escape token is stronger than resolving it, because there is then no canonical form for two
    /// implementations to disagree about.
    ///
    /// **A symlink under `root` pointing outside it is NOT caught**, because clean-core has no path resolution to
    /// catch it with.
    /// On a loopback dev server that is a link the same user made; anywhere else it is a hole.
    ///
    /// A directory resolves to its `index.html`, a missing file is a 404, and a file over
    /// `max_static_file_bytes` is a 500.
    void serve_directory(cc::string_view url_prefix, cc::string_view root);

    /// Stop accepting and close what is open.
    ///
    /// Everything in flight ends through the server's own cancellation token rather than through a deadline nobody
    /// set, which is what makes shutdown immediate rather than eventual.
    void stop();

    /// How many connections are open, for a test and for diagnostics.
    [[nodiscard]] i32 open_connections() const;

    /// How many requests have been answered.
    [[nodiscard]] i64 requests_handled() const;

    explicit http_server(cc::unique_ptr<struct http_server_state> state);
    http_server(http_server const&) = delete;
    http_server& operator=(http_server const&) = delete;
    ~http_server();

private:
    cc::unique_ptr<struct http_server_state> _state;
};
