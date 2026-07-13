#include <nexus/test.hh>
#include <shaped-graphics/blend_state.hh>
#include <shaped-graphics/depth_stencil_state.hh>
#include <shaped-graphics/primitive_topology.hh>
#include <shaped-graphics/raster_pipeline.hh>
#include <shaped-graphics/rasterization_state.hh>
#include <shaped-graphics/vertex_input.hh>

#include <cstddef> // offsetof

// Pure value-type tests for the raster pipeline's fixed-function vocabulary — no GPU / context needed.
// The real end-to-end pipeline build + draw runs in the dx12 backend suite (dx12-triangle-test.cc).

namespace
{
struct test_vertex
{
    float position[3];
    float color[4];
};
} // namespace

template <>
struct sg::vertex_layout_of<test_vertex>
{
    static sg::vertex_type_layout get()
    {
        return {
            .stride = sizeof(test_vertex),
            .attributes = {
                {.semantic = "POSITION",
                 .format = sg::vertex_attribute_format::vec3f,
                 .offset = offsetof(test_vertex, position)},
                {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec4f, .offset = offsetof(test_vertex, color)},
            }};
    }
};

TEST("sg - raster vertex_input_layout::create derives one slot per vertex type")
{
    auto const layout = sg::vertex_input_layout::create<test_vertex>();

    REQUIRE(layout.slots.size() == 1);
    CHECK(layout.slots[0].stride == cc::isize(sizeof(test_vertex)));
    CHECK(layout.slots[0].per_instance == false);

    REQUIRE(layout.attributes.size() == 2);
    CHECK(layout.attributes[0].semantic == "POSITION");
    CHECK(layout.attributes[0].slot == 0);
    CHECK(layout.attributes[0].offset == 0);
    CHECK(layout.attributes[0].format == sg::vertex_attribute_format::vec3f);
    CHECK(layout.attributes[1].semantic == "COLOR");
    CHECK(layout.attributes[1].slot == 0);
    CHECK(layout.attributes[1].offset == cc::isize(sizeof(float) * 3));
}

TEST("sg - raster vertex_input_layout::create assigns one slot per type in order")
{
    auto const layout = sg::vertex_input_layout::create<test_vertex, test_vertex>();

    REQUIRE(layout.slots.size() == 2);
    REQUIRE(layout.attributes.size() == 4);
    CHECK(layout.attributes[0].slot == 0);
    CHECK(layout.attributes[1].slot == 0);
    CHECK(layout.attributes[2].slot == 1);
    CHECK(layout.attributes[3].slot == 1);
}

TEST("sg - raster color_write_mask flag ops")
{
    using sg::color_write_mask;
    CHECK(sg::has_flag(color_write_mask::all, color_write_mask::r));
    CHECK(sg::has_flag(color_write_mask::all, color_write_mask::a));

    auto const rg = color_write_mask::r | color_write_mask::g;
    CHECK(sg::has_flag(rg, color_write_mask::r));
    CHECK(sg::has_flag(rg, color_write_mask::g));
    CHECK(!sg::has_flag(rg, color_write_mask::b));
}

TEST("sg - raster fixed-function state defaults")
{
    sg::rasterization_state const r;
    CHECK(r.fill == sg::fill_mode::solid);
    CHECK(r.cull == sg::cull_mode::back);
    CHECK(r.front == sg::front_face::counter_clockwise);
    CHECK(r.depth_clip_enabled == true);

    sg::depth_stencil_state const ds;
    CHECK(ds.depth_test == false);
    CHECK(ds.depth_write == false);
    CHECK(ds.stencil_test == false);
    CHECK(ds.depth_compare == sg::compare_op::less);

    // A default-constructed description writes triangles with no depth / no color targets.
    sg::raster_pipeline_description const desc;
    CHECK(desc.topology == sg::primitive_topology::triangle_list);
    CHECK(desc.color_targets.empty());
    CHECK(desc.depth_stencil_format == sg::pixel_format::undefined);
    CHECK(desc.sample_count == 1);
}

TEST("sg - raster topology family mapping")
{
    CHECK(sg::topology_type(sg::primitive_topology::point_list) == sg::primitive_topology_type::point);
    CHECK(sg::topology_type(sg::primitive_topology::line_strip) == sg::primitive_topology_type::line);
    CHECK(sg::topology_type(sg::primitive_topology::triangle_strip) == sg::primitive_topology_type::triangle);
}
