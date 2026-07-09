#include <clean-core/common/hash.hh>
#include <clean-core/container/array.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/pair.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/unique_array.hh>
#include <clean-core/container/unique_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

TEST("hash types - string and string_view hash equally (heterogeneous)")
{
    cc::string const s = "hello world";
    cc::string_view const sv = "hello world";
    CHECK(cc::make_hash(s) == cc::make_hash(sv));

    auto const raw = cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>("hello world"), 11);
    CHECK(cc::make_hash(s) == cc::make_hash_of_bytes(raw));

    cc::string const other = "hello worle";
    CHECK(cc::make_hash(s) != cc::make_hash(other));
}

TEST("hash types - vector is structural and order-dependent")
{
    cc::vector<int> const a = {1, 2, 3};
    cc::vector<int> const b = {1, 2, 3};
    cc::vector<int> const reordered = {3, 2, 1};
    cc::vector<int> const empty;
    cc::vector<int> const one = {0};

    CHECK(cc::make_hash(a) == cc::make_hash(b));         // same content
    CHECK(cc::make_hash(a) != cc::make_hash(reordered)); // order matters
    CHECK(cc::make_hash(empty) != cc::make_hash(one));   // length/content distinct
}

TEST("hash types - span / fixed_array / array families agree on content")
{
    cc::vector<int> const v = {1, 2, 3};
    cc::array<int> const ar = {1, 2, 3};
    cc::unique_array<int> const ua = {1, 2, 3};
    cc::unique_vector<int> const uv = {1, 2, 3};
    cc::fixed_array<int, 3> const fa = {1, 2, 3};
    cc::span<int const> const sp = cc::span<int const>(v.data(), v.size());

    // all use the same structural make_hash_range fold, so equal content -> equal hash
    auto const h = cc::make_hash(v);
    CHECK(cc::make_hash(ar) == h);
    CHECK(cc::make_hash(ua) == h);
    CHECK(cc::make_hash(uv) == h);
    CHECK(cc::make_hash(fa) == h);
    CHECK(cc::make_hash(sp) == h);
}

TEST("hash types - pair")
{
    cc::pair<int, int> const p{1, 2};
    cc::pair<int, int> const swapped{2, 1};
    CHECK(cc::make_hash(p) == cc::make_hash(1, 2));
    CHECK(cc::make_hash(p) != cc::make_hash(swapped));
}

TEST("hash types - optional distinguishes engaged/empty/value")
{
    cc::optional<int> const empty = {};
    cc::optional<int> const a = 5;
    cc::optional<int> const b = 5;
    cc::optional<int> const c = 6;

    CHECK(cc::make_hash(a) == cc::make_hash(b));
    CHECK(cc::make_hash(a) != cc::make_hash(c));
    CHECK(cc::make_hash(a) != cc::make_hash(empty));
}

TEST("hash types - unique_ptr hashes by pointer identity, not value")
{
    auto p = cc::make_unique<int>(5);
    auto q = cc::make_unique<int>(5);
    CHECK(cc::make_hash(p) == reinterpret_cast<u64>(p.get()));
    CHECK(cc::make_hash(p) != cc::make_hash(q)); // equal values, different addresses

    cc::unique_ptr<int> const empty;
    CHECK(cc::make_hash(empty) == 0ull);
}
