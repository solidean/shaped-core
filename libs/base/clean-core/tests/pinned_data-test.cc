#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

#include <memory>

TEST("pinned_data - basic access")
{
    auto const pd = cc::pinned_data<int>::create_filled(4, 7);
    CHECK(pd.size() == 4);
    CHECK(pd.size_bytes() == 4 * cc::isize(sizeof(int)));
    CHECK(!pd.empty());
    CHECK(pd.data() != nullptr);
    CHECK(pd[0] == 7);
    CHECK(pd.front() == 7);
    CHECK(pd.back() == 7);

    int sum = 0;
    for (auto x : pd)
        sum += x;
    CHECK(sum == 28);

    CHECK(pd.span().size() == 4);
    CHECK(cc::pinned_data<int>{}.empty());
}

TEST("pinned_data - passes as span")
{
    auto const total = [](cc::span<int const> s)
    {
        int t = 0;
        for (auto x : s)
            t += x;
        return t;
    };

    int src[] = {10, 20, 30};
    auto const pd = cc::pinned_data<int>::create_copy_of(cc::span<int const>{src, 3});
    CHECK(total(pd) == 60);
    CHECK(pd.span().size() == 3);
}

TEST("pinned_data - create factories")
{
    SECTION("create_defaulted")
    {
        auto const pd = cc::pinned_data<int>::create_defaulted(3);
        CHECK(pd.size() == 3);
        CHECK(pd[0] == 0);
    }

    SECTION("create_uninitialized")
    {
        auto pd = cc::pinned_data<int>::create_uninitialized(3);
        CHECK(pd.size() == 3);
        pd[1] = 5;
        CHECK(pd[1] == 5);
    }

    SECTION("create_copy_of is independent")
    {
        int src[] = {1, 2, 3};
        auto const pd = cc::pinned_data<int>::create_copy_of(cc::span<int const>{src, 3});
        CHECK(pd.data() != src);
        src[0] = 99;
        CHECK(pd[0] == 1);
    }

    SECTION("create_from_pin round-trip")
    {
        auto owner = std::make_shared<cc::vector<int>>();
        owner->push_back(1);
        owner->push_back(2);
        auto const pd = cc::pinned_data<int>::create_from_pin(cc::span<int>{owner->data(), owner->size()},
                                                              std::shared_ptr<void>(owner));
        CHECK(pd.size() == 2);
        CHECK(pd[1] == 2);
        CHECK(owner.use_count() == 2); // owner + pinned_data
    }
}

TEST("pinned_data - subdata shares ownership")
{
    cc::pinned_data<int> sub;
    {
        auto const pd = cc::pinned_data<int>::create_filled(5, 9);
        sub = pd.subdata({.offset = 1, .size = 2});
        // pd goes out of scope here; sub must keep the buffer alive
    }
    CHECK(sub.size() == 2);
    CHECK(sub[0] == 9);
    CHECK(sub[1] == 9);

    SECTION("subdata_clamped")
    {
        auto const pd = cc::pinned_data<int>::create_filled(5, 3);
        CHECK(pd.subdata_clamped(99).empty());
        CHECK(pd.subdata_clamped(-3).size() == 5);
    }
}

TEST("pinned_data - const conversion")
{
    auto const pd = cc::pinned_data<int>::create_filled(3, 5);
    cc::pinned_data<int const> cpd = pd;
    CHECK(cpd.size() == 3);
    CHECK(cpd[0] == 5);
}

TEST("pinned_data - as_pinned_data wraps without copying")
{
    auto sp = std::make_shared<cc::vector<int>>();
    sp->push_back(1);
    sp->push_back(2);
    sp->push_back(3);
    auto const* ptr = sp->data();

    auto const pd = cc::as_pinned_data(sp);
    CHECK(pd.data() == ptr); // no copy
    CHECK(pd.size() == 3);
    CHECK(sp.use_count() == 2);

    SECTION("null yields empty")
    {
        std::shared_ptr<cc::vector<int>> np;
        auto const empty = cc::as_pinned_data(np);
        CHECK(empty.empty());
    }
}

TEST("pinned_data - make_pinned_data strategies")
{
    SECTION("shared_ptr of container is wrapped")
    {
        auto sp = std::make_shared<cc::vector<int>>();
        sp->push_back(1);
        sp->push_back(2);
        auto const* ptr = sp->data();
        auto const pd = cc::make_pinned_data(sp);
        CHECK(pd.data() == ptr);
        CHECK(sp.use_count() == 2);
    }

    SECTION("owning rvalue is moved")
    {
        cc::vector<int> v;
        v.push_back(4);
        v.push_back(5);
        v.push_back(6);
        auto const* ptr = v.data();
        auto const pd = cc::make_pinned_data(cc::move(v));
        CHECK(pd.data() == ptr); // buffer moved, not copied
        CHECK(pd.size() == 3);
        CHECK(pd[2] == 6);
    }

    SECTION("lvalue container is copied")
    {
        cc::vector<int> v;
        v.push_back(7);
        v.push_back(8);
        auto pd = cc::make_pinned_data(v);
        CHECK(pd.data() != v.data());
        v[0] = 99;
        CHECK(pd[0] == 7); // independent copy
    }

    SECTION("borrow range (span) is copied")
    {
        int data[] = {1, 2, 3};
        auto const pd = cc::make_pinned_data(cc::span<int>{data, 3});
        CHECK(pd.data() != data);
        data[0] = 99;
        CHECK(pd[0] == 1);
    }
}

TEST("pinned_data - reinterpret")
{
    SECTION("as_bytes shares the owner and outlives the source")
    {
        cc::pinned_data<cc::byte const> bytes;
        {
            auto const pd = cc::pinned_data<int>::create_filled(2, 0);
            bytes = pd.as_bytes();
            // pd dropped here; bytes must keep the buffer alive via the shared owner
        }
        CHECK(bytes.size() == 2 * cc::isize(sizeof(int)));
    }

    SECTION("reinterpret_as to smaller type")
    {
        auto const pd = cc::pinned_data<int>::create_filled(2, 0);
        auto const shorts = pd.reinterpret_as<short>();
        CHECK(shorts.size() == 4);
    }

    SECTION("as_mutable_bytes writes through")
    {
        auto pd = cc::pinned_data<int>::create_filled(1, 0);
        auto const bytes = pd.as_mutable_bytes();
        for (auto& b : bytes)
            b = cc::byte{0xFF};
        CHECK(pd[0] == -1);
    }

    SECTION("try_reinterpret_as")
    {
        auto const pd = cc::pinned_data<cc::byte>::create_defaulted(8);
        CHECK(pd.try_reinterpret_as<int>().has_value());
        CHECK(!pd.subdata({.offset = 0, .size = 5}).try_reinterpret_as<int>().has_value());
    }
}
