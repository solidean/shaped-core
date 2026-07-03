#include <nexus/test.hh>
#include <shaped-graphics/bytes_future.hh>

#include <memory>

// Backend-agnostic tests for the sg download-result vocabulary (no GPU needed). Backend behaviour
// (real readback, actor completion) is exercised in the dx12 backend test.

TEST("sg bytes_future - default is invalid")
{
    sg::bytes_future f;
    CHECK(!f.is_valid());
    CHECK(!f.is_ready());
    CHECK(!f.try_get_bytes().has_value());
    CHECK(!f.wait_get_bytes().has_value());
}

TEST("sg bytes_future - ready waiter yields its bytes")
{
    auto pin = std::shared_ptr<cc::byte[]>(new cc::byte[3]{cc::byte(10), cc::byte(20), cc::byte(30)});
    cc::span<cc::byte const> const data(pin.get(), 3);

    sg::bytes_future f(data, std::shared_ptr<void>(pin), std::make_shared<sg::ready_bytes_waiter>());
    CHECK(f.is_valid());
    CHECK(f.is_ready());

    auto const got = f.try_get_bytes();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 3);
    CHECK(got.value()[1] == cc::byte(20));

    auto const waited = f.wait_get_bytes();
    REQUIRE(waited.has_value());
    CHECK(waited.value().size() == 3);
}

TEST("sg data_future - typed view over the bytes")
{
    auto pin = std::shared_ptr<int[]>(new int[2]{7, 9});
    cc::span<cc::byte const> const bytes(reinterpret_cast<cc::byte const*>(pin.get()), 2 * cc::isize(sizeof(int)));

    sg::data_future<int> df(
        sg::bytes_future(bytes, std::shared_ptr<void>(pin), std::make_shared<sg::ready_bytes_waiter>()));
    CHECK(df.is_valid());
    CHECK(df.is_ready());

    auto const got = df.try_get_data();
    REQUIRE(got.has_value());
    CHECK(got.value().size() == 2);
    CHECK(got.value()[0] == 7);
    CHECK(got.value()[1] == 9);
}
