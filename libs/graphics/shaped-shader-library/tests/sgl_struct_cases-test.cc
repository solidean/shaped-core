#include <nexus/test.hh>
#include <slib_test_sgl_shaders.hh>
#include <typed-geometry/linalg/vec.hh>

// The host types generated for the structs with cases of struct_cases.sgl (SGL CHK-540): a value built by its leaf's
// factory, asked its case, and read through its fields' accessors, as the shader reads the same bytes.

namespace
{
namespace shaders = slib_test::sgl_shaders::struct_cases;
} // namespace

TEST("slib sgl struct cases - a leaf's factory writes its tag, and its accessors read the words it wrote")
{
    auto const s = shaders::shape::sphere(tg::vec3f(1, 0, 0), tg::vec3f(0, 2, 0), 3.0f);
    CHECK(s.is_sphere());
    CHECK(!s.is_box());
    CHECK(!s.is_flat());
    CHECK(s.tint[0] == 1.0f);
    CHECK(s.center()[1] == 2.0f);
    CHECK(s.sphere_radius() == 3.0f);

    // a case that holds cases is asked as its leaves are, and its field reads on each of them
    auto const d = shaders::shape::disk(tg::vec3f(0), tg::vec3f(0, 0, 1), 0.5f);
    CHECK(d.is_flat());
    CHECK(d.is_disk());
    CHECK(d.normal()[2] == 1.0f);
    // sphere's radius and disk's are two fields, as SGL has them, each named after its case
    CHECK(d.disk_radius() == 0.5f);
}

TEST("slib sgl struct cases - a setter writes a field of the value's case")
{
    auto b = shaders::shape::box(tg::vec3f(0), tg::vec3f(0), tg::vec3f(1));
    b.set_hi(tg::vec3f(4, 5, 6));
    CHECK(b.hi()[1] == 5.0f);
    CHECK(b.lo()[0] == 0.0f);
}

TEST("slib sgl struct cases - a struct's @whens are its host's tests, and its factories write the fields they read")
{
    auto const t = shaders::material::thin_glass(0.25f, 1.33f);
    CHECK(t.is_thin_glass());
    CHECK(!t.is_glass());
    CHECK(t.kind == 0);
    CHECK(t.sides == 2);
    CHECK(t.thin_glass_ior() == 1.33f);

    auto const m = shaders::material::metal(2, 0.5f, tg::vec3f(0.9f));
    CHECK(m.is_metal());
    CHECK(m.f0()[0] == 0.9f);
}
