#include <clean-core/common/flags.hh>
#include <nexus/test.hh>
#include <typed-geometry/transform/transform_flags.hh>

using namespace cc::primitive_defines;

namespace
{
using tg::impl::transform_flag;
using tg::impl::transform_flags;
namespace tc = tg::impl::transform_class;

/// every bit pattern the seven flags can form.
/// The enum is INDEXED, so a raw pattern is built with create_from_bits — transform_flag(bits) would mean bit number `bits`.
constexpr int flag_pattern_count = 1 << 7;

[[nodiscard]] consteval transform_flags flag_set_from_pattern(int bits)
{
    return transform_flags::create_from_bits(u32(bits));
}

/// the distinct canonical classes, in the order canonical() first produces them.
struct class_list
{
    transform_flags data[32] = {};
    int count = 0;
};

consteval class_list canonical_classes()
{
    class_list r;
    for (int bits = 0; bits < flag_pattern_count; ++bits)
    {
        auto const c = tg::impl::transform_canonical(flag_set_from_pattern(bits));

        bool known = false;
        for (int i = 0; i < r.count; ++i)
            if (r.data[i] == c)
                known = true;

        if (!known)
            r.data[r.count++] = c;
    }
    return r;
}

/// The whole flag algebra, proven exhaustively over all 64 bit patterns.
///
/// canonical() must be a closure operator and canonical(a | b) must be the least upper bound in the class lattice —
/// the `as_*` views and the return type of compose both rest on those two facts.
consteval bool verify_transform_flag_lattice()
{
    // canonical() is a closure operator on the raw bit patterns
    for (int a = 0; a < flag_pattern_count; ++a)
    {
        auto const fa = flag_set_from_pattern(a);
        auto const ca = tg::impl::transform_canonical(fa);

        if (tg::impl::transform_canonical(ca) != ca)
            return false; // idempotent
        if (!tg::impl::transform_is_canonical(ca))
            return false;
        if (!tg::impl::transform_is_subclass(fa, ca))
            return false; // a flag set belongs to its own class
        if (tg::impl::transform_canonical(fa | ca) != ca)
            return false; // and adds nothing to it
    }

    // the join is the least upper bound.
    // Quantifying over the classes rather than over all 64 bit patterns is enough:
    // canonical(a | b) only depends on canonical(a) and canonical(b).
    auto const list = canonical_classes();
    auto const& classes = list.data;
    int const n = list.count;

    // Tabulated first, because the least-upper-bound loop below asks the same n^2 questions n times over.
    // Every join is itself one of the classes, which is what lets the tables be indexed rather than searched.
    bool is_sub[32][32] = {};
    int join_index[32][32] = {};

    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            is_sub[i][j] = tg::impl::transform_is_subclass(classes[i], classes[j]);

            auto const join = tg::impl::transform_canonical(classes[i] | classes[j]);
            if (!tg::impl::transform_is_canonical(join))
                return false; // closed

            join_index[i][j] = -1;
            for (int k = 0; k < n; ++k)
                if (classes[k] == join)
                    join_index[i][j] = k;

            if (join_index[i][j] < 0)
                return false; // and lands on a class we know
        }

    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            if (join_index[i][j] != join_index[j][i])
                return false; // commutative

            int const join = join_index[i][j];
            if (!is_sub[i][join] || !is_sub[j][join])
                return false; // an upper bound

            for (int k = 0; k < n; ++k)
            {
                // the LEAST upper bound: nothing containing both may fail to contain the join
                if (is_sub[i][k] && is_sub[j][k] && !is_sub[join][k])
                    return false;
            }
        }

    return true;
}
static_assert(verify_transform_flag_lattice());

// the nine linear classes with and without translation, plus projective
static_assert(canonical_classes().count == 19);

// the named classes are all canonical, so a user never trips the transform's static_assert
static_assert(tg::impl::transform_is_canonical(tc::identity));
static_assert(tg::impl::transform_is_canonical(tc::translation));
static_assert(tg::impl::transform_is_canonical(tc::uniform_scaling));
static_assert(tg::impl::transform_is_canonical(tc::uniform_scaling_translation));
static_assert(tg::impl::transform_is_canonical(tc::scaling));
static_assert(tg::impl::transform_is_canonical(tc::scaling_translation));
static_assert(tg::impl::transform_is_canonical(tc::rotation));
static_assert(tg::impl::transform_is_canonical(tc::rigid));
static_assert(tg::impl::transform_is_canonical(tc::scaled_rotation));
static_assert(tg::impl::transform_is_canonical(tc::similarity));
static_assert(tg::impl::transform_is_canonical(tc::linear));
static_assert(tg::impl::transform_is_canonical(tc::affine));
static_assert(tg::impl::transform_is_canonical(tc::projective));
} // namespace

TEST("tg transform_flags - canonicalization")
{
    SECTION("non-uniform scaling subsumes uniform scaling")
    {
        CHECK(tg::impl::transform_canonical(transform_flag::uniform_scaling | transform_flag::non_uniform_scaling)
              == transform_flag::non_uniform_scaling);
    }

    SECTION("rotation with non-uniform scaling is a general linear map")
    {
        // R1 S1 R2 S2 is not of the form R S, so the class is not closed without general_linear
        CHECK(tg::impl::transform_canonical(transform_flag::rotation | transform_flag::non_uniform_scaling) == tc::linear);
        CHECK(tc::linear.has_all(transform_flag::general_linear));
    }

    SECTION("projection is the top of the lattice")
    {
        CHECK(tg::impl::transform_canonical(transform_flag::projection) == tg::impl::transform_flag_all);
        CHECK(tc::projective == tg::impl::transform_flag_all);
    }

    SECTION("a rigid transform is exactly rotation plus translation")
    {
        CHECK(tc::rigid == (transform_flag::rotation | transform_flag::translation));
    }
}

TEST("tg transform_flags - is_subclass is not a bit-subset test")
{
    // This is the module's easiest mistake: canonical() CLEARS uniform_scaling when non_uniform_scaling is present,
    // so affine does not contain similarity's bits even though every similarity IS an affine map.
    SECTION("the bit test gets it wrong")
    {
        CHECK(!tc::affine.has_all(tc::similarity));
    }

    SECTION("the class test gets it right")
    {
        CHECK(tg::impl::transform_is_subclass(tc::similarity, tc::affine));
        CHECK(tg::impl::transform_is_subclass(tc::rigid, tc::similarity));
        CHECK(tg::impl::transform_is_subclass(tc::rotation, tc::rigid));
        CHECK(tg::impl::transform_is_subclass(tc::translation, tc::rigid));
        CHECK(tg::impl::transform_is_subclass(tc::affine, tc::projective));
        CHECK(tg::impl::transform_is_subclass(tc::uniform_scaling, tc::scaling_translation));
    }

    SECTION("containment is strict where it should be")
    {
        CHECK(!tg::impl::transform_is_subclass(tc::rigid, tc::rotation));
        CHECK(!tg::impl::transform_is_subclass(tc::rotation, tc::translation));
        CHECK(!tg::impl::transform_is_subclass(tc::scaling, tc::similarity));
        CHECK(!tg::impl::transform_is_subclass(tc::projective, tc::affine));
    }

    SECTION("every class contains itself")
    {
        CHECK(tg::impl::transform_is_subclass(tc::identity, tc::identity));
        CHECK(tg::impl::transform_is_subclass(tc::affine, tc::affine));
    }
}

TEST("tg transform_flags - set operations")
{
    SECTION("subtraction is what replaced the masked complement")
    {
        CHECK(tc::projective.without(tg::impl::transform_flag_all).is_empty());
        CHECK(tc::projective.without(tc::rigid)
              == (transform_flag::uniform_scaling | transform_flag::non_uniform_scaling
                  | transform_flag::negative_scaling | transform_flag::general_linear | transform_flag::projection));
    }

    SECTION("has_any / has_all / without")
    {
        CHECK(!tc::rigid.is_empty());
        CHECK(tc::identity.is_empty());
        CHECK(tc::rigid.has_all(transform_flag::rotation));
        CHECK(tc::rigid.without(transform_flag::rotation) == transform_flag::translation);
    }
}
