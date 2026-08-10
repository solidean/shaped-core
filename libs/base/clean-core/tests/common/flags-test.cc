#include <clean-core/common/flags.hh>
#include <nexus/test.hh>

#include <type_traits>

using namespace cc::primitive_defines;

namespace flags_test
{
enum class shape : u32
{
    none = 0,
    visible = 1u << 0,
    selected = 1u << 1,
    locked = 1u << 2,
    all = (1u << 3) - 1,
};

/// storage narrower than the enum's own underlying type — what the explicit storage argument is for.
enum class narrow : u32
{
    a = 1u << 0,
    b = 1u << 7,
};

/// declares u8 storage but names a bit above it — the mismatch nothing else can catch.
enum class overflowing : u32
{
    low = 1u << 0,
    high = 1u << 8,
};

/// a PLAIN enum, written with no bit patterns in mind — the case CC_FLAG_ENUM_INDEXED exists for.
enum class plain_indexed
{
    e1,
    e2,
    e3,
};

/// indexed, but names a value at or beyond the declared storage's bit count.
enum class indexed_overflowing
{
    low = 0,
    high = 8,
};

/// opted in by hand rather than by macro, and therefore without operators.
enum class manual : u16
{
    x = 1u << 0,
    y = 1u << 1,
};

/// never opted in at all.
enum class plain : u32
{
    a = 1,
    b = 2,
};
} // namespace flags_test

CC_FLAG_ENUM_BITMASK(flags_test, shape, u32);
CC_FLAG_ENUM_BITMASK(flags_test, narrow, u8);
CC_FLAG_ENUM_BITMASK(flags_test, overflowing, u8);
CC_FLAG_ENUM_INDEXED(flags_test, plain_indexed, u32);
CC_FLAG_ENUM_INDEXED(flags_test, indexed_overflowing, u8);

template <>
struct cc::custom::enum_traits<flags_test::manual>
{
    static constexpr bool is_flag_enum = true;
    static constexpr cc::flag_encoding flag_encoding = cc::flag_encoding::bit_mask;
    using flag_storage_type = u16;
};

// =========================================================================================================
// Opt-in
// =========================================================================================================

static_assert(cc::flag_enum<flags_test::shape>);
static_assert(cc::flag_enum<flags_test::manual>, "a hand-written enum_traits specialization opts in just as well");
static_assert(!cc::flag_enum<flags_test::plain>, "an enum that never opted in is not a flag enum");
static_assert(!cc::flag_enum<int>);

static_assert(cc::flag_enum<flags_test::plain_indexed>);

static_assert(sizeof(cc::flags<flags_test::shape>) == 4);
static_assert(sizeof(cc::flags<flags_test::narrow>) == 1, "storage comes from the traits, not from the underlying type");
static_assert(sizeof(cc::flags<flags_test::manual>) == 2);

static_assert(cc::flags<flags_test::shape>::encoding == cc::flag_encoding::bit_mask);
static_assert(cc::flags<flags_test::plain_indexed>::encoding == cc::flag_encoding::bit_index);

// =========================================================================================================
// bit_index — a plain enum, one bit per value
// =========================================================================================================

namespace
{
using flags_test::plain_indexed;
using indexed_flags = cc::flags<plain_indexed>;

consteval bool verify_indexed_encoding()
{
    // the value IS the bit position, so e1 == 0 lands on bit 0 rather than on nothing
    if (indexed_flags(plain_indexed::e1).bits != 0b001)
        return false;
    if (indexed_flags(plain_indexed::e2).bits != 0b010)
        return false;
    if (indexed_flags(plain_indexed::e3).bits != 0b100)
        return false;

    auto f = plain_indexed::e1 | plain_indexed::e3;
    if (f.bits != 0b101 || f.set_bit_count() != 2)
        return false;
    if (!f.has(plain_indexed::e1) || f.has(plain_indexed::e2))
        return false;

    f.set(plain_indexed::e2);
    if (f.bits != 0b111)
        return false;
    f.remove(plain_indexed::e1);
    if (f != (plain_indexed::e2 | plain_indexed::e3))
        return false;
    f.toggle(plain_indexed::e2);
    if (f != indexed_flags(plain_indexed::e3))
        return false;

    return f.without(plain_indexed::e3).is_empty();
}
static_assert(verify_indexed_encoding());
} // namespace

// =========================================================================================================
// The default state, and what guaranteeing it costs
// =========================================================================================================

namespace
{
/// `f` is DEFAULT-initialized, not value-initialized, so its bits are zero only because the member carries an initializer.
/// Drop that initializer and this stops compiling rather than silently reading garbage:
/// reading an indeterminate value is ill-formed in a constant expression.
consteval bool default_init_is_the_empty_set()
{
    cc::flags<flags_test::shape> f;
    return f.bits == 0 && f.is_empty() && f.set_bit_count() == 0;
}
} // namespace

static_assert(default_init_is_the_empty_set());
static_assert(cc::flags<flags_test::shape>().is_empty(), "and value-initialization agrees");

// What the initializer costs, pinned so the trade stays deliberate: trivial DEFAULT CONSTRUCTION, and nothing else.
// Every triviality requirement in clean-core is trivial copyability and/or destructibility — see allocation.hh's
// create_uninitialized — so cc::flags still satisfies all of them.
static_assert(std::is_trivially_copyable_v<cc::flags<flags_test::shape>>);
static_assert(std::is_trivially_destructible_v<cc::flags<flags_test::shape>>);
static_assert(!std::is_trivially_default_constructible_v<cc::flags<flags_test::shape>>);

// =========================================================================================================
// Structural type — a flag set as a non-type template parameter, which is what tg::transform_flags needs
// =========================================================================================================

template <cc::flags<flags_test::shape> F>
struct nttp_probe
{
    static constexpr i32 bit_count = F.set_bit_count();
};

static_assert(nttp_probe<flags_test::shape::visible | flags_test::shape::locked>::bit_count == 2);
static_assert(nttp_probe<cc::flags<flags_test::shape>{}>::bit_count == 0);

// =========================================================================================================
// Compile-time set algebra
// =========================================================================================================

namespace
{
using shape = flags_test::shape;
using shape_flags = cc::flags<shape>;

consteval bool verify_set_algebra()
{
    auto const f = shape::visible | shape::selected;

    if (!f.has(shape::visible) || !f.has(shape::selected) || f.has(shape::locked))
        return false;

    if (!f.has_any(shape::locked | shape::visible))
        return false;
    if (f.has_all(shape::locked | shape::visible))
        return false;

    if ((f | shape::locked) != shape_flags(shape::all))
        return false;
    if ((f & shape::visible) != shape_flags(shape::visible))
        return false;
    if ((f ^ shape::visible) != shape_flags(shape::selected))
        return false;

    if (f.without(shape::selected) != shape_flags(shape::visible))
        return false;
    if (!f.without(shape::all).is_empty())
        return false;

    return true;
}
static_assert(verify_set_algebra());
} // namespace

TEST("flags - construction")
{
    SECTION("default is empty")
    {
        auto const f = shape_flags();
        CHECK(f.is_empty());
        CHECK(f.bits == 0);
        CHECK(f.set_bit_count() == 0);
    }

    SECTION("a single flag converts implicitly")
    {
        shape_flags const f = shape::visible;
        CHECK(f.bits == 1);
        CHECK(f.has(shape::visible));
    }

    SECTION("several flags at once")
    {
        auto const f = shape_flags(shape::visible, shape::selected, shape::locked);
        CHECK(f == shape_flags(shape::all));
        CHECK(f.set_bit_count() == 3);
    }

    SECTION("create_from_bits round-trips")
    {
        auto const f = shape_flags::create_from_bits(0b101);
        CHECK(f.has(shape::visible));
        CHECK(f.has(shape::locked));
        CHECK(!f.has(shape::selected));
    }
}

TEST("flags - queries")
{
    auto const f = shape::visible | shape::selected;

    SECTION("has is a subset test for multi-bit values")
    {
        CHECK(f.has(shape::visible));
        CHECK(!f.has(shape::all));
        CHECK(f.has(shape::none)).note("the empty set is a subset of everything");
    }

    SECTION("has_any and has_all differ on a partial overlap")
    {
        auto const probe = shape::visible | shape::locked;
        CHECK(f.has_any(probe));
        CHECK(!f.has_all(probe));
    }

    SECTION("set_bit_count counts bits, not enumerators")
    {
        CHECK(f.set_bit_count() == 2);
        CHECK(shape_flags(shape::all).set_bit_count() == 3).note("`all` names three bits and counts as three");
    }
}

TEST("flags - modifiers")
{
    auto f = shape_flags();

    f.set(shape::visible);
    CHECK(f.has(shape::visible));

    f.set(shape::locked, true);
    f.set(shape::visible, false);
    CHECK(f == shape_flags(shape::locked));

    f.toggle(shape::selected);
    CHECK(f.has(shape::selected));
    f.toggle(shape::selected);
    CHECK(!f.has(shape::selected));

    f.remove(shape::locked);
    CHECK(f.is_empty());

    f |= shape::visible | shape::selected;
    f &= shape::selected | shape::locked;
    CHECK(f == shape_flags(shape::selected));

    f ^= shape::selected;
    CHECK(f.is_empty());

    f.set(shape::visible);
    f.clear();
    CHECK(f.is_empty());
}

TEST("flags - narrow storage keeps the enum's bit positions")
{
    auto const f = flags_test::narrow::a | flags_test::narrow::b;

    CHECK(f.bits == 0b1000'0001);
    CHECK(f.set_bit_count() == 2);
    CHECK(f.has(flags_test::narrow::b));
}

TEST("flags - a value wider than the declared storage asserts")
{
    SECTION("bit_mask: the pattern must fit the storage")
    {
        CHECK(cc::flags<flags_test::overflowing>(flags_test::overflowing::low).bits == 1);
        CHECK_ASSERTS(cc::flags<flags_test::overflowing>(flags_test::overflowing::high));
    }

    SECTION("bit_index: the index must be below the storage's bit count")
    {
        CHECK(cc::flags<flags_test::indexed_overflowing>(flags_test::indexed_overflowing::low).bits == 1);
        CHECK_ASSERTS(cc::flags<flags_test::indexed_overflowing>(flags_test::indexed_overflowing::high));
    }
}

TEST("flags - bit_index gives a plain enum one bit per value")
{
    auto f = flags_test::plain_indexed::e1 | flags_test::plain_indexed::e3;

    CHECK(f.bits == 0b101);
    CHECK(f.has(flags_test::plain_indexed::e3));
    CHECK(!f.has(flags_test::plain_indexed::e2));
    CHECK(f.set_bit_count() == 2);

    f.set(flags_test::plain_indexed::e2, true);
    CHECK(f.bits == 0b111);
    CHECK(f.without(flags_test::plain_indexed::e1).bits == 0b110);
}

TEST("flags - hand-written traits need no macro")
{
    auto const f = cc::flags<flags_test::manual>(flags_test::manual::x, flags_test::manual::y);

    CHECK(f.has(flags_test::manual::x));
    CHECK(f.set_bit_count() == 2);
}

TEST("flags - equal sets hash equally")
{
    auto const a = shape::visible | shape::locked;
    auto const b = shape_flags(shape::locked, shape::visible);

    CHECK(a == b);
    CHECK(cc::make_hash(a) == cc::make_hash(b));
    CHECK(cc::make_hash(a) != cc::make_hash(shape_flags(shape::all)));
}
