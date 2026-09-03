#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/http/http_client.hh>
#include <clean-net/http/http_server.hh>
#include <clean-net/http/polite_client.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The HTTP client, against a server this example starts on loopback.
//
// A real URL would work exactly the same way -- swap the address in and nothing else changes -- but an example that
// needs the internet is one that fails for reasons that teach nobody anything.

namespace
{
/// Wait for one async, driving the reactor while we do.
///
/// **This is the integration story, not a detail of the example.**
/// An unthreaded io_system runs on whatever already calls `cc::thread_pump_all()`, which in an application is the
/// frame loop and here is this function.
/// A threaded one would need none of this; nothing in the API changes either way.
template <class T>
void await(cc::shared_async<T> const& a)
{
    while (!a->is_ready())
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
}

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

EXAMPLE("clean-net/download")
{
    auto io = cnet::io_system::create({.unthreaded = true});
    auto const server = start_server(*io);
    auto const base = cc::format("http://127.0.0.1:{}", server->local().port);

    // The client owns the transport and the resolver it needs; `native_http_client` is the form that takes yours.
    auto client = cnet::make_http_client(*io).value();

    // ---- the whole body, buffered ------------------------------------------------------------------

    auto response = cnet::http_get(*client, cc::format("{}/hello", base));
    await(response);

    cc::println("GET /hello -> {} {}", response->value().status(), response->value().body_text());
    cc::println("  content-type: {}", response->value().head.headers.get("Content-Type").value());

    // ---- a body that never lands in memory ---------------------------------------------------------

    // The sink runs on the reactor thread and its RETURN VALUE is the backpressure: taking fewer bytes than offered
    // stops the transport reading more, all the way down to the socket.
    // So hand the bytes on and do no work here.
    auto received = i64(0);
    auto chunks = i64(0);

    auto request = cnet::http_request{.method = cnet::http_method::get,
                                      .target = cnet::http_target::parse(cc::format("{}/big", base)).value()};

    auto streamed = client->send_streaming(cc::move(request),
                                           [&](cc::span<byte const> chunk)
                                           {
                                               received += chunk.size();
                                               ++chunks;
                                               return chunk.size();
                                           },
                                           {}, {});
    await(streamed);

    cc::println("");
    cc::println("GET /big  -> {} in {} chunks, {} bytes, none of them kept", streamed->value().status, chunks, received);

    // ---- a redirect, followed ----------------------------------------------------------------------

    auto redirected = cnet::http_get(*client, cc::format("{}/moved", base));
    await(redirected);

    // The status is the one the request ended on, not the 302 on the way.
    cc::println("");
    cc::println("GET /moved -> {} {}", redirected->value().status(), redirected->value().body_text());

    // ---- a failure, which is a value ---------------------------------------------------------------

    auto missing = cnet::http_get(*client, cc::format("{}/nope", base));
    await(missing);
    cc::println("GET /nope  -> {}", missing->value().status());

    // A connection nobody is listening for fails the async rather than the status: there is no response to have one.
    auto refused = cnet::http_get(*client, "http://127.0.0.1:1/never", {.timeout = cnet::deadline::after_secs(2)});
    await(refused);
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
        await(one);

    cc::println("");
    cc::println("5 polite requests, 2 at a time, all {}", batch[0]->value().status());
    cc::println("the server saw {} requests on {} connection(s)", server->requests_handled(), server->open_connections());
}
