#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <typed-geometry/all.hh>

#include <type_traits>

using namespace cc::primitive_defines;

// Every tg value type names its scalar as a member, so generic code outside tg (a cc::sequence average) can ask for it.
static_assert(std::is_same_v<tg::vec3f::scalar_t, f32>);
static_assert(std::is_same_v<tg::pos<2, f64>::scalar_t, f64>);
static_assert(std::is_same_v<tg::comp3f::scalar_t, f32>);
static_assert(std::is_same_v<tg::bivec<3, f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::mat<3, 3, f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::quat<f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::angle<f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::aabb3f::scalar_t, f32>);
static_assert(std::is_same_v<tg::aabb3f_boundary::scalar_t, f32>);
static_assert(std::is_same_v<tg::triangle3f::scalar_t, f32>);
static_assert(std::is_same_v<tg::segment3f::scalar_t, f32>);
static_assert(std::is_same_v<tg::ray3f::scalar_t, f32>);
static_assert(std::is_same_v<tg::sphere<3, 3, f64>::scalar_t, f64>);
static_assert(std::is_same_v<tg::sphere<2, 3, f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::sphere_boundary<2, 3, f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::box<3, 3, f32>::scalar_t, f32>);
static_assert(std::is_same_v<tg::rigid_transform3f::scalar_t, f32>);

// and it agrees with the shape-reading trait the query layer uses
static_assert(std::is_same_v<tg::triangle3f::scalar_t, tg::traits::scalar_t<tg::triangle3f>>);

TEST("tg scalar_t - a sequence of vectors averages through it")
{
    auto const vs = cc::vector<tg::vec3f>{tg::vec3f(1, 0, 0), tg::vec3f(0, 2, 0), tg::vec3f(2, 4, 3)};
    auto const mean = vs.values().assert_non_empty().average();
    static_assert(std::is_same_v<decltype(mean), tg::vec3f const>);
    CHECK(mean == tg::vec3f(1, 2, 1));
}

TEST("tg scalar_t - a sequence of points averages to their centroid")
{
    auto const ps = cc::vector<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(3, 0, 0), tg::pos3f(0, 3, 6)};
    CHECK(ps.values().assert_non_empty().average() == tg::pos3f(1, 1, 2));
}
