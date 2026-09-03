#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/http/polite_client.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// Rate limiting and retries, with no server underneath at all.
//
// The client below stands in for one: it records what it was asked and answers from a script, which is what makes
// "the third attempt succeeds" and "two requests were a second apart" exact rather than approximate.
// The clock is manual, so a 250 ms backoff costs nothing and a token bucket refills when the test says so.

namespace
{
/// Wait for something that should happen, against the wall clock.
///
/// A round count would be a budget in scheduler slices rather than in time, and these tests share the process-wide
/// pump with every other test running beside them -- so under load a fixed number of rounds is a different amount of
/// work each run, which is a flaky test rather than a strict one.
/// The assertions that matter here are the counts and the order; nothing below asserts on how long the waiting took.
bool pump_for(cc::function_ref<bool()> done, f64 budget_secs = 5.0)
{
    auto& clk = system_clock();
    auto const deadline_ns = clk.now_ns() + i64(budget_secs * 1e9);

    while (true)
    {
        if (done())
            return true;
        if (clk.now_ns() >= deadline_ns)
            return false;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
}

/// Give something a chance to happen, for a test that expects it not to.
///
/// A round count is right here: the point is to hand the machinery every opportunity and then assert nothing moved.
bool pump_briefly(cc::function_ref<bool()> done, i32 rounds = 200)
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

/// A client that answers from a script and counts what it was asked.
///
/// It never touches a connection, which is the point: politeness and retries are policy, and this is what policy
/// looks like with the protocol taken away.
struct scripted_client final : http_client
{
    struct scripted_response
    {
        i32 status = 200;
        cc::string retry_after;
        bool fail = false;
        cc::string body;
    };

    cc::vector<scripted_response> script;
    isize next = 0;
    isize calls = 0;

    /// Set to hold every request open instead of answering, so a test can watch the concurrency cap.
    bool hold = false;
    cc::vector<cc::shared_async<http_response_head>> held;

    [[nodiscard]] http_level level() const override { return http_level::client; }

    [[nodiscard]] cc::shared_async<http_response_head> send_streaming(http_request,
                                                                      body_sink sink,
                                                                      request_options const&,
                                                                      cancel_token const&) override
    {
        ++calls;

        auto promise = cc::make_async_manual<http_response_head>();
        if (hold)
        {
            held.push_back(promise);
            return promise;
        }

        // The last entry repeats, and an empty script means a plain 200 -- a test that cares says so.
        auto const answer
            = script.empty() ? scripted_response() : script[next < script.size() ? next : script.size() - 1];
        ++next;

        if (answer.fail)
        {
            promise->push_error(cc::async_error::make_error(cc::any_error(error{.code = error_code::connection_reset,
                                                                                .native_code = 0,
                                                                                .message = cc::string("the connection "
                                                                                                      "failed")})));
            return promise;
        }

        if (!answer.body.empty())
        {
            auto const bytes
                = cc::span<byte const>(reinterpret_cast<byte const*>(answer.body.data()), answer.body.size());
            (void)sink(bytes);
        }

        auto head = http_response_head();
        head.status = answer.status;
        if (!answer.retry_after.empty())
            head.headers.add("Retry-After", answer.retry_after);

        promise->push_value(cc::move(head));
        return promise;
    }

    void answer_held(i32 status = 200)
    {
        // Taken first: pushing a value runs continuations, and one of them starts the request that was waiting --
        // which lands back in `held` while it is being iterated.
        auto pending = cc::move(held);
        held = {};

        for (auto& promise : pending)
        {
            auto head = http_response_head();
            head.status = status;
            promise->push_value(cc::move(head));
        }
    }
};

struct politeness_fixture
{
    manual_clock clk = manual_clock(0);
    cc::unique_ptr<io_system> io;
    scripted_client under;
    cc::unique_ptr<polite_http_client> client;

    explicit politeness_fixture(host_policy const& policy)
    {
        io = io_system::create({.unthreaded = true, .time_source = &clk});
        client = cc::make_unique<polite_http_client>(under, *io, policy);
    }

    [[nodiscard]] http_request request_for(cc::string_view url, http_method method = http_method::get)
    {
        auto request = http_request();
        request.method = method;
        request.target = http_target::parse(url).value();
        return request;
    }

    [[nodiscard]] cc::shared_async<http_response_head> send(cc::string_view url, http_method method = http_method::get)
    {
        return client->send_streaming(request_for(url, method), [](cc::span<byte const> c) { return c.size(); }, {}, {});
    }
};
} // namespace

TEST("cnet - a request that succeeds is not retried")
{
    auto fixture = politeness_fixture({});
    fixture.under.script = {{.status = 200}};

    auto response = fixture.send("http://example.test/");
    CHECK(pump_for([&] { return response->is_ready(); }));
    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status == 200);
    CHECK(fixture.under.calls == 1);
}

TEST("cnet - a failed idempotent request is retried after a backoff")
{
    auto fixture = politeness_fixture({.backoff_base_ms = 250, .backoff_jitter = 0});
    fixture.under.script = {{.fail = true}, {.fail = true}, {.status = 200}};

    auto response = fixture.send("http://example.test/");

    // Nothing happens until the clock moves: the backoff is a reactor timer, not a sleep.
    CHECK(pump_for([&] { return fixture.under.calls == 1; }));
    CHECK(!response->is_ready());

    fixture.clk.advance_ms(250);
    CHECK(pump_for([&] { return fixture.under.calls == 2; }));
    CHECK(!response->is_ready());

    // And it doubles, which is what keeps a failing server from being asked at a fixed rate forever.
    fixture.clk.advance_ms(499);
    CHECK(fixture.under.calls == 2);
    fixture.clk.advance_ms(2);
    CHECK(pump_for([&] { return response->is_ready(); }));

    CHECK(response->try_error() == nullptr);
    CHECK(response->value().status == 200);
    CHECK(fixture.under.calls == 3);
}

TEST("cnet - retries stop at the limit and the failure is handed back")
{
    auto fixture = politeness_fixture({.max_retries = 2, .backoff_base_ms = 10, .backoff_jitter = 0});
    fixture.under.script = {{.fail = true}};

    auto response = fixture.send("http://example.test/");

    for (auto i = 0; i < 5; ++i)
    {
        (void)pump_briefly([&] { return response->is_ready(); });
        fixture.clk.advance_ms(100);
    }
    CHECK(pump_for([&] { return response->is_ready(); }));

    CHECK(response->try_error() != nullptr);

    // One attempt plus two retries, and not one more.
    CHECK(fixture.under.calls == 3);
}

TEST("cnet - a POST is not retried, because sending it twice is a second order")
{
    auto fixture = politeness_fixture({.backoff_base_ms = 10, .backoff_jitter = 0});
    fixture.under.script = {{.fail = true}, {.status = 200}};

    auto response = fixture.send("http://example.test/", http_method::post);
    CHECK(pump_for([&] { return response->is_ready(); }));

    CHECK(response->try_error() != nullptr);
    CHECK(fixture.under.calls == 1);
}

TEST("cnet - a 429 is waited out for exactly as long as it asked")
{
    auto fixture = politeness_fixture({.backoff_base_ms = 10, .backoff_jitter = 0});
    fixture.under.script = {{.status = 429, .retry_after = "2"}, {.status = 200}};

    auto response = fixture.send("http://example.test/");
    CHECK(pump_for([&] { return fixture.under.calls == 1; }));

    // The backoff would have been 10 ms; the server said two seconds, and a 429 means wait rather than retry harder.
    fixture.clk.advance_ms(1'000);
    (void)pump_briefly([&] { return response->is_ready(); });
    CHECK(fixture.under.calls == 1);

    fixture.clk.advance_ms(1'001);
    CHECK(pump_for([&] { return response->is_ready(); }));
    CHECK(response->value().status == 200);
    CHECK(fixture.under.calls == 2);
}

TEST("cnet - a 503 is retried and a 404 is not")
{
    auto retried = politeness_fixture({.backoff_base_ms = 10, .backoff_jitter = 0});
    retried.under.script = {{.status = 503}, {.status = 200}};

    auto server_error = retried.send("http://example.test/");
    CHECK(pump_for([&] { return retried.under.calls == 1; }));
    retried.clk.advance_ms(50);
    CHECK(pump_for([&] { return server_error->is_ready(); }));
    CHECK(server_error->value().status == 200);
    CHECK(retried.under.calls == 2);

    // A 404 is an answer, not a failure: trying again would just ask the same question.
    auto kept = politeness_fixture({.backoff_base_ms = 10, .backoff_jitter = 0});
    kept.under.script = {{.status = 404}};

    auto not_found = kept.send("http://example.test/");
    CHECK(pump_for([&] { return not_found->is_ready(); }));
    CHECK(not_found->value().status == 404);
    CHECK(kept.under.calls == 1);
}

TEST("cnet - a response that reached the caller is never sent twice")
{
    auto fixture = politeness_fixture({.backoff_base_ms = 10, .backoff_jitter = 0});

    // A 503 that carries a body: the bytes are already with the caller, so repeating the request would deliver them
    // a second time rather than instead.
    fixture.under.script = {{.status = 503, .body = "already delivered"}, {.status = 200}};

    auto response = fixture.send("http://example.test/");
    CHECK(pump_for([&] { return response->is_ready(); }));

    CHECK(response->value().status == 503);
    CHECK(fixture.under.calls == 1);
}

TEST("cnet - only so many requests to one host are in flight at once")
{
    auto fixture = politeness_fixture({.max_concurrent_requests = 2});
    fixture.under.script = {{.status = 200}};
    fixture.under.hold = true;

    auto first = fixture.send("http://example.test/a");
    auto second = fixture.send("http://example.test/b");
    auto third = fixture.send("http://example.test/c");

    CHECK(pump_for([&] { return fixture.under.calls == 2; }));

    // The third is parked at the door rather than queued at the server, which is the difference a concurrency cap
    // makes to somebody else's machine.
    CHECK(fixture.under.calls == 2);
    CHECK(fixture.client->in_flight("example.test") == 2);
    CHECK(!third->is_ready());

    fixture.under.answer_held();
    fixture.under.hold = false;
    fixture.clk.advance_ms(10);

    CHECK(pump_for([&] { return third->is_ready(); }));
    CHECK(fixture.under.calls == 3);
    CHECK(fixture.client->in_flight("example.test") == 0);
}

TEST("cnet - the token bucket spaces requests out, and a different host is unaffected")
{
    auto fixture = politeness_fixture({.requests_per_second = 2, .burst = 1});
    fixture.under.script = {{.status = 200}};

    auto first = fixture.send("http://example.test/a");
    CHECK(pump_for([&] { return first->is_ready(); }));
    CHECK(fixture.under.calls == 1);

    // The burst is spent, so the next one waits for a token: half a second at two per second.
    auto second = fixture.send("http://example.test/b");
    (void)pump_briefly([&] { return second->is_ready(); });
    CHECK(!second->is_ready());

    // Another host has its own bucket, and is not made to wait for this one.
    auto elsewhere = fixture.send("http://other.test/a");
    CHECK(pump_for([&] { return elsewhere->is_ready(); }));

    fixture.clk.advance_ms(501);
    CHECK(pump_for([&] { return second->is_ready(); }));
    CHECK(second->try_error() == nullptr);
}

TEST("cnet - a request that spends its budget waiting fails rather than being sent late")
{
    auto fixture = politeness_fixture({.requests_per_second = 1, .burst = 1});
    fixture.under.script = {{.status = 200}};

    auto first = fixture.send("http://example.test/a");
    CHECK(pump_for([&] { return first->is_ready(); }));

    auto queued = fixture.client->send_streaming(fixture.request_for("http://example.test/b"), [](cc::span<byte const> c)
                                                 { return c.size(); }, {.timeout = deadline::after_secs(1)}, {});

    // A second of budget against a token that takes a second to arrive: the deadline covers the queueing, so this
    // fails rather than being sent to a server that stopped waiting for it.
    fixture.clk.advance_ms(1'001);
    CHECK(pump_for([&] { return queued->is_ready(); }));
    CHECK(queued->try_error() != nullptr);
    CHECK(fixture.under.calls == 1);
}
