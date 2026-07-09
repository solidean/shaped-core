#include <clean-core/common/hash128.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <nexus/test.hh>

TEST("byte_stream_builder - basic append + determinism")
{
    cc::byte_stream_builder a;
    a.add_pod(cc::i32(42));
    a.add_string("shaped");
    a.add_bool(true);

    cc::byte_stream_builder b;
    b.add_pod(cc::i32(42));
    b.add_string("shaped");
    b.add_bool(true);

    CHECK(a.written_bytes().size() == b.written_bytes().size());
    CHECK(cc::hash128::create(a.written_bytes(), 0) == cc::hash128::create(b.written_bytes(), 0));

    // a different value diverges
    cc::byte_stream_builder c;
    c.add_pod(cc::i32(43));
    c.add_string("shaped");
    c.add_bool(true);
    CHECK(cc::hash128::create(a.written_bytes(), 0) != cc::hash128::create(c.written_bytes(), 0));
}

TEST("byte_stream_builder - length prefixes disambiguate splits")
{
    // "ab" + "c" must not collide with "a" + "bc"
    cc::byte_stream_builder x;
    x.add_string("ab");
    x.add_string("c");

    cc::byte_stream_builder y;
    y.add_string("a");
    y.add_string("bc");

    CHECK(cc::hash128::create(x.written_bytes(), 0) != cc::hash128::create(y.written_bytes(), 0));
}

TEST("byte_stream_builder - pod span sized vs unsized")
{
    int const data[] = {1, 2, 3};

    cc::byte_stream_builder raw;
    raw.add_pod_span(cc::span<int const>(data, 3));

    cc::byte_stream_builder sized;
    sized.add_pod_span_sized(cc::span<int const>(data, 3));

    // sized carries a u64 count prefix, so it is strictly longer
    CHECK(sized.written_bytes().size() == raw.written_bytes().size() + cc::isize(sizeof(cc::u64)));
}

TEST("byte_stream_builder - clear reuses the buffer")
{
    cc::byte_stream_builder a;
    a.add_string("first");
    auto const first = cc::hash128::create(a.written_bytes(), 0);

    a.clear();
    CHECK(a.written_bytes().empty());

    a.add_string("first");
    CHECK(cc::hash128::create(a.written_bytes(), 0) == first);
}

TEST("byte_stream_builder - optional presence")
{
    cc::byte_stream_builder some;
    some.add_optional(cc::optional<cc::i32>(7));

    cc::byte_stream_builder none;
    none.add_optional(cc::optional<cc::i32>(cc::nullopt));

    CHECK(cc::hash128::create(some.written_bytes(), 0) != cc::hash128::create(none.written_bytes(), 0));
}

TEST("byte_stream_builder - thread_local_scratch is cleared on fetch")
{
    auto& s1 = cc::byte_stream_builder::thread_local_scratch();
    s1.add_string("junk");
    CHECK(!s1.written_bytes().empty());

    auto& s2 = cc::byte_stream_builder::thread_local_scratch();
    CHECK(&s1 == &s2);                 // same per-thread instance
    CHECK(s2.written_bytes().empty()); // cleared on fetch
}
