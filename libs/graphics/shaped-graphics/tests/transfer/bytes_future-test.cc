#include <clean-core/container/pinned_data.hh>
#include <nexus/test.hh>
#include <shaped-graphics/bytes_future.hh>

#include <memory>

// Backend-agnostic tests for the sg download-result vocabulary (no GPU needed). Only the non-blocking
// polls live on the future; the blocking wait is ctx.wait_for(future), covered in the context-driven
// suites (tests/transfer, backends/dx12/tests). Backend readback / actor completion is exercised there.

TEST("sg bytes_future - default is invalid")
{
    sg::bytes_future f;
    CHECK(!f.is_valid());
    CHECK(!f.is_ready());
    CHECK(!f.try_get_bytes().has_value());
}

TEST("sg bytes_future - ready waiter yields its bytes")
{
    cc::byte const src[] = {cc::byte(10), cc::byte(20), cc::byte(30)};
    auto const data = cc::pinned_data<cc::byte>::create_copy_of(src);

    sg::bytes_future f(data, std::make_shared<sg::ready_bytes_waiter>());
    CHECK(f.is_valid());
    CHECK(f.is_ready());

    auto const got = f.try_get_bytes();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 3);
    CHECK(got.value()[1] == cc::byte(20));
}

TEST("sg data_future - typed view over the bytes")
{
    int const src[] = {7, 9};
    auto const data = cc::pinned_data<int>::create_copy_of(src);

    sg::data_future<int> df(sg::bytes_future(data.as_bytes(), std::make_shared<sg::ready_bytes_waiter>()));
    CHECK(df.is_valid());
    CHECK(df.is_ready());

    auto const got = df.try_get_data();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 2);
    CHECK(got.value()[0] == 7);
    CHECK(got.value()[1] == 9);
}
