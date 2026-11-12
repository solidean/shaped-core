#include "cnet-test-types.hh"

#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/http/fetch_http_client.hh>
#include <clean-net/io/io_system.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#endif

using namespace cc::primitive_defines;
using namespace cnet;

// The fetch backend against a `fetch` this test writes: every request is answered by JavaScript in the same process,
// so the whole path -- the proxy to the main thread, the answers, the reactor stepping them -- runs under node.

namespace
{
using cnet::cc_rec_domain;

[[nodiscard]] cc::string failure_text(cc::async_error const* e)
{
    return e == nullptr ? cc::string() : e->underlying().to_string();
}
} // namespace

#if !defined(__EMSCRIPTEN__)
CNET_IO_TEST("cnet fetch - off wasm the client exists and refuses every request")
{
    CHECK(!fetch_http_client::is_supported());

    auto io = io_system::create({.unthreaded = true});
    auto client = fetch_http_client(*io);
    CHECK(client.level() == http_level::fetch);
    CHECK(!fetch_http_client::document_url().has_value());

    auto const response = http_get(client, "http://example.test/");
    REQUIRE(response->is_ready());
    CHECK(failure_text(response->try_error()).contains("only on wasm"));
}
#else

namespace
{
enum class mock : int
{
    /// 200 with two headers and a body in three chunks; the request is recorded.
    answers = 1,

    /// Rejects the way a browser does on a refused connection or a CORS refusal.
    rejects = 2,

    /// Never answers, and records an abort.
    hangs = 3,

    /// 64 bytes in four chunks, declaring no length.
    streams_64_bytes = 4,

    /// Declares a length of 1000 and sends nothing.
    declares_1000_bytes = 5,
};

EM_JS_DEPS(cnet_test, "$UTF8ToString");

EM_JS(void, cnet_test_install_fetch, (int mode), {
    var state = globalThis.cnetFetchMock = {aborted : false, method : "", url : "", header : ""};
    var stream_of = function(chunks)
    {
        var i = 0;
        return new ReadableStream({
            pull : function(controller) {
                if (i < chunks.length)
                    controller.enqueue(new TextEncoder().encode(chunks[i++]));
                else
                    controller.close();
    }
});
};
globalThis.fetch = function(url, init)
{
    state.method = init.method;
    state.url = String(url);
    for (var h of init.headers)
        if (h[0] == "x-request")
            state.header = h[1];
    if (mode == 1)
        return Promise.resolve(new Response(stream_of([ "hello ", "fetch ", "world" ]),
                                            {status : 201, statusText : "Created", headers : {"x-answer" : "yes"}}));
    if (mode == 2)
        return Promise.reject(new TypeError("Failed to fetch"));
    if (mode == 3)
        return new Promise(function(resolve, reject) {
            init.signal.addEventListener(
                "abort", function() {
                    state.aborted = true;
                    reject(new DOMException("aborted", "AbortError"));
                });
        });
    if (mode == 4)
        return Promise.resolve(new Response(
            stream_of([ "0123456789abcdef", "0123456789abcdef", "0123456789abcdef", "0123456789abcdef" ])));
    return Promise.resolve(new Response(stream_of([]), {headers : {"content-length" : "1000"}}));
};
});

EM_JS(int, cnet_test_fetch_was_aborted, (), { return globalThis.cnetFetchMock.aborted ? 1 : 0; });

EM_JS(int, cnet_test_fetch_saw, (char const* method, char const* url, char const* header), {
    var s = globalThis.cnetFetchMock;
    return s.method == UTF8ToString(method) && s.url == UTF8ToString(url) && s.header == UTF8ToString(header) ? 1 : 0;
});

void install_fetch(mock m)
{
    cnet_test_install_fetch(int(m));
}

[[nodiscard]] bool fetch_was_aborted()
{
    return cnet_test_fetch_was_aborted() != 0;
}

[[nodiscard]] bool fetch_saw(char const* method, char const* url, char const* header)
{
    return cnet_test_fetch_saw(method, url, header) != 0;
}
} // namespace

ASYNC_TEST("cnet fetch - a response arrives with its status, headers and every chunk in order",
           nx::config::main_thread,
           nx::config::exclusive("cnet-io"))
{
    install_fetch(mock::answers);
    auto io = io_system::create();
    auto client = fetch_http_client(*io);

    auto request
        = http_request{.method = http_method::post, .target = http_target::parse("http://example.test/a?b").value()};
    request.headers.add("x-request", "sent");
    auto const response = http_send(client, cc::move(request));
    co_await cc::async_settled(response);

    REQUIRE(response->try_error() == nullptr);
    CHECK(response->value().status() == 201);
    CHECK(response->value().head.reason == "Created");
    CHECK(response->value().head.headers.get("X-Answer") == cc::optional<cc::string_view>("yes"));
    CHECK(response->value().body_text() == "hello fetch world");
    CHECK(fetch_saw("POST", "http://example.test/a?b", "sent"));
}

ASYNC_TEST("cnet fetch - a sink that takes a little at a time still gets every byte, in order",
           nx::config::main_thread,
           nx::config::exclusive("cnet-io"))
{
    install_fetch(mock::answers);
    auto io = io_system::create();
    auto client = fetch_http_client(*io);

    // Four bytes per offer, resuming before it returns: the rest of each chunk is re-offered after a wake-up rather
    // than in a loop, which is the path a slow consumer on another thread takes.
    auto received = cc::make_shared<cc::string>();
    auto offers = cc::make_shared<i32>(0);
    auto const head
        = client.send_streaming({.target = http_target::parse("http://example.test/").value()},
                                [received, offers](cc::span<byte const> chunk, resume_body const& flow) -> isize
                                {
                                    ++*offers;
                                    auto const taken = chunk.size() < 4 ? chunk.size() : isize(4);
                                    for (auto i = isize(0); i < taken; ++i)
                                        *received += char(chunk[i]);
                                    if (taken < chunk.size())
                                        flow.resume();
                                    return taken;
                                },
                                {}, {});
    co_await cc::async_settled(head);

    REQUIRE(head->try_error() == nullptr);
    CHECK(*received == "hello fetch world");
    CHECK(*offers == 6); // ceil(6 / 4) + ceil(6 / 4) + ceil(5 / 4)
}

ASYNC_TEST("cnet fetch - a rejected fetch fails the request", nx::config::main_thread, nx::config::exclusive("cnet-io"))
{
    install_fetch(mock::rejects);
    auto io = io_system::create();
    auto client = fetch_http_client(*io);

    auto const response = http_get(client, "http://example.test/");
    co_await cc::async_settled(response);
    CHECK(failure_text(response->try_error()).contains("Failed to fetch"));
}

ASYNC_TEST("cnet fetch - the budget ends a request that never answers, and aborts the fetch",
           nx::config::main_thread,
           nx::config::exclusive("cnet-io"))
{
    install_fetch(mock::hangs);
    auto clk = manual_clock(0);
    auto io = io_system::create({.time_source = &clk});
    auto client = fetch_http_client(*io);

    auto const response = http_get(client, "http://example.test/", {.timeout = deadline::after_ms(100)});
    clk.advance_ms(200);
    co_await cc::async_settled(response);

    CHECK(failure_text(response->try_error()).contains("ran out of time"));
    CHECK(fetch_was_aborted());
}

ASYNC_TEST("cnet fetch - a cancelled token ends the request and aborts the fetch",
           nx::config::main_thread,
           nx::config::exclusive("cnet-io"))
{
    install_fetch(mock::hangs);
    auto io = io_system::create();
    auto client = fetch_http_client(*io);

    auto const token = cancel_token::create();
    auto const response = http_get(client, "http://example.test/", {}, token);
    token.cancel();
    co_await cc::async_settled(response);

    REQUIRE(response->try_error() != nullptr);
    CHECK(response->try_error()->is_cancelled());
    CHECK(fetch_was_aborted());
}

ASYNC_TEST("cnet fetch - the body cap holds, whether the length is declared or only streamed",
           nx::config::main_thread,
           nx::config::exclusive("cnet-io"))
{
    auto io = io_system::create();
    auto client = fetch_http_client(*io);

    install_fetch(mock::declares_1000_bytes);
    auto const declared = http_get(client, "http://example.test/", {.max_body_bytes = 100});
    co_await cc::async_settled(declared);
    CHECK(failure_text(declared->try_error()).contains("declares 1000"));

    install_fetch(mock::streams_64_bytes);
    auto const streamed = http_get(client, "http://example.test/", {.max_body_bytes = 40});
    co_await cc::async_settled(streamed);
    CHECK(failure_text(streamed->try_error()).contains("exceeds the 40"));

    auto const fits = http_get(client, "http://example.test/", {.max_body_bytes = 64});
    co_await cc::async_settled(fits);
    REQUIRE(fits->try_error() == nullptr);
    CHECK(fits->value().body.size() == 64);
}

ASYNC_TEST("cnet fetch - what a browser cannot do is refused rather than degraded",
           nx::config::main_thread,
           nx::config::exclusive("cnet-io"))
{
    install_fetch(mock::answers);
    auto io = io_system::create();
    auto client = fetch_http_client(*io);

    auto const manual_redirects = http_get(client, "http://example.test/", {.follow_redirects = false});
    co_await cc::async_settled(manual_redirects);
    CHECK(failure_text(manual_redirects->try_error()).contains("follows redirects itself"));

    auto request = http_request{.target = http_target::parse("http://example.test/").value()};
    request.headers.add("x-request", "two\nlines");
    auto const split_header = http_send(client, cc::move(request));
    co_await cc::async_settled(split_header);
    CHECK(failure_text(split_header->try_error()).contains("line break"));
}
#endif
