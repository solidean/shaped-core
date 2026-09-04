#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/function/function_ref.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <clean-net/address/resolver.hh>
#include <clean-net/io/io_system.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

// Name resolution, tested without a resolver.
// Every test but the last supplies its own lookup, for the same reason the clock is a seam: a test that depends on a
// real resolver depends on the machine it runs on, on a network, and on a name somebody else controls.

namespace
{
bool pump_until(cc::function_ref<bool()> done, f64 budget_secs = 5.0)
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

[[nodiscard]] ip_address v4(cc::string_view text)
{
    return ip_address::parse(text).value();
}
} // namespace

TEST("cnet - a literal address resolves to itself, with no lookup and no cache")
{
    auto io = io_system::create({.unthreaded = true});

    auto calls = 0;
    auto r = resolver::create(*io, {.lookup = [&calls](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                    {
                                        ++calls;
                                        return cc::vector<ip_address>();
                                    }});

    auto resolved = r->resolve("127.0.0.1");
    CHECK(resolved->is_ready());
    CHECK(resolved->value().size() == 1);
    CHECK(resolved->value()[0] == v4("127.0.0.1"));

    // A caller should not have to know which kind of string it holds, and an address costs nothing to recognise.
    CHECK(calls == 0);
    CHECK(r->cached_host_count() == 0);
}

TEST("cnet - a resolved host is cached, and the second caller never reaches the worker")
{
    auto io = io_system::create({.unthreaded = true});

    auto calls = 0;
    auto r = resolver::create(*io, {.lookup = [&calls](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                    {
                                        ++calls;
                                        return cc::vector<ip_address>{v4("10.0.0.1"), v4("10.0.0.2")};
                                    }});

    auto first = r->resolve("example.invalid");
    CHECK(pump_until([&] { return first->is_ready(); }));
    CHECK(first->try_error() == nullptr);
    CHECK(first->value().size() == 2);
    CHECK(calls == 1);
    CHECK(r->cached_host_count() == 1);

    // The cache is what confines a blocking lookup -- and, on a threads-off build, the stall -- to first contact.
    auto second = r->resolve("example.invalid");
    CHECK(second->is_ready());
    CHECK(second->value().size() == 2);
    CHECK(calls == 1);

    r->clear_cache();
    CHECK(r->cached_host_count() == 0);

    auto third = r->resolve("example.invalid");
    CHECK(pump_until([&] { return third->is_ready(); }));
    CHECK(calls == 2);
}

TEST("cnet - a cached answer expires on the injected clock")
{
    auto clk = manual_clock(0);
    auto io = io_system::create({.unthreaded = true, .time_source = &clk});

    auto calls = 0;
    auto r = resolver::create(*io, {.cache_ttl_ms = 30'000,
                                    .lookup = [&calls](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                    {
                                        ++calls;
                                        return cc::vector<ip_address>{v4("10.0.0.1")};
                                    }});

    auto first = r->resolve("example.invalid");
    CHECK(pump_until([&] { return first->is_ready(); }));
    CHECK(calls == 1);

    clk.advance_ms(29'000);
    auto still_good = r->resolve("example.invalid");
    CHECK(still_good->is_ready());
    CHECK(calls == 1);

    clk.advance_ms(2'000);
    auto expired = r->resolve("example.invalid");
    CHECK(pump_until([&] { return expired->is_ready(); }));
    CHECK(calls == 2);
}

TEST("cnet - a family preference filters, and says so when nothing is left")
{
    auto io = io_system::create({.unthreaded = true});

    auto r = resolver::create(
        *io, {.lookup = [](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
              { return cc::vector<ip_address>{v4("10.0.0.1"), ip_address::parse("2001:db8::1").value()}; }});

    auto both = r->resolve("example.invalid");
    CHECK(pump_until([&] { return both->is_ready(); }));
    CHECK(both->value().size() == 2);

    // The whole answer was cached, whatever the first caller asked for, so these come out of the cache.
    auto only_v6 = r->resolve("example.invalid", {.family = address_family_preference::v6_only});
    CHECK(only_v6->is_ready());
    CHECK(only_v6->value().size() == 1);
    CHECK(only_v6->value()[0].family() == ip_family::v6);

    auto only_v4 = r->resolve("example.invalid", {.family = address_family_preference::v4_only});
    CHECK(only_v4->is_ready());
    CHECK(only_v4->value()[0].family() == ip_family::v4);

    // A literal answers the same way, and refusing is better than handing back the wrong family.
    auto impossible = r->resolve("10.0.0.5", {.family = address_family_preference::v6_only});
    CHECK(impossible->is_ready());
    CHECK(impossible->try_error() != nullptr);
}

TEST("cnet - a lookup that fails fails the resolve")
{
    auto io = io_system::create({.unthreaded = true});

    auto r = resolver::create(*io, {.lookup = [](cc::string_view host) -> cc::result<cc::vector<ip_address>, error>
                                    {
                                        return cc::error(error{.code = error_code::name_not_resolved,
                                                               .native_code = 11001,
                                                               .message = cc::format("no such host {}", host)});
                                    }});

    auto resolved = r->resolve("nothing.invalid");
    CHECK(pump_until([&] { return resolved->is_ready(); }));
    CHECK(resolved->try_error() != nullptr);

    // A failure is not cached: the next caller asks again, because a name that failed once may resolve in a second.
    CHECK(r->cached_host_count() == 0);
}

#if CC_HAS_THREADS
TEST("cnet - a resolve can be cancelled while its worker is still blocked")
{
    auto io = io_system::create({.unthreaded = true});

    // The worker parks here, exactly as a real getaddrinfo parks on a bad network -- a call nobody can abort.
    auto release = cc::atomic<bool>(false);

    auto r = resolver::create(*io, {.lookup = [&release](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                    {
                                        while (!release.load())
                                            cc::this_thread_yield();
                                        return cc::vector<ip_address>{v4("10.0.0.1")};
                                    }});

    auto const token = cancel_token::create();
    auto resolved = r->resolve("slow.invalid", {.timeout = deadline::never()}, token);
    CHECK(!resolved->is_ready());

    token.cancel();
    CHECK(pump_until([&] { return resolved->is_ready(); }));

    auto const* const failure = resolved->try_error();
    CHECK(failure != nullptr);
    CHECK(failure->is_cancelled());

    // The worker comes back to a slot whose operation has already died, which is the whole reason there is a slot.
    release.store(true);
    CHECK(pump_until([&] { return true; }));
}

TEST("cnet - a resolve times out on the injected clock while its worker runs on")
{
    auto clk = manual_clock(0);
    auto io = io_system::create({.unthreaded = true, .time_source = &clk});

    auto release = cc::atomic<bool>(false);

    auto r = resolver::create(*io, {.lookup = [&release](cc::string_view) -> cc::result<cc::vector<ip_address>, error>
                                    {
                                        while (!release.load())
                                            cc::this_thread_yield();
                                        return cc::vector<ip_address>{v4("10.0.0.1")};
                                    }});

    auto resolved = r->resolve("slow.invalid", {.timeout = deadline::after_secs(5)});
    CHECK(!resolved->is_ready());

    // The timeout bounds the WAIT rather than the work: the worker is still in there.
    clk.advance_ms(5'001);
    CHECK(pump_until([&] { return resolved->is_ready(); }));
    CHECK(resolved->try_error() != nullptr);
    CHECK(!resolved->try_error()->is_cancelled());

    release.store(true);
}
#endif

TEST("cnet - localhost resolves through the platform")
{
    if (!resolver::is_supported())
        SKIP("this platform cannot resolve");

    auto io = io_system::create({.unthreaded = true});
    auto r = resolver::create(*io);

    // The one test that touches the OS resolver, and the only name that needs no network: every machine has it in
    // its hosts file.
    auto resolved = r->resolve("localhost");
    CHECK(pump_until([&] { return resolved->is_ready(); }));
    CHECK(resolved->try_error() == nullptr);
    CHECK(!resolved->value().empty());

    for (auto const& a : resolved->value())
        CHECK(a.is_loopback());
}
