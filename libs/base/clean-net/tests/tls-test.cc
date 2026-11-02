#include "cnet-test-types.hh"

#include <clean-core/common/profiling.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/impl/trust_store.hh>
#include <clean-net/tls/tls.hh>
#include <clean-net/transport/backend.hh>
#include <clean-net/transport/virtual_transport.hh>
#include <nexus/test.hh>

#include <memory> // std::unique_ptr, to own a backend through its interface -- see .shaped-lint.yml

using namespace cc::primitive_defines;

using namespace cnet;

// A real TLS handshake, with no socket anywhere in it.
//
// Both ends are ours and both run over cnet::virtual_network, which is what the transport seam bought: the record
// layer does not know or care that there is no network, so this is the actual handshake rather than a mock of one.
// The certificate is generated in-process by the fixture, so nothing here depends on a file, a clock skew, or a
// certificate somebody has to remember to renew.

namespace
{
// `using namespace cnet` leaves the recording macros two domains to choose from; the scopes below are cnet's.
using cnet::cc_rec_domain;

bool pump_until(cc::function_ref<bool()> done, i32 rounds = 20000)
{
    CC_RECORD_SCOPE("cnet_test.pump_until");

    for (i32 i = 0; i < rounds; ++i)
    {
        if (done())
            return true;
        if (!cc::thread_pump_all())
            cc::this_thread_yield();
    }
    return done();
}

[[nodiscard]] cc::span<byte const> bytes_of(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
}

/// A connected pair over a virtual network, ready to be wrapped in TLS.
struct tls_fixture
{
    cc::unique_ptr<io_system> io;
    cc::unique_ptr<virtual_network> net;
    cc::unique_ptr<stream_listener> listener;

    cc::shared_async<cc::shared_ptr<stream_connection>> accepted;
    cc::shared_async<cc::shared_ptr<stream_connection>> connected;

    tls_fixture()
    {
        io = io_system::create({.unthreaded = true});
        net = cc::make_unique<virtual_network>(*io);
        listener = stream_listener::try_create(*net, endpoint(ip_address::loopback(ip_family::v4), 0)).value();

        accepted = listener->accept();
        connected = tcp_connect(*net, listener->local());
    }

    [[nodiscard]] bool ready()
    {
        return pump_until([&] { return accepted->is_ready() && connected->is_ready(); })
            && accepted->try_error() == nullptr && connected->try_error() == nullptr;
    }
};

/// A connection that holds its sends while closed, so a test decides when the bytes reach "the OS".
struct send_gate
{
    bool open = true;
    cc::span<byte const> held_bytes;
    cc::shared_async<cc::unit> held_promise;
};

class gated_connection final : public connection_backend
{
public:
    gated_connection(cc::shared_ptr<stream_connection> inner, cc::shared_ptr<send_gate> gate)
      : _inner(cc::move(inner)), _gate(cc::move(gate))
    {
    }

    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d, cancel_token const& token) override
    {
        return _inner->receive(buffer, d, token);
    }

    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes, deadline d, cancel_token const& token) override
    {
        if (_gate->open)
            return _inner->send(bytes, d, token);
        _gate->held_bytes = bytes;
        _gate->held_promise = cc::make_async_manual<cc::unit>();
        return _gate->held_promise;
    }

    /// Lets the held send through, completing it once the connection underneath has.
    void release()
    {
        _gate->open = true;
        if (!_gate->held_promise.is_valid())
            return;
        auto promise = cc::move(_gate->held_promise);
        _gate->held_promise = {};
        impl::when_ready(_inner->send(_gate->held_bytes),
                         [promise](cc::shared_async<cc::unit> const& sent)
                         {
                             if (sent->try_error() != nullptr)
                                 promise->push_error(to_async_error({.code = error_code::connection_reset,
                                                                     .native_code = 0,
                                                                     .message = cc::string("the held send failed")}));
                             else
                                 promise->push_value(cc::unit{});
                         });
    }

    cc::result<cc::unit, error> shutdown_send() override { return _inner->shutdown_send(); }
    [[nodiscard]] endpoint local() const override { return _inner->local(); }
    [[nodiscard]] endpoint peer() const override { return _inner->peer(); }
    [[nodiscard]] bool is_open() const override { return _inner->is_open(); }
    void close() override { _inner->close(); }

private:
    cc::shared_ptr<stream_connection> _inner;
    cc::shared_ptr<send_gate> _gate;
};
} // namespace

CNET_IO_TEST("cnet - TLS reports whether this build has it")
{
    // NOT an assertion that it is supported.
    // The wasm build has no backend and says so rather than pretending, which is the whole reason this is a runtime
    // question -- so what must hold is that the answer and the behaviour agree.
    auto const identity = tls_make_self_signed("localhost");

    if (tls_is_supported())
    {
        CHECK(identity.has_value());
        return;
    }

    REQUIRE(identity.has_error());

    auto const says_so
        = identity.error().code == error_code::unsupported || identity.error().code == error_code::backend_missing;
    CHECK(says_so);
}

CNET_IO_TEST("cnet - a handshake over a virtual network carries bytes both ways")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("localhost").value();
    CHECK(!identity.certificate_chain_pem.empty());
    CHECK(!identity.private_key_pem.empty());

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    // The client trusts the fixture's certificate as a root, which is what a private CA looks like -- rather than
    // trusting anything at all, which is the setting this library makes hard on purpose.
    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side
        = tls_connect(fixture.connected->value(), "localhost",
                      {.trust = {.use_system_roots = false, .additional_roots_pem = {identity.certificate_chain_pem}}});

    CHECK(pump_until([&] { return server_side->is_ready() && client_side->is_ready(); }));
    CHECK(client_side->try_error() == nullptr);
    CHECK(server_side->try_error() == nullptr);

    auto const& client = client_side->value();
    auto const& server = server_side->value();
    CHECK(client->is_open());
    CHECK(server->is_open());

    // Encrypted in one direction...
    auto const request = cc::string_view("GET / HTTP/1.1");
    auto sent = client->send(bytes_of(request));
    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);

    byte inbox[128] = {};
    auto received = server->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return received->is_ready(); }));
    CHECK(received->try_error() == nullptr);
    CHECK(cc::string_view(reinterpret_cast<char const*>(inbox), received->value()) == request);

    // ...and back in the other.
    auto const answer = cc::string_view("HTTP/1.1 200 OK");
    auto replied = server->send(bytes_of(answer));
    CHECK(pump_until([&] { return replied->is_ready(); }));

    byte client_inbox[128] = {};
    auto client_received = client->receive(cc::span<byte>(client_inbox, isize(sizeof(client_inbox))));
    CHECK(pump_until([&] { return client_received->is_ready(); }));
    CHECK(client_received->try_error() == nullptr);
    CHECK(cc::string_view(reinterpret_cast<char const*>(client_inbox), client_received->value()) == answer);
}

CNET_IO_TEST("cnet - a TLS send completes once its records are handed on, not once they are encrypted")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("localhost").value();
    auto fixture = tls_fixture();
    REQUIRE(fixture.ready());

    auto gate = cc::make_shared<send_gate>();
    auto backend = std::make_unique<gated_connection>(fixture.connected->value(), gate);
    auto* const gated = backend.get();
    auto const under = cc::make_shared<stream_connection>(cc::move(backend));

    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side
        = tls_connect(under, "localhost",
                      {.trust = {.use_system_roots = false, .additional_roots_pem = {identity.certificate_chain_pem}}});
    REQUIRE(pump_until([&] { return server_side->is_ready() && client_side->is_ready(); }));
    REQUIRE(client_side->try_error() == nullptr);

    // with the socket send held, the bytes are encrypted at once and still not with the OS
    gate->open = false;
    auto const request = cc::string_view("GET / HTTP/1.1");
    auto sent = client_side->value()->send(bytes_of(request));
    CHECK(!pump_until([&] { return sent->is_ready(); }, 200));
    CHECK(gate->held_promise.is_valid());

    gated->release();
    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);

    byte inbox[128] = {};
    auto received = server_side->value()->receive(cc::span<byte>(inbox, isize(sizeof(inbox))));
    CHECK(pump_until([&] { return received->is_ready(); }));
    REQUIRE(received->try_error() == nullptr);
    CHECK(cc::string_view(reinterpret_cast<char const*>(inbox), received->value()) == request);
}

CNET_IO_TEST("cnet - an untrusted certificate is refused")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("localhost").value();

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    // The client is told to trust nothing at all, which is what a self-signed certificate meets in the wild.
    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side = tls_connect(fixture.connected->value(), "localhost", {.trust = {.use_system_roots = false}});

    CHECK(pump_until([&] { return client_side->is_ready(); }));
    CHECK(client_side->try_error() != nullptr);
    CHECK(!client_side->try_error()->is_cancelled());

    // A refusal is a decision about a chain rather than a breakdown, and the server hears the handshake fail too.
    CHECK(pump_until([&] { return server_side->is_ready(); }));
}

CNET_IO_TEST("cnet - a certificate for the wrong host is refused")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("example.invalid").value();

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    // The chain builds and the name does not match, which is exactly the case a caller must not be able to miss by
    // passing an address where a hostname belongs.
    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side
        = tls_connect(fixture.connected->value(), "localhost",
                      {.trust = {.use_system_roots = false, .additional_roots_pem = {identity.certificate_chain_pem}}});

    CHECK(pump_until([&] { return client_side->is_ready(); }));
    CHECK(client_side->try_error() != nullptr);
    CHECK(pump_until([&] { return server_side->is_ready(); }));
}

CNET_IO_TEST("cnet - allow_any_certificate is the one way past verification")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("somebody.else").value();

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    // Settable from code and from nowhere else, which is why this test is the only place it appears.
    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side = tls_connect(fixture.connected->value(), "localhost",
                                   {.trust = {.use_system_roots = false, .allow_any_certificate = true}});

    CHECK(pump_until([&] { return server_side->is_ready() && client_side->is_ready(); }));
    CHECK(client_side->try_error() == nullptr);
    CHECK(server_side->try_error() == nullptr);
}

CNET_IO_TEST("cnet - the two ends agree on an application protocol")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("localhost").value();

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    // The client offers two and the server speaks one, which is how HTTP will learn which version it is talking.
    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity, .alpn = {"http/1.1"}});
    auto client_side
        = tls_connect(fixture.connected->value(), "localhost",
                      {.trust = {.use_system_roots = false, .additional_roots_pem = {identity.certificate_chain_pem}},
                       .alpn = {"h2", "http/1.1"}});

    CHECK(pump_until([&] { return server_side->is_ready() && client_side->is_ready(); }));
    CHECK(client_side->try_error() == nullptr);

    CHECK(tls_negotiated_alpn(*client_side->value()) == "http/1.1");
    CHECK(tls_negotiated_alpn(*server_side->value()) == "http/1.1");

    // A connection that never had a handshake has nothing to report, rather than a made-up answer.
    CHECK(tls_negotiated_alpn(*fixture.connected->value()).empty());
}

CNET_IO_TEST("cnet - a large payload crosses the record boundary intact")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");

    auto const identity = tls_make_self_signed("localhost").value();

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side
        = tls_connect(fixture.connected->value(), "localhost",
                      {.trust = {.use_system_roots = false, .additional_roots_pem = {identity.certificate_chain_pem}}});
    CHECK(pump_until([&] { return server_side->is_ready() && client_side->is_ready(); }));
    CHECK(client_side->try_error() == nullptr);

    // Bigger than one TLS record, so the write is split and the reader has to reassemble across records.
    auto payload = cc::vector<byte>();
    payload.resize_to_defaulted(100 * 1024);
    for (isize i = 0; i < payload.size(); ++i)
        payload[i] = byte(i * 31 + 7);

    auto sent = client_side->value()->send(payload);

    auto collected = cc::vector<byte>();
    auto inbox = cc::vector<byte>();
    inbox.resize_to_defaulted(8 * 1024);

    while (collected.size() < payload.size())
    {
        auto received = server_side->value()->receive(inbox);
        if (!pump_until([&] { return received->is_ready(); }))
            break;
        if (received->try_error() != nullptr)
            break;

        for (isize i = 0; i < received->value(); ++i)
            collected.push_back(inbox[i]);
    }

    CHECK(pump_until([&] { return sent->is_ready(); }));
    CHECK(sent->try_error() == nullptr);
    CHECK(collected.size() == payload.size());

    auto identical = true;
    for (isize i = 0; i < collected.size() && i < payload.size(); ++i)
        if (collected[i] != payload[i])
        {
            identical = false;
            break;
        }
    CHECK(identical);
}

CNET_IO_TEST("cnet - the platform trust store answers, or says it cannot")
{
    auto const roots = cnet::impl::system_root_certificates();

    if (roots.has_error())
    {
        // An adapter that is not written yet reports `unsupported`, which is a different fact from "no roots" and
        // must stay different: only the second should let a caller believe a connection was verified.
        CHECK(roots.error().code == error_code::unsupported);
        SKIP("no trust store adapter for this platform yet");
    }

    // A machine with a working store has hundreds of roots, in whichever form that platform keeps them.
    CHECK(!roots.value().empty());
    for (auto const& der : roots.value().der)
        CHECK(!der.empty());
    for (auto const& bundle : roots.value().pem)
        CHECK(!bundle.empty());
}

CNET_IO_TEST("cnet - a self-signed certificate is refused by the machine's own roots")
{
    if (!tls_is_supported())
        SKIP("this build has no TLS backend");
    if (cnet::impl::system_root_certificates().has_error())
        SKIP("no trust store adapter for this platform yet");

    auto const identity = tls_make_self_signed("localhost").value();

    auto fixture = tls_fixture();
    CHECK(fixture.ready());

    // The default trust is this machine's, and nothing this test made is in it, since nobody can add to that
    // store from a config file.
    auto server_side = tls_accept(fixture.accepted->value(), {.identity = identity});
    auto client_side = tls_connect(fixture.connected->value(), "localhost");

    CHECK(pump_until([&] { return client_side->is_ready(); }));
    CHECK(client_side->try_error() != nullptr);
    CHECK(pump_until([&] { return server_side->is_ready(); }));
}
