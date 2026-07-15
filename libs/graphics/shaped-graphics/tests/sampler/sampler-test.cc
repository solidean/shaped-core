#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/sampler.hh>

// Pure value/vocabulary tests for samplers: the backend-neutral sampler, the sampler
// binding_type, and named_sampler. No GPU — the dx12 translation + heap wiring live in
// backends/dx12/tests/dx12-sampler-test.cc.

TEST("sg sampler - description defaults are a trilinear repeating sampler")
{
    sg::sampler const s;
    CHECK(s.min_filter == sg::sampler_filter::linear);
    CHECK(s.mag_filter == sg::sampler_filter::linear);
    CHECK(s.mip_filter == sg::sampler_filter::linear);
    CHECK(s.address_u == sg::sampler_address_mode::repeat);
    CHECK(s.address_v == sg::sampler_address_mode::repeat);
    CHECK(s.address_w == sg::sampler_address_mode::repeat);
    CHECK(s.max_anisotropy == 1u); // anisotropy off
    CHECK(!s.compare.has_value()); // not a comparison sampler
    CHECK(s.min_lod == 0.0f);
    CHECK(s.max_lod == sg::sampler::lod_max); // unclamped
}

TEST("sg sampler - descriptions compare by value")
{
    sg::sampler const a;
    sg::sampler b;
    CHECK(a == b);

    b.address_u = sg::sampler_address_mode::clamp_edge;
    CHECK(a != b);

    sg::sampler shadow;
    shadow.compare = sg::compare_op::less_equal;
    CHECK(a != shadow);
}

TEST("sg sampler - binding_type::sampler is not a view")
{
    CHECK(sg::is_sampler(sg::binding_type::sampler));
    CHECK(!sg::is_sampler(sg::binding_type::readonly_texture));
    CHECK(!sg::is_sampler(sg::binding_type::uniform_buffer));

    // A sampler binding is never satisfied by a resource view — samplers are bound as descriptions.
    sg::raw_view const not_a_sampler = sg::raw_texture_view{.access = sg::view_class::readonly};
    CHECK(!sg::accepts(sg::binding_type::sampler, not_a_sampler));
}

TEST("sg sampler - named_sampler pairs a name with a sampler state")
{
    sg::named_sampler const ns{.name = "Samp", .sampler = {.mag_filter = sg::sampler_filter::nearest}};
    CHECK(ns.name == "Samp");
    CHECK(ns.sampler.mag_filter == sg::sampler_filter::nearest);
    CHECK(ns.sampler.min_filter == sg::sampler_filter::linear); // untouched default
}
