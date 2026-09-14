#include <clean-core/container/pinned_data.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-net/address/resolver.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/http/http_server.hh>
#include <clean-net/ws/websocket.hh>
#include <nexus/async-test.hh>

using namespace cc::primitive_defines;

// The loopback dev server: routes, static files, a streamed body, and a WebSocket.
//
// Everything here is what a debug UI in a browser needs, and a browser is who it is for.
// This example plays both parts so it runs anywhere -- open the printed URL in a browser while it runs and you get the
// same answers.
//
// IT IS NOT HARDENED FOR HOSTILE INPUT, which is a decision rather than an omission: what separates this from a web
// server is almost entirely work about hostility, and nothing that can reach 127.0.0.1 is hostile that is not already
// running as you.

namespace
{
/// The first value a handler hands over, as an async the example awaits.
/// A handler runs on the io_system's thread, so it delivers through a node rather than into a variable the body reads.
template <class T>
void deliver_once(cc::shared_async<T> const& node, T value)
{
    if (!node->is_ready())
        node->push_value(cc::move(value));
}

/// One file in the temp directory, so `serve_directory` has something to serve.
struct served_file
{
    cc::string root;
    cc::string name;
    bool written = false;

    served_file()
    {
        root = cc::temp_directory_path();

        auto const path = cc::temp_file_path("cnet-example", ".html");
        auto const slash = cc::string_view(path).rfind('/');
        auto const backslash = cc::string_view(path).rfind('\\');
        auto const cut = slash > backslash ? slash : backslash;
        name = cc::string(cut < 0 ? cc::string_view(path) : cc::string_view(path).subview(cut + 1));

        auto adapter = cc::file_write_stream_adapter::create(path);
        if (adapter.has_error())
            return;

        auto const html = cc::string_view("<h1>served from disk</h1>");
        auto stream = adapter.value().stream();
        written = stream.write(cc::span<byte const>(reinterpret_cast<byte const*>(html.data()), html.size())).has_value()
               && stream.flush().has_value();
    }

    ~served_file() { cc::remove_file(cc::format("{}/{}", root, name)); }
};
} // namespace

ASYNC_EXAMPLE("clean-net/dev-server")
{
    auto io = cnet::io_system::create({});
    auto const files = served_file();

    auto server = cnet::http_server::try_create(*io).value();
    auto const base = cc::format("http://127.0.0.1:{}", server->local().port);

    // The library logs the same thing through `cc::rec`, whose console listener writes in batches -- so that line
    // lands wherever the batch ends rather than here, and this is the one a reader should follow.
    cc::println("serving on {}", base);
    cc::println("");

    // ---- routes ------------------------------------------------------------------------------------

    // Tried in the order they were added, so a specific path added before a wildcard wins.
    server->route(cnet::http_method::get, "/", [](cnet::http_server_request const&)
                  { return cnet::http_server_response::text("<h1>hello</h1>", "text/html; charset=utf-8"); });

    server->route(cnet::http_method::post, "/echo", [](cnet::http_server_request const& request)
                  { return cnet::http_server_response::text(cc::format("you said: {}", request.body_text())); });

    // ---- files, confined under their root ----------------------------------------------------------

    // `..`, `.`, an empty segment, a backslash, a colon and a NUL are refused OUTRIGHT rather than resolved.
    // Refusing the escape token is the stronger check: there is then no canonical form for two implementations to
    // disagree about, which is the shape every traversal bug has.
    server->serve_directory("/files", files.root);

    // ---- a body whose length nobody knows -----------------------------------------------------------

    auto const stream_opened = cc::make_async_manual<cc::shared_ptr<cnet::http_response_stream>>();
    server->route(cnet::http_method::get, "/events",
                  [stream_opened](cnet::http_server_request const&)
                  {
                      return cnet::http_server_response::stream(
                          "text/event-stream", [stream_opened](cc::shared_ptr<cnet::http_response_stream> body)
                          { deliver_once(stream_opened, cc::move(body)); });
                  });

    // ---- a websocket, for the half of a debug UI that is not request-shaped -------------------------

    auto const socket_accepted = cc::make_async_manual<cc::shared_ptr<cnet::websocket>>();
    server->websocket_route("/feed",
                            [socket_accepted](cc::shared_ptr<cnet::websocket> ws, cnet::http_server_request const&)
                            { deliver_once(socket_accepted, cc::move(ws)); });

    // ---- and now play the browser ------------------------------------------------------------------

    auto client = cnet::make_http_client(*io).value();

    auto home = cnet::http_get(*client, cc::format("{}/", base));
    co_await cc::async_settled(home);
    cc::println("GET  {:<21} -> {} {}", "/", home->value().status(), home->value().body_text());

    auto posted = cnet::http_request{.method = cnet::http_method::post,
                                     .target = cnet::http_target::parse(cc::format("{}/echo", base)).value()};
    // The body carries its OWNER, so it outlives this call without the caller having to keep anything alive:
    // `make_pinned_data` moves the string in rather than copying it.
    posted.body = cc::make_pinned_data(cc::string("ping")).reinterpret_as<byte const>();

    auto echoed = cnet::http_send(*client, cc::move(posted));
    co_await cc::async_settled(echoed);
    cc::println("POST {:<21} -> {} {}", "/echo", echoed->value().status(), echoed->value().body_text());

    if (files.written)
    {
        auto served = cnet::http_get(*client, cc::format("{}/files/{}", base, files.name));
        co_await cc::async_settled(served);
        cc::println("GET  {:<21} -> {} {}", "/files/<file>", served->value().status(), served->value().body_text());
    }

    // Every way out of the root is a 404, whether it is spelled plainly or hidden behind a percent-escape.
    for (auto const escape : {"/files/../secret", "/files/%2e%2e/secret"})
    {
        auto refused = cnet::http_get(*client, cc::format("{}{}", base, escape));
        co_await cc::async_settled(refused);
        cc::println("GET  {:<21} -> {}", escape, refused->value().status());
    }

    // ---- the streamed body, from both ends ---------------------------------------------------------

    cc::println("");
    auto events = cnet::http_get(*client, cc::format("{}/events", base));

    // The handler is called once the head is out, which has not happened yet.
    auto const body = co_await stream_opened;
    for (auto i = 0; i < 3; ++i)
    {
        auto const written = body->write_text(cc::format("data: tick {}\n\n", i));
        co_await cc::async_settled(written);
    }

    // Dropping the last reference would end it just as well; this says so out loud.
    body->finish();

    co_await cc::async_settled(events);
    cc::println("GET  {:<21} -> {} chunked, {} bytes after {} writes", "/events", events->value().status(),
                events->value().body.size(), 3);

    // ---- the websocket, from both ends -------------------------------------------------------------

    cc::println("");
    auto resolver = cnet::resolver::try_create(*io).value();
    auto connecting = cnet::websocket_connect(*io, *resolver, cc::format("ws://127.0.0.1:{}/feed", server->local().port));
    co_await cc::async_settled(connecting);

    auto const browser_side = connecting->value();
    auto const server_side = co_await socket_accepted;

    // Ping, pong and close are answered by the layer itself; a message is what a caller sees.
    auto const sent = browser_side->send_text("hello from the page");
    co_await cc::async_settled(sent);

    auto heard = server_side->receive();
    co_await cc::async_settled(heard);
    cc::println("ws  page -> server    : {}", heard->value().text());

    auto const replied = server_side->send_text("and back again");
    co_await cc::async_settled(replied);
    auto answer = browser_side->receive();
    co_await cc::async_settled(answer);
    cc::println("ws  server -> page    : {}", answer->value().text());

    browser_side->close();

    // ---- shutdown ----------------------------------------------------------------------------------

    cc::println("");
    cc::println("{} requests reached a route", server->routed_requests());

    // Immediate, through the server's own cancellation token: everything in flight ends rather than waiting for a
    // deadline nobody set.
    server->stop();
}
