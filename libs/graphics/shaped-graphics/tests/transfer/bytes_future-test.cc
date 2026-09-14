#include <clean-core/container/pinned_data.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr

using namespace cc::primitive_defines;

// Backend-agnostic tests for the sg download-result vocabulary, with no GPU needed.
// The polls and the awaitable accessors live on the future and are pinned here against hand-pushed completions.
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

ASYNC_TEST("sg bytes_future - completion composes into an async graph")
{
    byte const src[] = {byte(42)};
    auto const data = cc::pinned_data<byte>::create_copy_of(src);

    auto const completion = cc::make_async_manual<cc::unit>();
    sg::bytes_future const f(data, completion);

    auto const next = cc::make_async_lazy([](cc::unit) { return 5; }, f.completion());
    CHECK(!next->is_ready());

    completion->push_value(cc::unit{});
    CHECK(co_await next == 5);
}

ASYNC_TEST("sg bytes_future - bytes() and data() resolve to what try_get_* would return")
{
    int const src[] = {3, 4};
    auto const data = cc::pinned_data<int>::create_copy_of(src);

    auto const completion = cc::make_async_manual<cc::unit>();
    sg::data_future<int> const df(sg::bytes_future(data.as_bytes(), completion));
    auto const typed = df.data();
    auto const raw = sg::bytes_future(data.as_bytes(), completion).bytes();
    REQUIRE(typed != nullptr);
    CHECK(!typed->is_ready()); // nothing resolves before the transfer does

    completion->push_value(cc::unit{});
    auto const got = co_await typed;
    REQUIRE(got.size() == 2);
    CHECK(got[1] == 4);
    CHECK((co_await raw).size() == 2 * sizeof(int));

    CHECK(sg::bytes_future{}.bytes() == nullptr);
    CHECK(sg::data_future<int>{}.data() == nullptr);
}

ASYNC_TEST("sg bytes_future - a cancelled transfer fails bytes(), and a ragged byte count fails data()")
{
    byte const src[] = {byte(1), byte(2), byte(3)};
    auto const data = cc::pinned_data<byte>::create_copy_of(src);

    auto const completion = cc::make_async_manual<cc::unit>();
    auto const cancelled = sg::bytes_future(data, completion).bytes();
    completion->push_error(cc::async_error::make_cancelled());
    auto const outcome = co_await cc::async_as_result(cancelled);
    REQUIRE(outcome.has_error());
    CHECK(outcome.error().is_cancelled());

    // Three bytes are no whole number of ints.
    auto const ragged = sg::data_future<int>(sg::bytes_future(data, sg::make_ready_completion())).data();
    CHECK((co_await cc::async_as_result(ragged)).has_error());
}
