#include "http_server.hh"

#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/uri.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/http/impl/http1.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/ws/impl/websocket_internal.hh>
#include <clean-net/ws/impl/ws_frame.hh>
#include <clean-net/ws/websocket.hh>

// One accept loop, and one state machine per connection.
//
// A connection reads a request, answers it, and either reads another or goes away -- and every one of those steps is
// a completion rather than a wait, so a server with sixty connections has sixty small objects and no threads of its
// own.
//
// THE LIMITS ARE NOT A SAFETY CLAIM.
// They exist because a dev server eventually gets exposed by somebody with an SSH tunnel or a container port
// mapping, and they are the difference between "unsuitable for hostile input" and "trivially crashable".
// They cost almost nothing, and they are not the same thing as being safe to expose.

namespace cnet
{
namespace
{
constexpr isize k_read_chunk = 16 * 1024;

struct route_entry
{
    http_method method = http_method::get;
    cc::string pattern;

    /// True when the pattern ended in `*`, in which case `pattern` is what comes before it.
    bool is_prefix = false;

    route_handler handler;
};

struct websocket_route_entry
{
    cc::string pattern;
    websocket_handler handler;
};
} // namespace

/// Everything the server owns, shared with every connection it accepted.
struct http_server_state
{
    /// Set only when the server made its own transport, which the io_system overloads do.
    cc::unique_ptr<native_transport> owned_transport;

    transport& t;
    http_server_description desc;

    cc::unique_ptr<stream_listener> listener;

    /// What it bound to, kept so `local()` still answers once the listener has been let go.
    endpoint bound;

    cancel_token token = cancel_token::create();

    cc::mutex<cc::vector<route_entry>> routes;
    cc::mutex<cc::vector<websocket_route_entry>> websocket_routes;

    cc::atomic<i32> open_connections = 0;
    cc::atomic<i64> requests_handled = 0;
    cc::atomic<bool> stopped = false;

    http_server_state(cc::unique_ptr<native_transport> owned, transport& transport_ref, http_server_description const& d)
      : owned_transport(cc::move(owned)), t(transport_ref), desc(d)
    {
    }
};

/// One connection, and the request it is in the middle of.
struct session
{
    http_server_state* server = nullptr;
    cc::shared_ptr<stream_connection> connection;

    impl::http1_parser parser;
    cc::vector<byte> read_buffer;
    cc::vector<byte> unparsed;
    cc::vector<byte> body;

    /// The serialized response head, which must outlive the send that carries it.
    cc::string head_bytes;
    cc::vector<byte> response_body;

    bool body_too_large = false;
    bool closed = false;

    /// True while waiting for the FIRST byte of a request, which is the idle keep-alive wait rather than a slow read.
    bool idle = true;
};

/// One chunk waiting its turn on the wire.
struct outgoing_chunk
{
    cc::string bytes;
    cc::shared_async<cc::unit> promise;

    /// The last one, after which the session goes back to reading or closes.
    bool ends_the_body = false;
};

/// What a streaming response owns while it is being written.
struct response_stream_state
{
    cc::shared_ptr<session> owner;

    /// False for an HTTP/1.0 client, which has no chunked encoding and gets a body delimited by the close.
    bool chunked = true;

    /// Whether the connection survives the response, which only a chunked body allows.
    bool keep_alive = true;

    cc::vector<outgoing_chunk> outbox;
    bool writing = false;
    bool finished = false;
    bool failed = false;
};

namespace
{
void accept_one(http_server_state* server);
void read_request(cc::shared_ptr<session> const& s);
void pump_stream(cc::shared_ptr<response_stream_state> const& body);

void close_session(cc::shared_ptr<session> const& s)
{
    if (s->closed)
        return;
    s->closed = true;

    if (s->connection.is_valid())
        s->connection->close();
    s->connection = {};

    s->server->open_connections.fetch_sub(1);
}

/// Split a target into the path and the query, without decoding either.
[[nodiscard]] cc::string_view path_of(cc::string_view target)
{
    auto const question = target.find('?');
    return question < 0 ? target : target.subview({.offset = 0, .size = question});
}

[[nodiscard]] cc::string_view query_of(cc::string_view target)
{
    auto const question = target.find('?');
    return question < 0 ? cc::string_view() : target.subview(question + 1);
}

/// Find the route for this request, and say whether the path matched under a different method.
struct route_match
{
    route_handler* handler = nullptr;
    bool path_exists = false;
};

[[nodiscard]] route_match find_route(http_server_state* server, http_method method, cc::string_view path)
{
    return server->routes.lock(
        [&](cc::vector<route_entry>& all)
        {
            auto match = route_match();

            for (auto& entry : all)
            {
                auto const matches = entry.is_prefix ? path.starts_with(cc::string_view(entry.pattern))
                                                     : path == cc::string_view(entry.pattern);
                if (!matches)
                    continue;

                // A path that exists under another method is a 405 rather than a 404: they are different facts, and
                // a client can act on the difference.
                match.path_exists = true;
                if (entry.method == method)
                {
                    match.handler = &entry.handler;
                    return match;
                }
            }
            return match;
        });
}

[[nodiscard]] http_server_request make_request(cc::shared_ptr<session> const& s)
{
    auto const& head = s->parser.request();

    auto request = http_server_request();
    request.method = head.method;
    request.target = head.target;
    request.path = cc::string(path_of(head.target));
    request.query = cc::string(query_of(head.target));
    request.headers = head.headers;
    request.body = cc::move(s->body);
    request.peer = s->connection->peer();
    return request;
}

[[nodiscard]] http_server_response answer(cc::shared_ptr<session> const& s)
{
    if (s->body_too_large)
        return http_server_response::empty(413);

    auto const& head = s->parser.request();
    auto const path = path_of(head.target);

    auto const found = find_route(s->server, head.method, path);
    if (found.handler == nullptr)
        return http_server_response::empty(found.path_exists ? 405 : 404);

    auto request = make_request(s);

    s->server->requests_handled.fetch_add(1);
    return (*found.handler)(request);
}

/// Whether a comma-separated header list carries `token`.
///
/// `Connection` is such a list and a browser routinely sends `keep-alive, Upgrade`, so comparing the whole value
/// against one word rejects real clients.
[[nodiscard]] bool header_lists_token(cc::optional<cc::string_view> value, cc::string_view token)
{
    if (!value.has_value())
        return false;

    auto rest = value.value();
    while (!rest.empty())
    {
        auto const comma = rest.find(',');
        auto const piece = comma < 0 ? rest : rest.subview({.offset = 0, .size = comma});
        rest = comma < 0 ? cc::string_view() : rest.subview(comma + 1);

        auto trimmed = piece;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed = trimmed.subview(1);
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed = trimmed.subview({.offset = 0, .size = trimmed.size() - 1});

        if (header_names_equal(trimmed, token))
            return true;
    }
    return false;
}

/// Hand this connection to the WebSocket layer, if a route asked for that and the request is one.
///
/// Returns whether the session has stopped being the HTTP loop's to drive, which covers both a successful upgrade and
/// the 400 a malformed one gets.
[[nodiscard]] bool try_upgrade(cc::shared_ptr<session> const& s)
{
    auto const& head = s->parser.request();
    auto const path = path_of(head.target);

    auto* const handler = s->server->websocket_routes.lock(
        [&](cc::vector<websocket_route_entry>& all) -> websocket_handler*
        {
            for (auto& entry : all)
                if (path == cc::string_view(entry.pattern))
                    return &entry.handler;
            return nullptr;
        });

    if (handler == nullptr)
        return false;

    auto const key = head.headers.get("Sec-WebSocket-Key");
    auto const version = head.headers.get("Sec-WebSocket-Version");

    auto const well_formed = head.method == http_method::get                               //
                          && header_lists_token(head.headers.get("Upgrade"), "websocket")  //
                          && header_lists_token(head.headers.get("Connection"), "upgrade") //
                          && version.has_value() && version.value() == "13"                //
                          && key.has_value();

    auto accept
        = well_formed
            ? impl::websocket_accept_key(key.value())
            : cc::result<cc::string, error>(cc::error(
                  error{.code = error_code::protocol_error, .native_code = 0, .message = cc::string("not an upgrade")}));

    if (accept.has_error())
    {
        // A client that asked for a path only WebSockets live at, and did not ask correctly, learns that here rather
        // than from a 404 about a path that does exist.
        auto bad = impl::write_response_head(400, {}, {}, 0, false);
        if (bad.has_error())
        {
            close_session(s);
            return true;
        }

        s->head_bytes = cc::move(bad).value();
        auto const span = cc::span<byte const>(reinterpret_cast<byte const*>(s->head_bytes.data()), s->head_bytes.size());
        impl::when_ready(
            s->connection->send(span, deadline::after_ms(s->server->desc.request_timeout_ms), s->server->token),
            [s](cc::shared_async<cc::unit> const&) { close_session(s); });
        return true;
    }

    // Written by hand rather than through `write_response_head`, which would add a Content-Length: a 101 has no body,
    // and every byte after its blank line already belongs to the WebSocket.
    s->head_bytes = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: ";
    s->head_bytes += cc::move(accept).value();
    s->head_bytes += "\r\n\r\n";

    auto request = make_request(s);
    s->server->requests_handled.fetch_add(1);

    auto const span = cc::span<byte const>(reinterpret_cast<byte const*>(s->head_bytes.data()), s->head_bytes.size());
    impl::when_ready(s->connection->send(span, deadline::after_ms(s->server->desc.request_timeout_ms), s->server->token),
                     [s, handler, request = cc::move(request)](cc::shared_async<cc::unit> const& sent) mutable
                     {
                         if (sent->has_error())
                         {
                             close_session(s);
                             return;
                         }

                         // The session hands the connection over rather than closing it, so the count it keeps stops here: what is
                         // open from now on is a WebSocket rather than an HTTP connection.
                         s->closed = true;
                         s->server->open_connections.fetch_sub(1);

                         auto connection = cc::move(s->connection);
                         s->connection = {};

                         auto ws = impl::adopt_websocket(
                             s->server->t.io(), {.connection = cc::move(connection),
                                                 .is_client = false,
                                                 .leftover = cc::move(s->unparsed),
                                                 .max_message_bytes = s->server->desc.max_websocket_message_bytes,
                                                 .ping_interval_ms = s->server->desc.websocket_ping_interval_ms,
                                                 .pong_timeout_ms = s->server->desc.websocket_pong_timeout_ms,
                                                 .token = s->server->token});
                         (*handler)(cc::move(ws), request);
                     });

    return true;
}

/// Send the head of a streaming response, then hand the body to the handler.
void start_stream(cc::shared_ptr<session> const& s, http_server_response response)
{
    // HTTP/1.0 has no chunked encoding, so its body is whatever arrives before the close -- which is exactly the
    // problem chunked was added to solve, and the reason a streamed response cannot keep such a connection alive.
    auto const chunked = !s->parser.request().http_1_0;
    auto const keep_alive
        = chunked && !s->server->stopped.load() && s->parser.can_reuse_connection() && s->unparsed.empty();

    if (chunked)
        response.headers.set("Transfer-Encoding", "chunked");

    // A HEAD gets the head and nothing else, so there is no body for the handler to write and no call to make.
    auto const method = s->parser.request().method;
    auto const wants_body = method != http_method::head;

    auto head = impl::write_response_head(response.status, {}, response.headers, 0, keep_alive);
    if (head.has_error())
    {
        CC_LOG_WARNING("a streaming route produced a response that cannot be sent: {}", head.error().message);
        close_session(s);
        return;
    }

    s->head_bytes = cc::move(head).value();

    auto body = cc::make_shared<response_stream_state>();
    body->owner = s;
    body->chunked = chunked;
    body->keep_alive = keep_alive;

    auto const head_span
        = cc::span<byte const>(reinterpret_cast<byte const*>(s->head_bytes.data()), s->head_bytes.size());

    impl::when_ready(
        s->connection->send(head_span, deadline::after_ms(s->server->desc.request_timeout_ms), s->server->token),
        [s, body, wants_body, on_open = cc::move(response.on_open)](cc::shared_async<cc::unit> const& sent) mutable
        {
            if (sent->has_error())
            {
                body->failed = true;
                close_session(s);
                return;
            }

            if (!wants_body)
            {
                body->finished = true;
                if (body->keep_alive)
                    read_request(s);
                else
                    close_session(s);
                return;
            }

            // The handler owns the stream from here; dropping it finishes the body rather than stranding the
            // connection, which is what makes an abandoned stream an ordinary end.
            on_open(cc::make_shared<http_response_stream>(cc::move(body)));
        });
}

void write_response(cc::shared_ptr<session> const& s)
{
    if (!s->body_too_large && try_upgrade(s))
        return;

    auto response = answer(s);

    if (response.on_open)
    {
        start_stream(s, cc::move(response));
        return;
    }

    // A HEAD gets the head and none of the bytes, and its Content-Length still describes what a GET would return.
    auto const method = s->parser.request().method;
    auto const body_bytes = i64(response.body.size());
    auto const send_body = method != http_method::head && !response.body.empty();

    auto const keep_alive = !s->server->stopped.load() && s->parser.can_reuse_connection() && s->unparsed.empty();

    auto head = impl::write_response_head(response.status, {}, response.headers, body_bytes, keep_alive);
    if (head.has_error())
    {
        // A handler that built an unsendable header is a bug in the handler, and the connection is the only place
        // left to say so.
        CC_LOG_WARNING("a route produced a response that cannot be sent: {}", head.error().message);
        close_session(s);
        return;
    }

    s->head_bytes = cc::move(head).value();
    s->response_body = cc::move(response.body);

    auto const head_span
        = cc::span<byte const>(reinterpret_cast<byte const*>(s->head_bytes.data()), s->head_bytes.size());

    impl::when_ready(
        s->connection->send(head_span, deadline::after_ms(s->server->desc.request_timeout_ms), s->server->token),
        [s, keep_alive, send_body](cc::shared_async<cc::unit> const& sent)
        {
            if (sent->has_error())
            {
                close_session(s);
                return;
            }

            if (!send_body)
            {
                if (!keep_alive)
                {
                    close_session(s);
                    return;
                }
                read_request(s);
                return;
            }

            impl::when_ready(s->connection->send(s->response_body, deadline::after_ms(s->server->desc.request_timeout_ms),
                                                 s->server->token),
                             [s, keep_alive](cc::shared_async<cc::unit> const& body_sent)
                             {
                                 if (body_sent->has_error() || !keep_alive)
                                 {
                                     close_session(s);
                                     return;
                                 }
                                 read_request(s);
                             });
        });
}

/// Write queued chunks, one at a time, and end the response when the last one has gone out.
void pump_stream(cc::shared_ptr<response_stream_state> const& body)
{
    if (body->writing || body->outbox.empty() || body->failed)
        return;

    auto const& front = body->outbox[0];
    auto const ends = front.ends_the_body;
    auto const span = cc::span<byte const>(reinterpret_cast<byte const*>(front.bytes.data()), front.bytes.size());

    body->writing = true;

    auto const& s = body->owner;
    impl::when_ready(s->connection->send(span, deadline::after_ms(s->server->desc.request_timeout_ms), s->server->token),
                     [body, ends](cc::shared_async<cc::unit> const& sent)
                     {
                         body->writing = false;

                         auto promise = body->outbox[0].promise;
                         for (isize i = 1; i < body->outbox.size(); ++i)
                             body->outbox[i - 1] = cc::move(body->outbox[i]);
                         body->outbox.remove_back();

                         if (sent->has_error())
                         {
                             body->failed = true;
                             promise->push_error(sent->propagate_error());
                             close_session(body->owner);
                             return;
                         }

                         promise->push_value(cc::unit{});

                         if (!ends)
                         {
                             pump_stream(body);
                             return;
                         }

                         if (body->keep_alive)
                             read_request(body->owner);
                         else
                             close_session(body->owner);
                     });
}

/// Feed what has arrived, and answer once a whole request is in.
void parse_available(cc::shared_ptr<session> const& s)
{
    auto cursor = isize(0);

    while (cursor < s->unparsed.size() && !s->parser.message_complete())
    {
        auto const fed = s->parser.feed(cc::span<byte const>(s->unparsed.data() + cursor, s->unparsed.size() - cursor),
                                        [&s](cc::span<byte const> chunk) -> isize
                                        {
                                            if (isize(s->body.size()) + chunk.size() > s->server->desc.max_body_bytes)
                                            {
                                                // Read and thrown away rather than buffered: the answer is a 413, and getting there means
                                                // finishing the message so the connection is still in a known state.
                                                s->body_too_large = true;
                                                return chunk.size();
                                            }

                                            for (auto const b : chunk)
                                                s->body.push_back(b);
                                            return chunk.size();
                                        });

        if (fed.has_error())
        {
            // A request nobody can parse gets a 400 and the connection ends: whatever comes next on it is bytes
            // this server has already lost track of.
            auto const bad = impl::write_response_head(400, {}, {}, 0, false);
            if (bad.has_value())
            {
                s->head_bytes = cc::move(bad).value();
                auto const span
                    = cc::span<byte const>(reinterpret_cast<byte const*>(s->head_bytes.data()), s->head_bytes.size());
                impl::when_ready(
                    s->connection->send(span, deadline::after_ms(s->server->desc.request_timeout_ms), s->server->token),
                    [s](cc::shared_async<cc::unit> const&) { close_session(s); });
                return;
            }

            close_session(s);
            return;
        }

        if (fed.value() == 0)
            break;
        cursor += fed.value();
    }

    if (cursor > 0)
    {
        auto rest = cc::vector<byte>();
        for (auto i = cursor; i < s->unparsed.size(); ++i)
            rest.push_back(s->unparsed[i]);
        s->unparsed = cc::move(rest);
    }

    if (s->parser.message_complete())
    {
        write_response(s);
        return;
    }

    read_request(s);
}

void read_request(cc::shared_ptr<session> const& s)
{
    if (s->closed || s->server->stopped.load())
    {
        close_session(s);
        return;
    }

    // A finished message means the next request starts over; a half-read one keeps its parser and its buffers.
    if (s->parser.message_complete())
    {
        s->parser.start_request({.max_start_line_bytes = s->server->desc.max_header_bytes,
                                 .max_header_bytes = s->server->desc.max_header_bytes,
                                 .max_header_count = s->server->desc.max_header_count});
        s->body.clear();
        s->body_too_large = false;
        s->idle = true;

        // Bytes may already be here from the last read, which is what pipelining looks like even when nobody meant
        // to pipeline.
        if (!s->unparsed.empty())
        {
            parse_available(s);
            return;
        }
    }

    // Idle keep-alive is a longer wait than a half-sent request: one is a client thinking, the other is a client
    // dribbling, and only the second is what the short timeout is for.
    auto const timeout_ms = s->idle ? s->server->desc.idle_timeout_ms : s->server->desc.request_timeout_ms;

    auto buffer = cc::span<byte>(s->read_buffer.data(), s->read_buffer.size());
    impl::when_ready(s->connection->receive(buffer, deadline::after_ms(timeout_ms), s->server->token),
                     [s](cc::shared_async<isize> const& received)
                     {
                         if (received->has_error())
                         {
                             // A client that hung up between requests is the ordinary end of a keep-alive
                             // connection, not a failure worth reporting.
                             close_session(s);
                             return;
                         }

                         s->idle = false;

                         auto const n = received->value();
                         for (isize i = 0; i < n; ++i)
                             s->unparsed.push_back(s->read_buffer[i]);

                         parse_available(s);
                     });
}

void accept_one(http_server_state* server)
{
    if (server->stopped.load() || !server->listener.is_valid())
        return;

    impl::when_ready(server->listener->accept(deadline::never(), server->token),
                     [server](cc::shared_async<cc::shared_ptr<stream_connection>> const& accepted)
                     {
                         if (accepted->has_error())
                             return; // stopped, or the listener is gone

                         // Keep accepting first: a handler that takes a moment must not stall the next connection.
                         accept_one(server);

                         auto connection = accepted->value();

                         if (server->open_connections.load() >= server->desc.max_connections)
                         {
                             // Closed rather than queued: a connection nobody will read from is worth less than the
                             // file descriptor it holds, and a client learns immediately.
                             CC_LOG_WARNING("refusing a connection: {} are already open", server->desc.max_connections);
                             connection->close();
                             return;
                         }

                         server->open_connections.fetch_add(1);

                         auto s = cc::make_shared<session>();
                         s->server = server;
                         s->connection = cc::move(connection);
                         s->read_buffer.resize_to_defaulted(k_read_chunk);
                         s->parser.start_request({.max_start_line_bytes = server->desc.max_header_bytes,
                                                  .max_header_bytes = server->desc.max_header_bytes,
                                                  .max_header_count = server->desc.max_header_count});

                         read_request(s);
                     });
}

// ---- static files --------------------------------------------------------------------------------------

/// Turn a request path into a path under `root`, or refuse it.
///
/// Every escape is refused rather than resolved: `..` never gets a chance to mean anything, so there is no canonical
/// form here for two implementations to disagree about.
[[nodiscard]] cc::optional<cc::string> resolve_under_root(cc::string_view root,
                                                          cc::string_view url_prefix,
                                                          cc::string_view path)
{
    if (!path.starts_with(url_prefix))
        return {};

    auto relative = path.subview(url_prefix.size());
    while (!relative.empty() && relative.front() == '/')
        relative = relative.subview(1);

    auto const decoded = cc::percent_decode(relative);
    if (!decoded.has_value())
        return {};

    auto const text = cc::string_view(decoded.value());

    for (auto const c : text)
        if (c == '\0' || c == '\\' || c == ':')
            return {};

    auto rest = cc::string_view(text);
    while (!rest.empty())
    {
        auto const slash = rest.find('/');
        auto const segment = slash < 0 ? rest : rest.subview({.offset = 0, .size = slash});
        rest = slash < 0 ? cc::string_view() : rest.subview(slash + 1);

        // An empty segment is a `//`, which some path handlers collapse and others do not -- and a disagreement about
        // where a path starts is the whole traversal problem.
        if (segment.empty() || segment == "." || segment == "..")
            return {};
    }

    auto full = cc::string(root);
    if (!full.empty() && full.back() != '/' && full.back() != '\\')
        full += "/";

    // A bare directory is its index page, which is what a browser asking for `/` means.
    full += text.empty() ? cc::string_view("index.html") : cc::string_view(text);
    return full;
}

/// What a browser should treat a file as, from its extension and nothing else.
///
/// Sniffing content is what `X-Content-Type-Options: nosniff` exists to stop, so this never guesses from bytes.
[[nodiscard]] cc::string_view content_type_of(cc::string_view path)
{
    auto const dot = path.rfind('.');
    if (dot < 0)
        return "application/octet-stream";

    auto const ext = path.subview(dot + 1);

    if (ext == "html" || ext == "htm")
        return "text/html; charset=utf-8";
    if (ext == "css")
        return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs")
        return "text/javascript; charset=utf-8";
    if (ext == "json")
        return "application/json";
    if (ext == "svg")
        return "image/svg+xml";
    if (ext == "png")
        return "image/png";
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "gif")
        return "image/gif";
    if (ext == "webp")
        return "image/webp";
    if (ext == "ico")
        return "image/x-icon";
    if (ext == "wasm")
        return "application/wasm";
    if (ext == "woff2")
        return "font/woff2";
    if (ext == "txt" || ext == "md")
        return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

[[nodiscard]] http_server_response serve_file(cc::string_view root,
                                              cc::string_view url_prefix,
                                              cc::string_view path,
                                              isize max_bytes)
{
    auto const resolved = resolve_under_root(root, url_prefix, path);
    if (!resolved.has_value())
        return http_server_response::empty(404);

    auto adapter = cc::file_read_stream_adapter::open(resolved.value());
    if (adapter.has_error())
        return http_server_response::empty(404);

    auto stream = adapter.value().stream();

    auto const size = stream.size();
    if (size.has_value() && size.value() > i64(max_bytes))
    {
        CC_LOG_WARNING("refusing to serve {}: {} bytes is over the {} this server allows", resolved.value(),
                       size.value(), max_bytes);
        return http_server_response::empty(500);
    }

    auto content = stream.read_all();
    if (content.has_error())
        return http_server_response::empty(404);

    // Nothing here sniffs content, so the browser must not either.
    auto response = http_server_response::bytes(cc::move(content).value(), content_type_of(resolved.value()));
    response.headers.set("X-Content-Type-Options", "nosniff");
    return response;
}

[[nodiscard]] cc::result<cc::unique_ptr<http_server>, error> create_on(cc::unique_ptr<native_transport> owned,
                                                                       transport& t,
                                                                       http_server_description const& desc)
{
    if (desc.bind_all_interfaces)
        CC_LOG_WARNING("binding every interface: this server is not hardened for hostile input");

    auto const where = endpoint(
        desc.bind_all_interfaces ? ip_address::any(ip_family::v4) : ip_address::loopback(ip_family::v4), desc.port);

    auto listener = stream_listener::try_create(t, where);
    if (listener.has_error())
        return cc::error(cc::move(listener).error());

    auto state = cc::make_unique<http_server_state>(cc::move(owned), t, desc);
    state->listener = cc::move(listener).value();
    state->bound = state->listener->local();

    CC_LOG_INFO("http server listening on {}", state->bound);

    auto* const raw = state.get();
    auto server = cc::make_unique<http_server>(cc::move(state));
    accept_one(raw);
    return server;
}
} // namespace

// ---- the response helpers ------------------------------------------------------------------------------

http_server_response http_server_response::text(cc::string_view body, cc::string_view content_type, i32 status)
{
    auto response = http_server_response();
    response.status = status;
    response.headers.set("Content-Type", content_type);
    for (auto const c : body)
        response.body.push_back(byte(c));
    return response;
}

http_server_response http_server_response::bytes(cc::vector<byte> body, cc::string_view content_type, i32 status)
{
    auto response = http_server_response();
    response.status = status;
    response.headers.set("Content-Type", content_type);
    response.body = cc::move(body);
    return response;
}

http_server_response http_server_response::stream(cc::string_view content_type, response_stream_handler on_open, i32 status)
{
    auto response = http_server_response();
    response.status = status;
    response.headers.set("Content-Type", content_type);
    response.on_open = cc::move(on_open);
    return response;
}

http_server_response http_server_response::empty(i32 status)
{
    auto response = http_server_response();
    response.status = status;
    return response;
}

// ---- a streaming body ----------------------------------------------------------------------------------

http_response_stream::http_response_stream(cc::shared_ptr<response_stream_state> state) : _state(cc::move(state))
{
}

http_response_stream::~http_response_stream()
{
    finish();
}

cc::shared_async<cc::unit> http_response_stream::write(cc::span<byte const> chunk)
{
    // A zero-length chunk is what ENDS a chunked body, so writing one would truncate the response rather than send
    // nothing.
    if (chunk.empty())
    {
        auto done = cc::make_async_manual<cc::unit>();
        done->push_value(cc::unit{});
        return done;
    }

    if (!is_open())
        return impl::failed_async<cc::unit>({.code = error_code::connection_closed,
                                             .native_code = 0,
                                             .message = cc::string("the response body is already finished")});

    auto entry = outgoing_chunk();
    entry.promise = cc::make_async_manual<cc::unit>();

    if (_state->chunked)
    {
        entry.bytes = cc::format("{:x}\r\n", chunk.size());
        entry.bytes += cc::string_view(reinterpret_cast<char const*>(chunk.data()), chunk.size());
        entry.bytes += "\r\n";
    }
    else
    {
        entry.bytes = cc::string_view(reinterpret_cast<char const*>(chunk.data()), chunk.size());
    }

    auto const promise = entry.promise;
    _state->outbox.push_back(cc::move(entry));
    pump_stream(_state);
    return promise;
}

cc::shared_async<cc::unit> http_response_stream::write_text(cc::string_view chunk)
{
    return write(cc::span<byte const>(reinterpret_cast<byte const*>(chunk.data()), chunk.size()));
}

void http_response_stream::finish()
{
    if (!_state.is_valid() || _state->finished)
        return;
    _state->finished = true;

    if (_state->failed)
        return;

    auto entry = outgoing_chunk();
    entry.promise = cc::make_async_manual<cc::unit>();
    entry.ends_the_body = true;

    // An HTTP/1.0 body ends with the connection rather than with a terminator, so there is nothing to write -- but
    // the empty entry still runs the queue down to the close in order.
    if (_state->chunked)
        entry.bytes = "0\r\n\r\n";

    _state->outbox.push_back(cc::move(entry));
    pump_stream(_state);
}

bool http_response_stream::is_open() const
{
    return _state.is_valid() && !_state->finished && !_state->failed;
}

// ---- the server ----------------------------------------------------------------------------------------

http_server::http_server(cc::unique_ptr<http_server_state> state) : _state(cc::move(state))
{
}

http_server::~http_server()
{
    stop();
}

cc::result<cc::unique_ptr<http_server>, error> http_server::try_create(transport& t, http_server_description const& desc)
{
    return create_on({}, t, desc);
}

cc::result<cc::unique_ptr<http_server>, error> http_server::try_create(transport& t)
{
    return try_create(t, http_server_description());
}

cc::result<cc::unique_ptr<http_server>, error> http_server::try_create(io_system& io, http_server_description const& desc)
{
    // Owned rather than a temporary: the server outlives this call, and a transport reference into a dead temporary
    // is the obvious way to get this wrong.
    auto owned = cc::make_unique<native_transport>(io);
    auto& t = *owned;
    return create_on(cc::move(owned), t, desc);
}

cc::result<cc::unique_ptr<http_server>, error> http_server::try_create(io_system& io)
{
    return try_create(io, http_server_description());
}

endpoint http_server::local() const
{
    // Remembered rather than asked, so it still answers after the listener is gone -- a caller logging where the
    // server was should not have to care that it has stopped.
    return _state->bound;
}

void http_server::route(http_method method, cc::string_view pattern, route_handler handler)
{
    auto entry = route_entry();
    entry.method = method;
    entry.is_prefix = !pattern.empty() && pattern[pattern.size() - 1] == '*';
    entry.pattern = cc::string(entry.is_prefix ? pattern.subview({.offset = 0, .size = pattern.size() - 1}) : pattern);
    entry.handler = cc::move(handler);

    _state->routes.lock([&](cc::vector<route_entry>& all) { all.push_back(cc::move(entry)); });
}

void http_server::serve_directory(cc::string_view url_prefix, cc::string_view root)
{
    auto pattern = cc::string(url_prefix);
    if (pattern.empty() || pattern.back() != '/')
        pattern += "/";
    auto const prefix = pattern;
    pattern += "*";

    auto const root_copy = cc::string(root);
    auto const max_bytes = _state->desc.max_static_file_bytes;

    for (auto const method : {http_method::get, http_method::head})
        route(method, pattern, [root_copy, prefix, max_bytes](http_server_request const& request)
              { return serve_file(root_copy, prefix, request.path, max_bytes); });
}

void http_server::websocket_route(cc::string_view pattern, websocket_handler handler)
{
    auto entry = websocket_route_entry();
    entry.pattern = pattern;
    entry.handler = cc::move(handler);

    _state->websocket_routes.lock([&](cc::vector<websocket_route_entry>& all) { all.push_back(cc::move(entry)); });
}

void http_server::stop()
{
    if (_state->stopped.exchange(true))
        return;

    // Everything in flight -- the accept, every read, every write -- is registered with this token, so shutdown is
    // immediate rather than a wait for deadlines nobody set.
    _state->token.cancel();

    // And the listener goes with it: a stopped server that still holds its port would accept connections nobody
    // will ever read from, which is a client waiting forever rather than learning at once.
    _state->listener = {};
}

i32 http_server::open_connections() const
{
    return _state->open_connections.load();
}

i64 http_server::requests_handled() const
{
    return _state->requests_handled.load();
}
} // namespace cnet
