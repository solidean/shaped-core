#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/http/http_server.hh>
#include <clean-net/http/polite_client.hh>
#include <nexus/async-test.hh>

using namespace cc::primitive_defines;

// The HTTP client, against a server this example starts on loopback.
//
// A real URL would work exactly the same way -- swap the address in and nothing else changes -- but an example that
// needs the internet is one that fails for reasons that teach nobody anything.
//
// **Awaiting is the integration story, not a detail of the example.**
// The io_system has a thread of its own, so nothing here pumps: the body awaits, and resumes on the main thread its
// ASYNC_EXAMPLE is homed to.
// An unthreaded one belongs to a loop that drives it instead — a frame loop calling `cc::thread_pump_all()` — and
// nothing in the API changes either way.
// A plain `co_await` short-circuits on a failed request; the refused connection below is the one place a failure is the
// demonstration, so it waits with `cc::async_settled` and reads the error itself.

namespace
{
/// Something to talk to: three routes and nothing more.
[[nodiscard]] cc::unique_ptr<cnet::http_server> start_server(cnet::io_system& io)
{
    auto server = cnet::http_server::try_create(io).value();

    server->route(cnet::http_method::get, "/hello", [](cnet::http_server_request const&)
                  { return cnet::http_server_response::text("hello from the dev server"); });

    // Something long enough that the streaming download below has more than one chunk to report.
    server->route(cnet::http_method::get, "/big",
                  [](cnet::http_server_request const&)
                  {
                      auto body = cc::string();
                      for (auto i = 0; i < 2000; ++i)
                          body += cc::format("line {} of a body nobody wants in memory all at once\n", i);
                      return cnet::http_server_response::text(body);
                  });

    server->route(cnet::http_method::get, "/moved",
                  [](cnet::http_server_request const&)
                  {
                      auto response = cnet::http_server_response::empty(302);
                      response.headers.set("Location", "/hello");
                      return response;
                  });

    return cc::move(server);
}
} // namespace

ASYNC_EXAMPLE("clean-net/download")
{
    auto io = cnet::io_system::create({});
    auto const server = start_server(*io);
    auto const base = cc::format("http://127.0.0.1:{}", server->local().port);

    // The client owns the transport and the resolver it needs; `native_http_client` is the form that takes yours.
    auto client = cnet::make_http_client(*io).value();

    // ---- the whole body, buffered ------------------------------------------------------------------

    auto const response = co_await cnet::http_get(*client, cc::format("{}/hello", base));

    cc::println("GET /hello -> {} {}", response.status(), response.body_text());
    cc::println("  content-type: {}", response.head.headers.get("Content-Type").value());

    // ---- a body that never lands in memory ---------------------------------------------------------

    // The sink runs on the reactor thread and its RETURN VALUE is the backpressure: taking fewer bytes than offered
    // stops the request reading more until the `resume_body` it was handed says otherwise.
    // This one takes everything, so it never pushes back and never needs the flow.
    // Either way, hand the bytes on and do no work here.
    auto received = i64(0);
    auto chunks = i64(0);

    auto request = cnet::http_request{.method = cnet::http_method::get,
                                      .target = cnet::http_target::parse(cc::format("{}/big", base)).value()};

    auto const streamed = co_await client->send_streaming(cc::move(request),
                                                          [&](cc::span<byte const> chunk, cnet::resume_body const&)
                                                          {
                                                              received += chunk.size();
                                                              ++chunks;
                                                              return chunk.size();
                                                          },
                                                          {}, {});

    cc::println("");
    cc::println("GET /big  -> {} in {} chunks, {} bytes, none of them kept", streamed.status, chunks, received);

    // ---- a redirect, followed ----------------------------------------------------------------------

    auto const redirected = co_await cnet::http_get(*client, cc::format("{}/moved", base));

    // The status is the one the request ended on, not the 302 on the way.
    cc::println("");
    cc::println("GET /moved -> {} {}", redirected.status(), redirected.body_text());

    // ---- a failure, which is a value ---------------------------------------------------------------

    auto const missing = co_await cnet::http_get(*client, cc::format("{}/nope", base));
    cc::println("GET /nope  -> {}", missing.status());

    // A connection nobody is listening for fails the async rather than the status: there is no response to have one.
    auto refused = cnet::http_get(*client, "http://127.0.0.1:1/never", {.timeout = cnet::deadline::after_secs(2)});
    co_await cc::async_settled(refused);
    cc::println("GET :1     -> failed: {}", refused->try_error() != nullptr ? "yes" : "no");

    // ---- politeness, which is where retries live ---------------------------------------------------

    // A decorator over any client.
    // Retries live WITH the rate limit on purpose: a retry policy without one is how a transient failure becomes a
    // self-inflicted denial of service.
    auto polite = cnet::polite_http_client(*client, *io, {.requests_per_second = 20, .max_concurrent_requests = 2});

    auto batch = cc::vector<cc::shared_async<cnet::http_response>>();
    for (auto i = 0; i < 5; ++i)
        batch.push_back(cnet::http_get(polite, cc::format("{}/hello", base)));

    for (auto const& one : batch)
        (void)co_await one;

    cc::println("");
    cc::println("5 polite requests, 2 at a time, all {}", batch[0]->value().status());
    cc::println("the server saw {} requests on {} connection(s)", server->routed_requests(), server->open_connections());
}
