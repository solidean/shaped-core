#include <clean-core/container/vector.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/http/http_server.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// Our server, answered by our client, over a network that does not exist.
//
// Both ends are real -- a real request goes out, a real response comes back, and both are parsed by the code that
// would parse them over a socket.
// What is absent is the socket, which is why none of this needs a port or a second process.

namespace
{
bool pump_until(cc::function_ref<bool()> done, i32 rounds = 20000)
{
    for (i32 i = 0; i < rounds; ++i)
    {
        if (done())
            return true;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
    return done();
}

/// Pump against the wall clock rather than a round count.
///
/// The round-counting one above is right for a virtual network, where nothing waits on the world; a real socket
/// does, and counting rounds there measures how fast this machine spins rather than how long it was given.
bool pump_for(cc::function_ref<bool()> done, f64 budget_secs = 10.0)
{
    auto& clk = system_clock();
    auto const deadline_ns = clk.now_ns() + i64(budget_secs * 1e9);

    while (true)
    {
        if (done())
            return true;
        if (clk.now_ns() >= deadline_ns)
        {
            // A budget that runs out here is a wait that never finished, and the thread that noticed is never the one
            // that matters -- so say what every other thread was doing before failing.
            cc::report_all_thread_stacks("a cnet test waited out its budget");
            return false;
        }
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
}

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view text)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
}

/// A virtual network with our server on it and our client pointed at it.
struct server_fixture
{
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<resolver> res;
    cc::unique_ptr<http_server> server;
    cc::unique_ptr<native_http_client> client;

    ip_address address = ip_address::loopback(ip_family::v4);

    explicit server_fixture(http_server_description const& desc = {})
    {
        io = io_system::create({.unthreaded = true});
        net = cc::make_unique<virtual_network>(*io);

        auto const answer = address;
        res = resolver::create(*io, {.lookup = [answer](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                     { return cc::vector<ip_address>{answer}; }});

        server = http_server::try_create(*net, desc).value();
        client = cc::make_unique<native_http_client>(*net, *res);
    }

    /// The URL of a path on this server, which needs the port it actually got.
    [[nodiscard]] cc::string url_for(cc::string_view path) const
    {
        return cc::format("http://localhost:{}{}", server->local().port, path);
    }
};
} // namespace

TEST("cnet - a server answers a route")
{
    auto fixture = server_fixture();

    fixture.server->route(http_method::get, "/hello",
                          [](http_server_request const&) { return http_server_response::text("hello, world"); });

    auto response = http_get(*fixture.client, fixture.url_for("/hello"));
    CHECK(pump_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);

    CHECK(response->value().status() == 200);
    CHECK(response->value().body_text() == "hello, world");
    CHECK(response->value().head.headers.get("Content-Type").value() == "text/plain; charset=utf-8");

    // The length is written for the handler, so a keep-alive connection has an end the client can find.
    CHECK(response->value().head.headers.get("Content-Length").value() == "12");
    CHECK(fixture.server->requests_handled() == 1);
}

TEST("cnet - a path nothing serves is a 404, and a method nothing serves is a 405")
{
    auto fixture = server_fixture();

    fixture.server->route(http_method::get, "/only-get",
                          [](http_server_request const&) { return http_server_response::text("here"); });

    auto missing = http_get(*fixture.client, fixture.url_for("/nowhere"));
    CHECK(pump_until([&] { return missing->is_ready(); }));
    CHECK(missing->value().status() == 404);

    // The path exists and the method does not, which is a different fact and a client can act on it.
    auto request = http_request();
    request.method = http_method::post;
    request.target = http_target::parse(fixture.url_for("/only-get")).value();

    auto wrong_method = http_send(*fixture.client, cc::move(request));
    CHECK(pump_until([&] { return wrong_method->is_ready(); }));
    CHECK(wrong_method->value().status() == 405);
}

TEST("cnet - a handler sees the request it was sent")
{
    auto fixture = server_fixture();

    auto seen = cc::make_shared<http_server_request>();
    fixture.server->route(http_method::post, "/echo",
                          [seen](http_server_request const& request)
                          {
                              seen->method = request.method;
                              seen->target = request.target;
                              seen->path = request.path;
                              seen->query = request.query;
                              seen->body = request.body;
                              return http_server_response::text(request.body_text());
                          });

    auto const payload = cc::string_view("the body");

    auto request = http_request();
    request.method = http_method::post;
    request.target = http_target::parse(fixture.url_for("/echo?a=1&b=2")).value();
    request.headers.add("Content-Length", cc::format("{}", payload.size()));
    request.headers.add("X-Custom", "kept");
    request.body = bytes_of(payload);

    auto response = http_send(*fixture.client, cc::move(request));
    CHECK(pump_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().body_text() == payload);

    CHECK(seen->path == "/echo");
    CHECK(seen->query == "a=1&b=2");
    CHECK(seen->target == "/echo?a=1&b=2");
}

TEST("cnet - a wildcard route matches everything under it, and a specific one added first wins")
{
    auto fixture = server_fixture();

    fixture.server->route(http_method::get, "/files/special",
                          [](http_server_request const&) { return http_server_response::text("the special one"); });
    fixture.server->route(http_method::get, "/files/*",
                          [](http_server_request const& request) { return http_server_response::text(request.path); });

    auto specific = http_get(*fixture.client, fixture.url_for("/files/special"));
    CHECK(pump_until([&] { return specific->is_ready(); }));
    CHECK(specific->value().body_text() == "the special one");

    auto under = http_get(*fixture.client, fixture.url_for("/files/a/b/c"));
    CHECK(pump_until([&] { return under->is_ready(); }));
    CHECK(under->value().body_text() == "/files/a/b/c");
}

TEST("cnet - a HEAD gets the head and none of the body")
{
    auto fixture = server_fixture();

    fixture.server->route(http_method::head, "/thing",
                          [](http_server_request const&) { return http_server_response::text("0123456789"); });

    auto request = http_request();
    request.method = http_method::head;
    request.target = http_target::parse(fixture.url_for("/thing")).value();

    auto response = http_send(*fixture.client, cc::move(request));
    CHECK(pump_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status() == 200);

    // The length still describes what a GET would have returned, and not one byte comes with it.
    CHECK(response->value().head.headers.get("Content-Length").value() == "10");
    CHECK(response->value().body.empty());
}

TEST("cnet - two requests share one connection")
{
    auto fixture = server_fixture();

    fixture.server->route(http_method::get, "/a",
                          [](http_server_request const&) { return http_server_response::text("first"); });
    fixture.server->route(http_method::get, "/b",
                          [](http_server_request const&) { return http_server_response::text("second"); });

    auto first = http_get(*fixture.client, fixture.url_for("/a"));
    CHECK(pump_until([&] { return first->is_ready(); }));
    CHECK(first->value().body_text() == "first");

    auto second = http_get(*fixture.client, fixture.url_for("/b"));
    CHECK(pump_until([&] { return second->is_ready(); }));
    CHECK(second->value().body_text() == "second");

    // Both ends kept it: the client pooled it, and the server did not close it after answering.
    CHECK(fixture.server->open_connections() == 1);
    CHECK(fixture.server->requests_handled() == 2);
}

TEST("cnet - a body over the limit is refused rather than buffered")
{
    auto fixture = server_fixture({.max_body_bytes = 16});

    fixture.server->route(http_method::post, "/upload", [](http_server_request const& request)
                          { return http_server_response::text(cc::format("{}", request.body.size())); });

    auto const payload = cc::string_view("0123456789012345678901234567890123456789");

    auto request = http_request();
    request.method = http_method::post;
    request.target = http_target::parse(fixture.url_for("/upload")).value();
    request.headers.add("Content-Length", cc::format("{}", payload.size()));
    request.body = bytes_of(payload);

    auto response = http_send(*fixture.client, cc::move(request));
    CHECK(pump_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);

    // Read to the end and thrown away rather than buffered: the answer is a 413, and the handler never runs.
    CHECK(response->value().status() == 413);
    CHECK(fixture.server->requests_handled() == 0);
}

TEST("cnet - a request nothing can parse gets a 400 and the connection ends")
{
    auto fixture = server_fixture();
    fixture.server->route(http_method::get, "/",
                          [](http_server_request const&) { return http_server_response::text("fine"); });

    // Straight onto the wire, since no client of ours would send this.
    auto connected = tcp_connect(*fixture.net, fixture.server->local());
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(connected->try_error() == nullptr);

    auto const& raw = connected->value();
    auto sent = raw->send(bytes_of("GET / HTTP/1.1\r\nBad Header: x\r\n\r\n"));
    CHECK(pump_until([&] { return sent->is_ready(); }));

    byte inbox[256] = {};
    auto received = raw->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->try_error() == nullptr);

    auto const answer = cc::string_view(reinterpret_cast<char const*>(inbox), received->value());
    CHECK(answer.starts_with("HTTP/1.1 400 "));
    CHECK(answer.contains("Connection: close"));
}

TEST("cnet - a connection past the limit is closed rather than queued")
{
    auto fixture = server_fixture({.max_connections = 1});
    fixture.server->route(http_method::get, "/",
                          [](http_server_request const&) { return http_server_response::text("ok"); });

    auto first = tcp_connect(*fixture.net, fixture.server->local());
    CHECK(pump_until([&] { return first->is_ready(); }));
    CHECK(pump_until([&] { return fixture.server->open_connections() == 1; }));

    auto second = tcp_connect(*fixture.net, fixture.server->local());
    CHECK(pump_until([&] { return second->is_ready(); }));

    // The connection is accepted and then closed, so the client learns immediately rather than waiting on a server
    // that will never read from it.
    byte inbox[16] = {};
    auto received = second->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->try_error() != nullptr);

    CHECK(fixture.server->open_connections() == 1);
}

TEST("cnet - stopping the server ends everything in flight")
{
    auto fixture = server_fixture();
    fixture.server->route(http_method::get, "/",
                          [](http_server_request const&) { return http_server_response::text("ok"); });

    auto connected = tcp_connect(*fixture.net, fixture.server->local());
    CHECK(pump_until([&] { return connected->is_ready(); }));
    CHECK(pump_until([&] { return fixture.server->open_connections() == 1; }));

    // Shutdown goes through the server's own token, so the connection parked on a read ends at once rather than on
    // a deadline nobody set.
    fixture.server->stop();
    CHECK(pump_until([&] { return fixture.server->open_connections() == 0; }));

    // And nothing new is accepted afterwards.
    //
    // Waited out on a clock rather than a round count: what is being asserted is that the request FAILS, and a round
    // budget would make that a claim about how fast this machine spins -- which it is not, and which is a different
    // number on every one CI runs.
    auto late = http_get(*fixture.client, fixture.url_for("/"));
    CHECK(pump_for([&] { return late->is_ready(); }));
    CHECK(late->try_error() != nullptr);
}

TEST("cnet - a server on a real socket answers a real client")
{
    auto io = io_system::try_create({.unthreaded = true});
    if (io.has_error())
        SKIP("this platform has no sockets");

    auto server = http_server::try_create(*io.value());
    if (server.has_error())
        SKIP("this platform cannot listen");

    server.value()->route(http_method::get, "/ping",
                          [](http_server_request const&) { return http_server_response::text("pong"); });

    auto res = resolver::try_create(*io.value());
    if (res.has_error())
        SKIP("this platform cannot resolve");

    // The one test here that uses the platform's own sockets and its own resolver: `localhost` is in every machine's
    // hosts file, and the port came from the OS.
    auto transport = native_transport(*io.value());
    auto client = native_http_client(transport, *res.value());
    auto response = http_get(client, cc::format("http://localhost:{}/ping", server.value()->local().port));

    CHECK(pump_for([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status() == 200);
    CHECK(response->value().body_text() == "pong");
}

// ---- streaming responses -------------------------------------------------------------------------------

TEST("cnet - a streamed response arrives as chunks and keeps the connection")
{
    auto fixture = server_fixture();

    auto opened = cc::vector<cc::shared_ptr<http_response_stream>>();
    fixture.server->route(http_method::get, "/stream",
                          [&opened](http_server_request const&)
                          {
                              return http_server_response::stream("text/plain; charset=utf-8",
                                                                  [&opened](cc::shared_ptr<http_response_stream> body)
                                                                  { opened.push_back(cc::move(body)); });
                          });

    auto response = http_get(*fixture.client, fixture.url_for("/stream"));
    CHECK(pump_until([&] { return !opened.empty(); }));
    REQUIRE(opened.size() == 1);

    auto const body = opened[0];
    auto const a = body->write_text("one ");
    auto const b = body->write_text("two ");
    auto const c = body->write_text("three");
    CHECK(pump_until([&] { return a->is_ready() && b->is_ready() && c->is_ready(); }));

    // Nothing has ended the body yet, so the client is still waiting on it.
    CHECK(!response->is_ready());

    body->finish();
    CHECK(pump_for([&] { return response->is_ready(); }));
    REQUIRE(response->try_error() == nullptr);

    CHECK(response->value().status() == 200);
    CHECK(response->value().body_text() == "one two three");
    CHECK(response->value().head.headers.get("Transfer-Encoding").value() == "chunked");

    // No Content-Length, because nobody knew one -- which is the whole reason chunked exists.
    CHECK(!response->value().head.headers.contains("Content-Length"));

    // And the connection survives it, which close-delimiting would not allow.
    opened.clear();
    fixture.server->route(http_method::get, "/after",
                          [](http_server_request const&) { return http_server_response::text("still here"); });

    auto second = http_get(*fixture.client, fixture.url_for("/after"));
    CHECK(pump_for([&] { return second->is_ready(); }));
    REQUIRE(second->try_error() == nullptr);
    CHECK(second->value().body_text() == "still here");
}

TEST("cnet - an abandoned stream ends the response rather than hanging it")
{
    auto fixture = server_fixture();

    auto opened = cc::vector<cc::shared_ptr<http_response_stream>>();
    fixture.server->route(http_method::get, "/stream",
                          [&opened](http_server_request const&)
                          {
                              return http_server_response::stream("text/plain",
                                                                  [&opened](cc::shared_ptr<http_response_stream> body)
                                                                  { opened.push_back(cc::move(body)); });
                          });

    auto response = http_get(*fixture.client, fixture.url_for("/stream"));
    CHECK(pump_until([&] { return !opened.empty(); }));

    auto const wrote = opened[0]->write_text("partial");
    CHECK(pump_until([&] { return wrote->is_ready(); }));

    // Dropping the last reference is what a handler that decided it has nothing more to say looks like.
    opened.clear();

    CHECK(pump_for([&] { return response->is_ready(); }));
    REQUIRE(response->try_error() == nullptr);
    CHECK(response->value().body_text() == "partial");
}

TEST("cnet - an empty chunk is dropped rather than ending the body")
{
    auto fixture = server_fixture();

    auto opened = cc::vector<cc::shared_ptr<http_response_stream>>();
    fixture.server->route(http_method::get, "/stream",
                          [&opened](http_server_request const&)
                          {
                              return http_server_response::stream("text/plain",
                                                                  [&opened](cc::shared_ptr<http_response_stream> body)
                                                                  { opened.push_back(cc::move(body)); });
                          });

    auto response = http_get(*fixture.client, fixture.url_for("/stream"));
    CHECK(pump_until([&] { return !opened.empty(); }));

    auto const body = opened[0];
    auto const empty = body->write({});
    auto const after = body->write_text("still arrives");
    CHECK(pump_until([&] { return empty->is_ready() && after->is_ready(); }));
    CHECK(body->is_open());

    body->finish();
    CHECK(pump_for([&] { return response->is_ready(); }));
    REQUIRE(response->try_error() == nullptr);
    CHECK(response->value().body_text() == "still arrives");
}

// ---- static files --------------------------------------------------------------------------------------

namespace
{
/// Two files in the process's temp directory, and the names to reach them by.
///
/// The temp directory itself is the served root: clean-core has no `mkdir`, so a test cannot make a directory of its
/// own -- which is the library gap this works around rather than hides.
struct static_files
{
    cc::string root;
    cc::string css_name;
    cc::string html_name;

    bool written = false;

    static_files()
    {
        root = cc::temp_directory_path();

        auto const css_path = cc::temp_file_path("cnet-static", ".css");
        auto const html_path = cc::temp_file_path("cnet-static", ".html");
        css_name = basename_of(css_path);
        html_name = basename_of(html_path);

        written = write(css_path, "body { color: red }") && write(html_path, "<h1>home</h1>");
    }

    ~static_files()
    {
        cc::remove_file(cc::format("{}/{}", root, css_name));
        cc::remove_file(cc::format("{}/{}", root, html_name));
    }

    [[nodiscard]] static cc::string basename_of(cc::string_view path)
    {
        auto const slash = path.rfind('/');
        auto const backslash = path.rfind('\\');
        auto const cut = slash > backslash ? slash : backslash;
        return cc::string(cut < 0 ? path : path.subview(cut + 1));
    }

    [[nodiscard]] static bool write(cc::string_view path, cc::string_view content)
    {
        auto adapter = cc::file_write_stream_adapter::create(path);
        if (adapter.has_error())
            return false;

        auto stream = adapter.value().stream();
        auto const bytes = cc::span<byte const>(reinterpret_cast<byte const*>(content.data()), content.size());
        return stream.write(bytes).has_value() && stream.flush().has_value();
    }
};
} // namespace

// The two file-serving tests wait on a generous wall budget rather than a tight one.
// They share a machine with every other test in this binary, and a slow answer here is the scheduler rather than a
// defect worth failing on -- while a real hang still fails, just later.
TEST("cnet - a served directory hands back its files")
{
    auto const files = static_files();
    if (!files.written)
        SKIP("this platform did not let the test write into its temp directory");

    auto fixture = server_fixture();
    fixture.server->serve_directory("/app", files.root);

    auto css = http_get(*fixture.client, fixture.url_for(cc::format("/app/{}", files.css_name)));
    CHECK(pump_for([&] { return css->is_ready(); }));
    REQUIRE(css->try_error() == nullptr);

    CHECK(css->value().status() == 200);
    CHECK(css->value().body_text() == "body { color: red }");

    // The extension decides the type, and nothing sniffs the bytes -- which is what nosniff tells the browser too.
    CHECK(css->value().head.headers.get("Content-Type").value() == "text/css; charset=utf-8");
    CHECK(css->value().head.headers.get("X-Content-Type-Options").value() == "nosniff");

    auto html = http_get(*fixture.client, fixture.url_for(cc::format("/app/{}", files.html_name)));
    CHECK(pump_for([&] { return html->is_ready(); }));
    CHECK(html->value().body_text() == "<h1>home</h1>");
    CHECK(html->value().head.headers.get("Content-Type").value() == "text/html; charset=utf-8");

    auto missing = http_get(*fixture.client, fixture.url_for("/app/nope.txt"));
    CHECK(pump_for([&] { return missing->is_ready(); }));
    CHECK(missing->value().status() == 404);
}

TEST("cnet - a served directory refuses every way out of itself")
{
    auto const files = static_files();
    auto fixture = server_fixture();
    fixture.server->serve_directory("/app", files.root);

    // Every one of these is refused outright rather than resolved and then checked.
    auto const escapes = cc::vector<cc::string>{
        "/app/../secret.txt",     //
        "/app/%2e%2e/secret.txt", // the same, hidden from anything that looks before decoding
        "/app/deep/../../secret.txt",
        "/app//style.css",  // an empty segment, which path handlers disagree about
        "/app/./style.css", // a segment that means nothing, and is therefore a second spelling of one that does
        "/app/C%3A/windows/win.ini",
        "/app/..%5csecret.txt", // a backslash, which is a separator on one platform and not the other
    };

    for (auto const& path : escapes)
    {
        auto response = http_get(*fixture.client, fixture.url_for(path));
        CHECK(pump_for([&] { return response->is_ready(); }));
        REQUIRE(response->try_error() == nullptr);
        CHECK(response->value().status() == 404);
    }
}

TEST("cnet - a request body arrives, and the connection is still usable afterwards")
{
    auto fixture = server_fixture();

    fixture.server->route(http_method::post, "/echo", [](http_server_request const& request)
                          { return http_server_response::text(cc::format("got {}", request.body_text())); });
    fixture.server->route(http_method::get, "/after",
                          [](http_server_request const&) { return http_server_response::text("second"); });

    auto const payload = cc::string_view("some body bytes");
    auto request
        = http_request{.method = http_method::post, .target = http_target::parse(fixture.url_for("/echo")).value()};
    request.body = bytes_of(payload);

    auto posted = http_send(*fixture.client, cc::move(request));
    CHECK(pump_until([&] { return posted->is_ready(); }));
    REQUIRE(posted->try_error() == nullptr);
    CHECK(posted->value().body_text() == "got some body bytes");

    // The one that matters: an unframed body would still be sitting on the connection, and this request would be
    // read as a continuation of it.
    auto second = http_get(*fixture.client, fixture.url_for("/after"));
    CHECK(pump_until([&] { return second->is_ready(); }));
    REQUIRE(second->try_error() == nullptr);
    CHECK(second->value().status() == 200);
    CHECK(second->value().body_text() == "second");
}
