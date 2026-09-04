#include <clean-core/error/result.hh>
#include <clean-net/common/error.hh>
#include <clean-net/common/level.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

namespace
{
cc::result<i32, error> fails_with(error_code code)
{
    return cc::error(error{.code = code, .native_code = 0, .message = cc::string("nope")});
}

/// The erasure a caller who does not care about the code gets for free.
cc::result<i32> erased()
{
    auto r = fails_with(error_code::connection_refused);
    CC_RETURN_IF_ERROR(r);
    return r.value();
}
} // namespace

TEST("cnet - an error carries a code, a native number and a message")
{
    auto const r = fails_with(error_code::timed_out);
    CHECK(r.has_error());
    CHECK(r.error().code == error_code::timed_out);
    CHECK(r.error().message == "nope");

    // Copyable, unlike cc::any_error, so a caller can latch the first failure and still read it.
    auto const latched = r.error();
    CHECK(latched.code == error_code::timed_out);
}

TEST("cnet - a typed error erases into cc::any_error and keeps its message")
{
    auto r = erased();
    CHECK(r.has_error());
    CHECK(r.error().to_string().contains("nope"));
}

TEST("cnet - the three kinds of no are distinguishable")
{
    // A platform that will never do this, answered once at startup.
    auto const never = unsupported_here("listening");
    CHECK(never.code == error_code::unsupported);
    CHECK(never.native_code == 0);

    // A build that did not compile it in, which changes when someone fetches a dependency.
    auto const absent = backend_missing("the curl backend", "no system libcurl was found");
    CHECK(absent.code == error_code::backend_missing);
    CHECK(absent.native_code == 0);

    CHECK(never.code != absent.code);
}

TEST("cnet - codes and levels have stable spellings for a log line")
{
    CHECK(to_string(error_code::timed_out) == "timed_out");
    CHECK(to_string(error_code::certificate_rejected) == "certificate_rejected");
    CHECK(to_string(http_level::fetch) == "fetch");
    CHECK(to_string(http_level::connection) == "connection");
}

TEST("cnet - the http level ladder is ordered, which is the whole point")
{
    CHECK(http_level::fetch < http_level::client);
    CHECK(http_level::client < http_level::connection);

    // Code written against `client` runs on every backend at that level or above.
    auto const backend = http_level::connection;
    CHECK(backend >= http_level::client);
}
