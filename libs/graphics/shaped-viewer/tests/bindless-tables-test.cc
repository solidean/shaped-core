#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-viewer/resources/bindless_tables.hh>

using namespace cc::primitive_defines;

// sv's bindless layout, which is module tracer's `binding bindless`: the names a footprint resolves by, the dimensions, and
// the ranges a shader indexes.
// Pure — no context, so it runs on every platform, unlike the manager half in gpu-resource-manager-test.cc.

TEST("sv - the bindless layout is module tracer's, one binding per table")
{
    auto const all = sv::bindless_bindings();
    REQUIRE(all.size() == isize(sv::bindless_table::count_));

    // Laid out end to end, because an array consumes one index per element: each table begins where the one before it ended.
    auto next_free = u32(0);
    for (auto i = u32(0); i < u32(sv::bindless_table::count_); ++i)
    {
        auto const& b = all[i];
        CHECK(b.count >= 2);
        CHECK(b.is_array());
        CHECK(b.index == next_free);
        next_free = b.index + b.count;

        // The name is the module's, which is what sg resolves an access declaration's footprint by.
        CHECK(sv::name_of(sv::bindless_table(i)) == cc::string_view(b.name));
        CHECK(cc::string_view(b.name).starts_with("bindless."));
        CHECK(sv::capacity_of(sv::bindless_table(i)) == b.count);
    }

    // A texture table carries the dimension a backend needs for a dimension-correct null descriptor; the buffer
    // table is raw bytes and carries none.
    auto const& tex2d = all[u32(sv::bindless_table::textures_2d)];
    CHECK(tex2d.name == cc::string_view("bindless.textures_2d"));
    CHECK(tex2d.type == sg::binding_type::texture);
    REQUIRE(tex2d.texture_dimension.has_value());
    CHECK(tex2d.texture_dimension.value() == sg::texture_view_dimension::tex_2d);

    auto const& cube = all[u32(sv::bindless_table::textures_cube)];
    REQUIRE(cube.texture_dimension.has_value());
    CHECK(cube.texture_dimension.value() == sg::texture_view_dimension::cube);

    auto const& buffers = all[u32(sv::bindless_table::buffers)];
    CHECK(buffers.name == cc::string_view("bindless.buffers"));
    CHECK(buffers.type == sg::binding_type::bytes);
    CHECK(buffers.access == sg::access_mode::read);
    CHECK(!buffers.texture_dimension.has_value());
}
