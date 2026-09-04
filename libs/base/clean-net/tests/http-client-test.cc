#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// The whole client, from a URL to a parsed response, against a server that is a string.
//
// Everything runs over cnet::virtual_network with a resolver answering from a table, so a test names a host, gets a
// connection to something in this process, and exercises the real resolve-connect-write-parse path.
// No port, no server, and no dependence on anything outside the test.

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

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view text)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size());
}

/// A request body over borrowed bytes: the caller keeps `text` alive, which a test's local string does.
[[nodiscard]] cc::pinned_data<byte const> borrowed_body(cc::string_view text)
{
    return cc::pinned_data<byte const>::create_from_pin(bytes_of(text), nullptr);
}

/// A server that reads a request and answers with whatever it was told to.
///
/// It is deliberately not an HTTP server: it answers with bytes, which is what lets a test hand it a malformed or
/// hostile response as easily as a well-formed one.
///
/// Nothing here waits.
/// Every step starts what it can and looks at what has already settled, because a server that pumped inside its own
/// step would be pumping the same loop that drives it.
struct scripted_server
{
    struct session
    {
        cc::shared_ptr<stream_connection> connection;
        cc::shared_async<isize> receiving;
        cc::shared_async<cc::unit> sending;
        cc::vector<byte> inbox;
        cc::string response;
    };

    cc::unique_ptr<stream_listener> listener;
    cc::vector<cc::string> responses;
    isize next_response = 0;

    /// Hang up after answering, however the response was framed.
    /// A server is allowed to, and a client that pooled the connection has no way to know until it tries.
    bool close_after_response = false;

    /// How many connections were accepted, which is what says whether pooling did anything.
    isize accept_count = 0;

    /// What the last request looked like on the wire.
    cc::string last_request;
    isize request_count = 0;

    cc::shared_async<cc::shared_ptr<stream_connection>> accepting;
    cc::vector<cc::unique_ptr<session>> sessions;

    void listen_again() { accepting = listener->accept(); }

    void step()
    {
        if (accepting->is_ready())
        {
            if (accepting->try_error() == nullptr)
            {
                ++accept_count;

                auto fresh = cc::make_unique<session>();
                fresh->connection = accepting->value();
                fresh->inbox.resize_to_defaulted(8 * 1024);
                fresh->receiving = fresh->connection->receive(fresh->inbox, deadline::never());
                sessions.push_back(cc::move(fresh));
            }
            listen_again();
        }

        for (auto& s : sessions)
        {
            if (!s->connection.is_valid() || !s->receiving.is_valid() || !s->receiving->is_ready())
                continue;

            if (s->receiving->try_error() != nullptr)
            {
                s->connection->close();
                s->connection = {};
                continue;
            }

            last_request
                = cc::string(cc::string_view(reinterpret_cast<char const*>(s->inbox.data()), s->receiving->value()));
            ++request_count;
            s->receiving = {};

            s->response = responses[next_response < responses.size() ? next_response : responses.size() - 1];
            ++next_response;

            if (s->response.empty())
                continue; // a server that never answers, which is what a cancellation test wants

            s->sending = s->connection->send(bytes_of(s->response), deadline::never());
        }

        for (auto& s : sessions)
        {
            if (!s->sending.is_valid() || !s->sending->is_ready() || !s->connection.is_valid())
                continue;

            s->sending = {};

            // The client says which it wants; the flag is how a test makes the server disagree.
            auto const client_asked_to_close = last_request.contains("Connection: close");
            if (close_after_response || client_asked_to_close || s->response.contains("Connection: close"))
            {
                s->connection->close();
                s->connection = {};
                continue;
            }

            // Keep-alive: wait for the next request on the same connection.
            s->receiving = s->connection->receive(s->inbox, deadline::never());
        }
    }
};

/// A virtual network, a resolver that answers for one name, a server, and the client under test.
struct client_fixture
{
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<resolver> res;
    cc::unique_ptr<scripted_server> server;
    cc::unique_ptr<native_http_client> client;

    ip_address address = ip_address::parse("10.0.0.1").value();

    explicit client_fixture(cc::vector<cc::string> responses, i32 port = 80)
    {
        io = io_system::create({.unthreaded = true});
        net = cc::make_unique<virtual_network>(*io);

        auto const answer = address;
        res = resolver::create(*io, {.lookup = [answer](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                     { return cc::vector<ip_address>{answer}; }});

        server = cc::make_unique<scripted_server>();
        server->responses = cc::move(responses);
        server->listener = stream_listener::try_create(*net, endpoint(address, port)).value();
        server->listen_again();

        client = cc::make_unique<native_http_client>(*net, *res);
    }

    /// Drive both ends until the request settles.
    ///
    /// One round is one step of the server and one sweep of the pump, which is all either side needs: nothing here
    /// waits on the world, so a run that does not finish promptly is a bug rather than a slow machine.
    /// Drive both ends until `done`, against the wall clock.
    ///
    /// A round count would be a budget in scheduler slices rather than in time, and these tests share the machine
    /// with every other test in the binary -- so a fixed number of rounds is a different amount of work on every
    /// host CI runs on.
    [[nodiscard]] bool run_until(cc::function_ref<bool()> done, f64 budget_secs = 10.0)
    {
        auto& clk = system_clock();
        auto const deadline_ns = clk.now_ns() + i64(budget_secs * 1e9);

        while (true)
        {
            if (done())
                return true;
            if (clk.now_ns() >= deadline_ns)
            {
                cc::report_all_thread_stacks("a cnet test waited out its budget");
                return false;
            }
            server->step();
            (void)cc::thread_pump_all();
        }
    }

    /// Give both ends a few turns, for a test that wants the request under way rather than finished.
    ///
    /// A round count is right here and only here: nothing is being waited FOR, so there is no budget to get wrong.
    void run_briefly(i32 rounds = 20)
    {
        for (i32 i = 0; i < rounds; ++i)
        {
            server->step();
            (void)cc::thread_pump_all();
        }
    }
};
} // namespace

TEST("cnet - a GET goes out and a response comes back")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "hello, client"});

    auto response = http_get(*fixture.client, "http://example.test/greeting");
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);

    CHECK(response->value().status() == 200);
    CHECK(response->value().is_success());
    CHECK(response->value().body_text() == "hello, client");
    CHECK(response->value().head.headers.get("Content-Type").value() == "text/plain");

    // The request line, and the Host the client took from the target.
    CHECK(fixture.server->last_request.contains("GET /greeting HTTP/1.1\r\n"));
    CHECK(fixture.server->last_request.contains("Host: example.test\r\n"));

    // No `Connection: close`: the connection is meant to be kept, which is what pooling needs the server to know.
    CHECK(!fixture.server->last_request.contains("Connection: close"));
}

TEST("cnet - a chunked response is delivered whole")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\n"
                                   "Transfer-Encoding: chunked\r\n"
                                   "\r\n"
                                   "6\r\nchunk1\r\n"
                                   "6\r\nchunk2\r\n"
                                   "0\r\n\r\n"});

    auto response = http_get(*fixture.client, "http://example.test/");
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().body_text() == "chunk1chunk2");
}

TEST("cnet - a POST carries its body and its headers")
{
    auto fixture = client_fixture({"HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n"});

    auto const payload = cc::string_view("{\"a\":1}");

    auto request = http_request();
    request.method = http_method::post;
    request.target = http_target::parse("http://example.test/items").value();
    request.headers.add("Content-Type", "application/json");
    request.headers.add("Content-Length", cc::format("{}", payload.size()));
    request.body = borrowed_body(payload);

    auto response = http_send(*fixture.client, cc::move(request));
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status() == 201);

    CHECK(fixture.server->last_request.contains("POST /items HTTP/1.1\r\n"));
    CHECK(fixture.server->last_request.contains("Content-Type: application/json\r\n"));
    CHECK(fixture.server->last_request.contains(payload));
}

TEST("cnet - a redirect is followed, and the second request is a GET")
{
    auto fixture = client_fixture({"HTTP/1.1 302 Found\r\n"
                                   "Location: /elsewhere\r\n"
                                   "Content-Length: 5\r\n"
                                   "\r\n"
                                   "ignore",
                                   "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 5\r\n"
                                   "\r\n"
                                   "there"});

    auto const payload = cc::string_view("data");

    auto request = http_request();
    request.method = http_method::post;
    request.target = http_target::parse("http://example.test/start").value();
    request.headers.add("Content-Length", cc::format("{}", payload.size()));
    request.body = borrowed_body(payload);

    auto response = http_send(*fixture.client, cc::move(request));
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);

    // Only the final response's body reaches the caller: the redirect's is read and thrown away.
    CHECK(response->value().status() == 200);
    CHECK(response->value().body_text() == "there");

    // A 302 turns a POST into a GET without a body, which is what every client does and what servers assume.
    CHECK(fixture.server->last_request.contains("GET /elsewhere HTTP/1.1\r\n"));
    CHECK(!fixture.server->last_request.contains("data"));
}

TEST("cnet - a redirect loop stops at the limit")
{
    auto fixture = client_fixture({"HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n"});

    auto response = http_get(*fixture.client, "http://example.test/", {.max_redirects = 2});
    CHECK(fixture.run_until([&] { return response->is_ready(); }));

    // The last redirect is handed back rather than followed, so a caller can see where it stopped.
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status() == 302);
}

TEST("cnet - redirects can be turned off")
{
    auto fixture = client_fixture({"HTTP/1.1 301 Moved\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n"});

    auto response = http_get(*fixture.client, "http://example.test/old", {.follow_redirects = false});
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status() == 301);
    CHECK(response->value().head.headers.get("Location").value() == "/new");
}

TEST("cnet - a body over the cap is refused rather than buffered")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 100\r\n"
                                   "\r\n"
                                   "0123456789012345678901234567890123456789"});

    // The declared length is enough to refuse it: nothing is buffered before the decision.
    auto response = http_get(*fixture.client, "http://example.test/big", {.max_body_bytes = 10});
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() != nullptr);
}

TEST("cnet - a sink that pushes back stops the reading and is charged only what it took")
{
    // Long enough that it cannot arrive in one chunk, and a cap barely above its real length: if the cap counted what
    // the sink was OFFERED rather than what it took, re-offering the same bytes would blow it long before the end.
    auto body = cc::string();
    for (auto i = 0; i < 400; ++i)
        body += "0123456789";

    auto fixture = client_fixture({cc::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}", body.size(), body)});

    // A sink that takes a fixed bite and then refuses, so it genuinely pushes back rather than draining the buffer a
    // half at a time -- which a sink taking a FRACTION would do, geometrically, inside one parse.
    struct slow_consumer
    {
        cc::string taken;
        bool may_take = true;
        i32 offers = 0;
        resume_body flow;
    };

    auto consumer = cc::make_shared<slow_consumer>();

    auto request
        = http_request{.method = http_method::get, .target = http_target::parse("http://example.test/slow").value()};

    auto head = fixture.client->send_streaming(cc::move(request),
                                               [consumer](cc::span<byte const> chunk, resume_body const& f) -> isize
                                               {
                                                   ++consumer->offers;
                                                   consumer->flow = f;

                                                   if (!consumer->may_take)
                                                       return 0;

                                                   consumer->may_take = false;
                                                   auto const n = chunk.size() < isize(64) ? chunk.size() : isize(64);
                                                   for (isize i = 0; i < n; ++i)
                                                       consumer->taken.push_back(char(chunk[i]));
                                                   return n;
                                               },
                                               {.max_body_bytes = isize(body.size()) + 16}, {});

    // Nothing more is read while the sink is refusing, which is the backpressure: without it the request would keep
    // pulling bytes off the connection and pile them up in a buffer of ours.
    // `run_briefly` rather than a budget: nothing is being waited FOR, so a turn count is the whole answer.
    fixture.run_briefly(200);
    CHECK(!head->is_ready());

    auto const stalled_at = consumer->offers;
    fixture.run_briefly(200);
    CHECK(consumer->offers == stalled_at);
    CHECK(isize(consumer->taken.size()) < isize(body.size()));

    // Resuming carries it to the end, sixty-four bytes at a time -- and the cap counts what the sink TOOK, so the
    // bytes re-offered after every refusal are not charged again.
    CHECK(fixture.run_until(
        [&]
        {
            consumer->may_take = true;
            consumer->flow.resume();
            return head->is_ready();
        }));

    REQUIRE(head->try_error() == nullptr);
    CHECK(head->value().status == 200);
    CHECK(cc::string_view(consumer->taken) == cc::string_view(body));
}

TEST("cnet - a streaming response reaches the sink as it arrives")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\n"
                                   "Transfer-Encoding: chunked\r\n"
                                   "\r\n"
                                   "5\r\nfirst\r\n"
                                   "6\r\nsecond\r\n"
                                   "0\r\n\r\n"});

    auto chunks = cc::make_shared<cc::vector<cc::string>>();

    auto request = http_request();
    request.target = http_target::parse("http://example.test/stream").value();

    auto head = fixture.client->send_streaming(
        cc::move(request),
        [chunks](cc::span<byte const> chunk, resume_body const&) -> isize
        {
            chunks->push_back(cc::string(cc::string_view(reinterpret_cast<char const*>(chunk.data()), chunk.size())));
            return chunk.size();
        },
        {}, {});

    CHECK(fixture.run_until([&] { return head->is_ready(); }));
    CHECK(head->try_error() == nullptr);
    CHECK(head->value().status == 200);

    // One call per chunk, in order: the sink sees the shape the server sent rather than one reassembled blob.
    CHECK(chunks->size() == 2);
    CHECK((*chunks)[0] == "first");
    CHECK((*chunks)[1] == "second");
}

TEST("cnet - a malformed response fails the request rather than being repaired")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 5\r\n"
                                   "Transfer-Encoding: chunked\r\n"
                                   "\r\n"
                                   "hello"});

    // Framed two ways at once, which is the request smuggling primitive the parser refuses.
    auto response = http_get(*fixture.client, "http://example.test/");
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() != nullptr);
}

TEST("cnet - a URL the client cannot fetch fails before anything happens")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"});

    auto const relative = http_get(*fixture.client, "/no/scheme");
    CHECK(relative->is_ready());
    CHECK(relative->try_error() != nullptr);

    auto const wrong_scheme = http_get(*fixture.client, "ftp://example.test/x");
    CHECK(wrong_scheme->is_ready());
    CHECK(wrong_scheme->try_error() != nullptr);
}

TEST("cnet - cancelling a request in flight ends it")
{
    // A server that never answers: the connection is made and then nothing comes back.
    auto fixture = client_fixture({""});

    auto const token = cancel_token::create();
    auto response = http_get(*fixture.client, "http://example.test/slow", {.timeout = deadline::never()}, token);

    CHECK(!response->is_ready());
    fixture.run_briefly();

    token.cancel();
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() != nullptr);
    CHECK(response->try_error()->is_cancelled());
}

TEST("cnet - a second request to the same origin reuses the connection")
{
    auto fixture = client_fixture(
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst", "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond"});

    auto first = http_get(*fixture.client, "http://example.test/one");
    CHECK(fixture.run_until([&] { return first->is_ready(); }));
    CHECK(first->try_error() == nullptr);
    CHECK(first->value().body_text() == "first");

    // The connection went back to the pool rather than being closed, which is the whole point.
    CHECK(fixture.client->pool().idle_count() == 1);

    auto second = http_get(*fixture.client, "http://example.test/two");
    CHECK(fixture.run_until([&] { return second->is_ready(); }));
    CHECK(second->try_error() == nullptr);
    CHECK(second->value().body_text() == "second");

    // One accept for two requests: the second paid no connect and, over https, would have paid no handshake.
    CHECK(fixture.server->accept_count == 1);
    CHECK(fixture.server->request_count == 2);
    CHECK(!fixture.server->last_request.contains("Connection: close"));
}

TEST("cnet - a request can refuse to share a connection")
{
    auto fixture = client_fixture(
        {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd"});

    auto first = http_get(*fixture.client, "http://example.test/one", {.reuse_connections = false});
    CHECK(fixture.run_until([&] { return first->is_ready(); }));
    CHECK(first->try_error() == nullptr);

    // Nothing was kept, and the request said so on the wire so the server did not hold one open either.
    CHECK(fixture.client->pool().idle_count() == 0);
    CHECK(fixture.server->last_request.contains("Connection: close"));

    auto second = http_get(*fixture.client, "http://example.test/two", {.reuse_connections = false});
    CHECK(fixture.run_until([&] { return second->is_ready(); }));
    CHECK(second->try_error() == nullptr);
    CHECK(fixture.server->accept_count == 2);
}

TEST("cnet - a pooled connection the server already closed is retried on a fresh one")
{
    auto fixture = client_fixture(
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst", "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond"});

    // The server answers as though the connection will live on, and then hangs up anyway -- which is a server's
    // right, and invisible to a client holding the other end.
    fixture.server->close_after_response = true;

    auto first = http_get(*fixture.client, "http://example.test/one");
    CHECK(fixture.run_until([&] { return first->is_ready(); }));
    CHECK(first->try_error() == nullptr);

    auto second = http_get(*fixture.client, "http://example.test/two");
    CHECK(fixture.run_until([&] { return second->is_ready(); }));

    // The retry is what makes pooling safe: the caller sees a successful request, not the dead connection.
    CHECK(second->try_error() == nullptr);
    CHECK(second->value().body_text() == "second");
    CHECK(fixture.server->accept_count == 2);
}

TEST("cnet - a connection is not kept when the response leaves the stream unclean")
{
    auto fixture = client_fixture({"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc"});

    auto response = http_get(*fixture.client, "http://example.test/");
    CHECK(fixture.run_until([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);

    // The server said it was closing, so keeping the connection would be keeping a dead one.
    CHECK(fixture.client->pool().idle_count() == 0);
}
