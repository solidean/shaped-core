#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-graphics/views.hh>

#include <memory>

// Pure tests for texture views: the `as_*_view` factories on texture<Traits> build the right strongly-typed
// descriptor, which erases to a `raw_view` with shape `texture`. No GPU — a minimal raw_texture subclass
// (shape/usage metadata only) is enough. The dx12 backend turning these into SRV/UAV descriptors is covered
// in backends/dx12/tests/dx12-texture-view-test.cc.

namespace
{
struct test_texture final : sg::raw_texture
{
    explicit test_texture(sg::texture_description const& d) : sg::raw_texture(d) {}
};

sg::texture_description desc_2d(sg::texture_usage usage, int mips = 1, int samples = 1)
{
    sg::texture_description d;
    d.format = sg::pixel_format::rgba8_unorm;
    d.dimension = sg::texture_dimension::d2;
    d.width = 64;
    d.height = 64;
    d.mip_levels = mips;
    d.sample_count = samples;
    d.usage = usage;
    return d;
}

// A storage (UAV) view exists only on non-multisampled textures — a compile-time contract.
template <class T>
concept has_readwrite_view = requires(T t) { t.as_readwrite_view(); };
} // namespace

static_assert(has_readwrite_view<sg::texture_2d>);
static_assert(!has_readwrite_view<sg::texture_2d_ms>);

TEST("sg - texture as_readonly_view builds a sampled (SRV) view over the whole texture")
{
    auto const d = desc_2d(sg::texture_usage::readonly_texture, /*mips*/ 2);
    sg::raw_texture_handle raw = std::make_shared<test_texture>(d);
    sg::texture_2d tex(raw);

    sg::raw_view const rv = tex.as_readonly_view().to_raw();
    CHECK(rv.access == sg::view_class::readonly);
    CHECK(rv.shape == sg::view_shape::texture);
    CHECK(rv.texture == raw);
    CHECK(rv.buffer == nullptr);
    CHECK(rv.format == sg::pixel_format::rgba8_unorm);
    CHECK(rv.range.mip_range.start == 0);
    CHECK(rv.range.mip_range.end == 2); // all mips
}

TEST("sg - texture as_readwrite_view builds a storage (UAV) view of one mip")
{
    auto const d = desc_2d(sg::texture_usage::readwrite_texture, /*mips*/ 3);
    sg::texture_2d tex(std::make_shared<test_texture>(d));

    sg::raw_view const rv = tex.as_readwrite_view(/*mip*/ 1).to_raw();
    CHECK(rv.access == sg::view_class::readwrite);
    CHECK(rv.shape == sg::view_shape::texture);
    CHECK(rv.range.mip_range.start == 1);
    CHECK(rv.range.mip_range.end == 2); // a UAV targets a single mip
}

TEST("sg - texture views assert on missing usage")
{
    sg::texture_2d storage_only(std::make_shared<test_texture>(desc_2d(sg::texture_usage::readwrite_texture)));
    CHECK_ASSERTS(storage_only.as_readonly_view()); // lacks readonly_texture

    sg::texture_2d sampled_only(std::make_shared<test_texture>(desc_2d(sg::texture_usage::readonly_texture)));
    CHECK_ASSERTS(sampled_only.as_readwrite_view()); // lacks readwrite_texture
}

TEST("sg - texture binding types accept the matching texture view")
{
    auto const d = desc_2d(sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture);
    sg::texture_2d tex(std::make_shared<test_texture>(d));

    CHECK(sg::accepts(sg::binding_type::sampled_texture, tex.as_readonly_view().to_raw()));
    CHECK(sg::accepts(sg::binding_type::storage_texture, tex.as_readwrite_view().to_raw()));
    CHECK(!sg::accepts(sg::binding_type::sampled_texture, tex.as_readwrite_view().to_raw()));           // wrong access
    CHECK(!sg::accepts(sg::binding_type::readonly_structured_buffer, tex.as_readonly_view().to_raw())); // wrong shape
}
