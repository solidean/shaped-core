#include <clean-core/common/macros.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <versioned-document/value.hh>
#include <versioned-document/value_builder.hh>
#include <versioned-document/value_debug.hh>

using namespace cc::primitive_defines;

using vdoc::value;
using vdoc::value_build_error;
using vdoc::value_builder;
using vdoc::value_decode_error_kind;
using vdoc::value_kind;
using vdoc::value_view;

namespace
{
/// Assembles encodings BY HAND, without sorting and without validating.
/// That is the whole point: a decoder is only worth anything if it refuses the bytes nothing in the library would produce.
struct raw_bytes
{
    cc::vector<byte> data;

    raw_bytes& tag(value_kind k) { return put_u8(u8(k)); }

    raw_bytes& put_u8(u8 v)
    {
        data.push_back(byte(v));
        return *this;
    }

    raw_bytes& put_u32(u32 v)
    {
        for (isize i = 0; i < 4; ++i)
            data.push_back(byte((v >> (i * 8)) & 0xFF));
        return *this;
    }

    raw_bytes& put_chars(cc::string_view s)
    {
        for (isize i = 0; i < s.size(); ++i)
            data.push_back(byte(u8(s[i])));
        return *this;
    }

    raw_bytes& put(cc::span<byte const> s)
    {
        for (isize i = 0; i < s.size(); ++i)
            data.push_back(s[i]);
        return *this;
    }

    /// An object entry: the u32 key length, the key, then an already-encoded value.
    raw_bytes& entry(cc::string_view key, cc::span<byte const> encoded)
    {
        put_u32(u32(key.size()));
        put_chars(key);
        return put(encoded);
    }

    [[nodiscard]] cc::span<byte const> view() const { return cc::span<byte const>(data.data(), data.size()); }
};

/// tag | u32 payload length | u32 count | entries, with the length derived from `entries` and the count taken as given.
/// Handing a count that disagrees with the entries is exactly what several tests below do.
[[nodiscard]] cc::vector<byte> raw_container(value_kind kind, u32 count, cc::span<byte const> entries)
{
    auto r = raw_bytes();
    r.tag(kind).put_u32(u32(4 + entries.size())).put_u32(count).put(entries);
    return r.data;
}

/// One array wrapped around an already-encoded value, for building nesting the builder would refuse to build.
[[nodiscard]] cc::vector<byte> wrap_in_array(cc::span<byte const> inner)
{
    return raw_container(value_kind::array, 1, inner);
}

[[nodiscard]] cc::vector<byte> nested_arrays(isize depth)
{
    CC_ASSERT(depth >= 1, "depth counts the innermost value");

    auto current = cc::vector<byte>::create_copy_of(value::of_null().bytes());
    for (isize i = 1; i < depth; ++i)
        current = wrap_in_array(cc::span<byte const>(current.data(), current.size()));

    return current;
}

[[nodiscard]] cc::span<byte const> as_span(cc::vector<byte> const& v)
{
    return cc::span<byte const>(v.data(), v.size());
}
} // namespace

// ---- round-trips -------------------------------------------------------------------------------

TEST("vdoc - value round-trips every scalar kind")
{
    auto const n = value::of_null();
    CHECK(n.kind() == value_kind::null);
    CHECK(n.is_null());

    CHECK(value::of(true).as_bool() == true);
    CHECK(value::of(false).as_bool() == false);
    CHECK(value::of(true).kind() == value_kind::boolean);

    CHECK(value::of(42).kind() == value_kind::integer);
    CHECK(value::of(42).as_i64() == 42);
    CHECK(value::of(-7).as_i64() == -7);
    CHECK(value::of(u8(200)).as_i64() == 200);

    CHECK(value::of(1.5).kind() == value_kind::number);
    CHECK(value::of(1.5).as_f64() == 1.5);
    CHECK(value::of(1.5f).as_f64() == 1.5);

    CHECK(value::of("wall").kind() == value_kind::string);
    CHECK(value::of("wall").as_string() == "wall");
    CHECK(value::of(cc::string_view("wall")).as_string() == "wall");
}

TEST("vdoc - value round-trips the integer extremes")
{
    auto const lo = i64(0x8000000000000000ull);
    auto const hi = 0x7FFFFFFFFFFFFFFFll;

    CHECK(value::of(lo).as_i64() == lo);
    CHECK(value::of(hi).as_i64() == hi);
    CHECK(value::of(i64(0)).as_i64() == 0);
}

TEST("vdoc - value round-trips empty strings, bytes and containers")
{
    CHECK(value::of("").as_string().size() == 0);
    CHECK(value::of("").kind() == value_kind::string);

    auto const empty_bytes = value::of_bytes(cc::span<byte const>());
    CHECK(empty_bytes.kind() == value_kind::bytes);
    CHECK(empty_bytes.as_bytes().size() == 0);

    auto const empty_array = value_builder::array().build();
    CHECK(empty_array.kind() == value_kind::array);
    CHECK(empty_array.size() == 0);

    auto const empty_object = value_builder::object().build();
    CHECK(empty_object.kind() == value_kind::object);
    CHECK(empty_object.size() == 0);

    // everything above must survive the decoder, which is the only thing that makes it a value
    CHECK(vdoc::try_decode(empty_array.bytes()).has_value());
    CHECK(vdoc::try_decode(empty_object.bytes()).has_value());
    CHECK(vdoc::try_decode(empty_bytes.bytes()).has_value());
    CHECK(vdoc::try_decode(value::of("").bytes()).has_value());
}

TEST("vdoc - value round-trips a bytes payload verbatim")
{
    byte const payload[5] = {byte(0x00), byte(0xFF), byte(0x7F), byte(0x80), byte(0x01)};
    auto const v = value::of_bytes(cc::span<byte const>(payload, 5));

    REQUIRE(v.as_bytes().size() == 5);
    for (isize i = 0; i < 5; ++i)
        CHECK(v.as_bytes()[i] == payload[i]);
}

TEST("vdoc - containers round-trip through the decoder")
{
    auto const arr = value_builder::array().push(1.0).push(2.0).push(3.0).build();
    REQUIRE(vdoc::try_decode(arr.bytes()).has_value());
    REQUIRE(arr.size() == 3);
    CHECK(arr.element_at(0).as_f64() == 1.0);
    CHECK(arr.element_at(1).as_f64() == 2.0);
    CHECK(arr.element_at(2).as_f64() == 3.0);

    auto const obj = value_builder::object().set("name", "wall").set("height", 3).set("alive", true).build();
    REQUIRE(vdoc::try_decode(obj.bytes()).has_value());
    REQUIRE(obj.size() == 3);

    // stored in key order, not in call order
    CHECK(obj.key_at(0) == "alive");
    CHECK(obj.key_at(1) == "height");
    CHECK(obj.key_at(2) == "name");

    REQUIRE(obj.try_find("name").has_value());
    CHECK(obj.try_find("name").value().as_string() == "wall");
    CHECK(obj.try_find("height").value().as_i64() == 3);
    CHECK(obj.try_find("alive").value().as_bool());
    CHECK(!obj.try_find("missing").has_value());
}

TEST("vdoc - nested containers round-trip")
{
    auto const v = value_builder::object()
                       .set("p", value_builder::array().push(1).push(2).build())
                       .set("meta", value_builder::object().set("k", "v").build())
                       .build();

    REQUIRE(vdoc::try_decode(v.bytes()).has_value());
    REQUIRE(v.try_find("p").has_value());
    CHECK(v.try_find("p").value().size() == 2);
    CHECK(v.try_find("p").value().element_at(1).as_i64() == 2);
    CHECK(v.try_find("meta").value().try_find("k").value().as_string() == "v");
}

// ---- canonicality ------------------------------------------------------------------------------

TEST("vdoc - decoding rejects an unknown tag")
{
    auto const bytes = raw_bytes().put_u8(8).data;
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::unknown_tag);
    CHECK(r.error().offset == 0);
}

TEST("vdoc - decoding rejects a boolean that is neither 0 nor 1")
{
    auto const bytes = raw_bytes().tag(value_kind::boolean).put_u8(2).data;
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::invalid_boolean);
    CHECK(r.error().offset == 1);
}

TEST("vdoc - decoding rejects a truncated payload")
{
    auto const bytes = raw_bytes().tag(value_kind::integer).put_u8(0).put_u8(0).data;
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::truncated);
    CHECK(r.error().offset == 1);
}

TEST("vdoc - decoding rejects a length prefix that overruns its buffer")
{
    auto const bytes = raw_bytes().tag(value_kind::string).put_u32(5).put_chars("ab").data;
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::length_mismatch);
    CHECK(r.error().offset == 1);
}

TEST("vdoc - decoding rejects trailing bytes")
{
    auto const bytes = raw_bytes().tag(value_kind::null).tag(value_kind::null).data;
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::trailing_bytes);
    CHECK(r.error().offset == 1);
}

TEST("vdoc - decoding rejects unsorted object keys")
{
    auto entries = raw_bytes();
    entries.entry("b", value::of_null().bytes()).entry("a", value::of_null().bytes());

    auto const bytes = raw_container(value_kind::object, 2, entries.view());
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::unsorted_keys);
    CHECK(r.error().offset == 15); // the second entry, where the order first goes wrong
}

TEST("vdoc - decoding rejects duplicate object keys")
{
    auto entries = raw_bytes();
    entries.entry("a", value::of_null().bytes()).entry("a", value::of_null().bytes());

    auto const bytes = raw_container(value_kind::object, 2, entries.view());
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::duplicate_key);
    CHECK(r.error().offset == 15);
}

TEST("vdoc - decoding rejects a count larger than the entries")
{
    auto const entries = raw_bytes().tag(value_kind::null).data;
    auto const bytes = raw_container(value_kind::array, 2, as_span(entries));
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::count_mismatch);
}

TEST("vdoc - decoding rejects a count smaller than the entries")
{
    auto const entries = raw_bytes().tag(value_kind::null).tag(value_kind::null).data;
    auto const bytes = raw_container(value_kind::array, 1, as_span(entries));
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::count_mismatch);
}

TEST("vdoc - decoding rejects a container payload too short to hold its count")
{
    auto const bytes = raw_bytes().tag(value_kind::array).put_u32(2).put_u8(0).put_u8(0).data;
    auto const r = vdoc::try_decode(as_span(bytes));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::length_mismatch);
    CHECK(r.error().offset == 1);
}

TEST("vdoc - decoding rejects empty input")
{
    auto const r = vdoc::try_decode(cc::span<byte const>());

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::truncated);
    CHECK(r.error().offset == 0);
}

TEST("vdoc - object keys order by byte value, not by signed char")
{
    // 0x80 is negative as a char, so a signed comparison would sort it FIRST and the decoder would then reject the very bytes the builder produced.
    // Every non-ASCII key in the wild depends on this.
    auto const low = cc::string_view("\x7f", 1);
    auto const high = cc::string_view("\x80", 1);

    auto const v = value_builder::object().set(high, 1).set(low, 2).build();

    REQUIRE(vdoc::try_decode(v.bytes()).has_value());
    REQUIRE(v.size() == 2);
    CHECK(v.key_at(0) == low);
    CHECK(v.key_at(1) == high);
    CHECK(v.try_find(high).value().as_i64() == 1);
}

TEST("vdoc - a shorter key sorts before what extends it")
{
    auto const v = value_builder::object().set("ab", 2).set("a", 1).build();

    REQUIRE(vdoc::try_decode(v.bytes()).has_value());
    CHECK(v.key_at(0) == "a");
    CHECK(v.key_at(1) == "ab");
}

// ---- byte equality -----------------------------------------------------------------------------

TEST("vdoc - equality is byte equality")
{
    CHECK(value::of(42) == value::of(42));
    CHECK(value::of(42) != value::of(43));
    CHECK(value::of("a") != value::of(cc::string_view("a\0", 2)));

    // an integer 1 and a number 1.0 are different values; the tag is part of the bytes
    CHECK(value::of(1) != value::of(1.0));

    CHECK(hash(value::of(42)) == hash(value::of(42)));
}

TEST("vdoc - an object literal is order-independent because the builder sorts")
{
    auto const a = value_builder::object().set("x", 1).set("y", 2).set("z", 3).build();
    auto const b = value_builder::object().set("z", 3).set("x", 1).set("y", 2).build();

    CHECK(a == b);
    CHECK(hash(a) == hash(b));
}

TEST("vdoc - values built by different routes compare equal")
{
    auto const built = value_builder::array().push(1).push("two").build();

    auto const decoded = vdoc::try_decode(built.bytes());
    REQUIRE(decoded.has_value());

    auto const copied = value::from_validated_bytes(decoded.value().bytes());
    CHECK(copied == built);
    CHECK(copied.view() == built.view());
    CHECK(hash(copied) == hash(built.view()));
}

// ---- skipping ----------------------------------------------------------------------------------

TEST("vdoc - skip_value lands exactly on the next value")
{
    auto const a = value::of(42);
    auto const b = value::of("wall");
    auto const c = value_builder::array().push(1).push(2).build();

    auto stream = raw_bytes();
    stream.put(a.bytes()).put(b.bytes()).put(c.bytes());

    auto rest = stream.view();
    CHECK(vdoc::encoded_size(rest) == a.bytes().size());

    rest = vdoc::skip_value(rest);
    CHECK(vdoc::try_decode(cc::span<byte const>(rest.data(), b.bytes().size())).value() == b.view());

    rest = vdoc::skip_value(rest);
    CHECK(vdoc::try_decode(rest).value() == c.view());

    rest = vdoc::skip_value(rest);
    CHECK(rest.size() == 0);
}

TEST("vdoc - skip_value steps over a deeply nested value in one go")
{
    auto const nested = nested_arrays(20);
    auto const trailer = value::of(7);

    auto stream = raw_bytes();
    stream.put(as_span(nested)).put(trailer.bytes());

    auto const rest = vdoc::skip_value(stream.view());
    CHECK(rest.size() == trailer.bytes().size());
    CHECK(vdoc::try_decode(rest).value() == trailer.view());
}

// ---- the depth limit ---------------------------------------------------------------------------

TEST("vdoc - nesting at the depth limit decodes")
{
    auto const at_limit = nested_arrays(value_view::max_depth);
    CHECK(vdoc::try_decode(as_span(at_limit)).has_value());
}

TEST("vdoc - nesting one past the depth limit is an error")
{
    auto const past_limit = nested_arrays(value_view::max_depth + 1);
    auto const r = vdoc::try_decode(as_span(past_limit));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::depth_exceeded);
}

TEST("vdoc - far past the depth limit still returns rather than overflowing the stack")
{
    auto const way_past = nested_arrays(value_view::max_depth * 8);
    auto const r = vdoc::try_decode(as_span(way_past));

    REQUIRE(r.has_error());
    CHECK(r.error().kind == value_decode_error_kind::depth_exceeded);
}

// ---- storage -----------------------------------------------------------------------------------

TEST("vdoc - a value is small_vector sized and small values stay inline")
{
    static_assert(sizeof(value) == sizeof(cc::small_vector<byte, 1>));

#if CC_HAS_64BIT_POINTERS
    // Pinned deliberately: a cc::small_vector change that quietly pushed every value onto the heap would cost every
    // property in every document, and would show up in a profile long before it showed up in a test.
    static_assert(sizeof(value) == 48);
    static_assert(cc::small_vector<byte, 1>::inline_capacity() == 36);

    // A position-like struct is the shape this inline budget exists for, and it encodes to exactly 36 bytes — so it
    // fits the 64-bit buffer precisely, and nothing smaller.
    // Guarded with the size assertions above rather than left loose: on a 32-bit target a small_vector is narrower,
    // so this heap-allocates, which is slower and not wrong.
    CHECK(value_builder::array().push(1.0).push(2.0).push(3.0).build().is_inline());
#endif

    // Well inside any target's buffer, 32-bit ones included.
    CHECK(value::of_null().is_inline());
    CHECK(value::of(true).is_inline());
    CHECK(value::of(1.5).is_inline());
    CHECK(value::of(0x7FFFFFFFFFFFFFFFll).is_inline());
    CHECK(value::of("a short name").is_inline());

    // and something past it is merely heap-backed, not wrong
    auto const big = value::of("a string long enough that it cannot possibly fit the inline buffer of a value");
    CHECK(!big.is_inline());
    CHECK(vdoc::try_decode(big.bytes()).has_value());
}

// ---- floats ------------------------------------------------------------------------------------

TEST("vdoc - NaN and negative zero survive verbatim")
{
    auto const negative_zero = value::of(-0.0);
    auto const positive_zero = value::of(0.0);

    // documented behaviour: the codec does not normalize a caller's float, so these are DIFFERENT values
    CHECK(negative_zero != positive_zero);
    CHECK(negative_zero.as_f64() == positive_zero.as_f64()); // ... even though they compare equal as doubles

    auto const nan_a = cc::bit_cast<f64>(0x7FF8000000000001ull);
    auto const nan_b = cc::bit_cast<f64>(0x7FF8000000000002ull);

    CHECK(value::of(nan_a) != value::of(nan_b));
    CHECK(value::of(nan_a) == value::of(nan_a));
    CHECK(cc::bit_cast<u64>(value::of(nan_a).as_f64()) == 0x7FF8000000000001ull);

    CHECK(vdoc::try_decode(value::of(nan_a).bytes()).has_value());
    CHECK(vdoc::try_decode(negative_zero.bytes()).has_value());
}

// ---- the builder -------------------------------------------------------------------------------

TEST("vdoc - try_build rejects a duplicate key and build asserts on it")
{
    auto const b = value_builder::object().set("a", 1).set("a", 2);

    auto const r = b.try_build();
    REQUIRE(r.has_error());
    CHECK(r.error() == value_build_error::duplicate_key);

    CHECK_ASSERTS(b.build());
}

TEST("vdoc - building is non-destructive and repeatable")
{
    auto b = value_builder::array().push(1);
    auto const first = b.build();

    b.push(2);
    auto const second = b.build();

    CHECK(first.size() == 1);
    CHECK(second.size() == 2);
    CHECK(first.element_at(0) == second.element_at(0));
    CHECK(second == b.build());
}

TEST("vdoc - a builder refuses the wrong operation for its kind")
{
    CHECK_ASSERTS(value_builder::array().set("a", 1));
    CHECK_ASSERTS(value_builder::object().push(1));
}

TEST("vdoc - composing past the depth limit is caught at build time")
{
    auto v = value::of_null();
    for (isize i = 1; i < value_view::max_depth; ++i)
        v = value_builder::array().push(v).build();

    CHECK(vdoc::try_decode(v.bytes()).has_value());
    CHECK_ASSERTS(value_builder::array().push(v).build());
}

// ---- debug text --------------------------------------------------------------------------------

TEST("vdoc - to_debug_string renders each kind")
{
    CHECK(vdoc::to_debug_string(value::of_null()) == "null");
    CHECK(vdoc::to_debug_string(value::of(true)) == "true");
    CHECK(vdoc::to_debug_string(value::of(-7)) == "-7");
    CHECK(vdoc::to_debug_string(value::of("wall")) == "\"wall\"");

    byte const payload[2] = {byte(0xDE), byte(0xAD)};
    CHECK(vdoc::to_debug_string(value::of_bytes(cc::span<byte const>(payload, 2))) == "bytes(dead)");

    CHECK(vdoc::to_debug_string(value_builder::array().push(1).push("x").build()) == "[1, \"x\"]");
    CHECK(vdoc::to_debug_string(value_builder::object().set("b", 2).set("a", 1).build()) == "{\"a\": 1, \"b\": 2}");
}

TEST("vdoc - to_debug_string shows what the codec promises to preserve")
{
    CHECK(vdoc::to_debug_string(value::of(-0.0)) == "-0.0");
    CHECK(vdoc::to_debug_string(value::of(cc::bit_cast<f64>(0x7FF8000000000000ull))) == "NaN");
    CHECK(vdoc::to_debug_string(value::of(cc::bit_cast<f64>(0x7FF8000000000001ull))) == "NaN(0x7ff8000000000001)");
    CHECK(vdoc::to_debug_string(value::of(cc::bit_cast<f64>(0x7FF0000000000000ull))) == "Infinity");
    CHECK(vdoc::to_debug_string(value::of(cc::bit_cast<f64>(0xFFF0000000000000ull))) == "-Infinity");
}

TEST("vdoc - to_debug_string keeps its output printable")
{
    CHECK(vdoc::to_debug_string(value::of(cc::string_view("a\"b\\c", 5))) == "\"a\\\"b\\\\c\"");
    CHECK(vdoc::to_debug_string(value::of(cc::string_view("\x01", 1))) == "\"\\x01\"");
    CHECK(vdoc::to_debug_string(value::of(cc::string_view("\xff", 1))) == "\"\\xff\"");
}

// ---- views -------------------------------------------------------------------------------------

TEST("vdoc - a default value_view is the null value")
{
    auto const v = value_view();
    CHECK(v.is_null());
    CHECK(v.kind() == value_kind::null);
    CHECK(v.size() == 0);
    CHECK(v.bytes().size() == 1);
    CHECK(v == value::of_null().view());
    CHECK(!v.try_find("anything").has_value());
}

TEST("vdoc - a typed accessor on the wrong kind asserts")
{
    auto const v = value::of(42);
    CHECK_ASSERTS(v.as_bool());
    CHECK_ASSERTS(v.as_f64());
    CHECK_ASSERTS(v.as_string());
    CHECK_ASSERTS(v.as_bytes());
    CHECK_ASSERTS(v.element_at(0));
    CHECK_ASSERTS(v.key_at(0));
}

TEST("vdoc - a view borrows and does not copy")
{
    auto const owned = value_builder::array().push(1).push(2).build();
    auto const decoded = vdoc::try_decode(owned.bytes());
    REQUIRE(decoded.has_value());

    CHECK(decoded.value().bytes().data() == owned.bytes().data());
    CHECK(decoded.value().element_at(1).bytes().data() > owned.bytes().data());
}
