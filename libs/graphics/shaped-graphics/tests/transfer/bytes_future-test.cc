#include <clean-core/container/pinned_data.hh>
#include <nexus/test.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr

using namespace cc::primitive_defines;

// Backend-agnostic tests for the sg download-result vocabulary, with no GPU needed.
// Only the non-blocking polls live on the future; the blocking wait is ctx.wait_for(future), covered in the context-driven suites (tests/transfer, backends/dx12/tests).
// Backend readback and actor completion are exercised there.

TEST("sg bytes_future - default is invalid")
{
    sg::bytes_future f;
    CHECK(!f.is_valid());
    CHECK(!f.is_ready());
    CHECK(!f.try_get_bytes().has_value());
}

TEST("sg bytes_future - a settled completion yields its bytes")
{
    byte const src[] = {byte(10), byte(20), byte(30)};
    auto const data = cc::pinned_data<byte>::create_copy_of(src);

    sg::bytes_future f(data, sg::make_ready_completion());
    CHECK(f.is_valid());
    CHECK(f.is_ready());

    auto const got = f.try_get_bytes();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 3);
    CHECK(got.value()[1] == byte(20));
}

TEST("sg data_future - typed view over the bytes")
{
    int const src[] = {7, 9};
    auto const data = cc::pinned_data<int>::create_copy_of(src);

    sg::data_future<int> df(sg::bytes_future(data.as_bytes(), sg::make_ready_completion()));
    CHECK(df.is_valid());
    CHECK(df.is_ready());

    auto const got = df.try_get_data();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 2);
    CHECK(got.value()[0] == 7);
    CHECK(got.value()[1] == 9);
}

TEST("sg bytes_future - a cancelled completion yields no bytes")
{
    byte const src[] = {byte(1), byte(2)};
    auto const data = cc::pinned_data<byte>::create_copy_of(src);

    auto const completion = cc::make_async_manual<cc::unit>();
    sg::bytes_future f(data, completion);
    CHECK(f.is_valid());
    CHECK(!f.is_ready());
    CHECK(!f.try_get_bytes().has_value());

    // Settling on the error channel is what a dropped list or a dropped destination does.
    completion->push_error(cc::async_error::make_cancelled());
    CHECK(f.is_ready()); // settled — but with an error, so the bytes never arrive
    CHECK(!f.try_get_bytes().has_value());
}

TEST("sg bytes_future - completion composes into an async graph")
{
    byte const src[] = {byte(42)};
    auto const data = cc::pinned_data<byte>::create_copy_of(src);

    auto const completion = cc::make_async_manual<cc::unit>();
    sg::bytes_future const f(data, completion);

    auto const next = cc::make_async_lazy([](cc::unit) { return 5; }, f.completion());
    CHECK(!next->is_ready());

    completion->push_value(cc::unit{});
    CHECK(cc::async_blocking_get(next) == 5);
}
