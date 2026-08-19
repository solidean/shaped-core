#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/binding/binding_group_layout.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/fwd.hh> // std::unique_ptr / std::shared_ptr
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/raw_buffer.hh>

#include <type_traits>

using namespace cc::primitive_defines;

// The bind-path handles are shared_ptr to immutable backend objects.
static_assert(std::is_same_v<sg::binding_group_layout_handle, std::shared_ptr<sg::binding_group_layout const>>);
static_assert(std::is_same_v<sg::pipeline_layout_handle, std::shared_ptr<sg::pipeline_layout const>>);
static_assert(std::is_same_v<sg::compute_pipeline_handle, std::shared_ptr<sg::compute_pipeline const>>);
static_assert(std::is_same_v<sg::binding_group_handle, std::shared_ptr<sg::binding_group const>>);

// The binding vocabulary + compiled_shader data model are pure CPU value types — no GPU backend.
// A minimal concrete buffer subclass produces real views to validate bindings against.

namespace
{
struct test_buffer final : sg::raw_buffer
{
    test_buffer(isize size_in_bytes, sg::buffer_usages usage) : sg::raw_buffer(size_in_bytes, usage) {}
};

struct particle
{
    u32 a, b, c, d;
};

std::shared_ptr<test_buffer> make_buffer(isize size, sg::buffer_usages usage)
{
    return std::make_shared<test_buffer>(size, usage);
}
} // namespace

TEST("sg bindings - binding_type maps to view (access/shape)")
{
    using bt = sg::binding_type;
    CHECK(sg::access_of(bt::uniform_buffer) == sg::view_class::uniform);
    CHECK(sg::shape_of(bt::uniform_buffer) == sg::view_shape::uniform_block);

    CHECK(sg::access_of(bt::readonly_structured_buffer) == sg::view_class::readonly);
    CHECK(sg::shape_of(bt::readonly_structured_buffer) == sg::view_shape::structured);

    CHECK(sg::access_of(bt::readwrite_structured_buffer) == sg::view_class::readwrite);
    CHECK(sg::shape_of(bt::readwrite_structured_buffer) == sg::view_shape::structured);

    CHECK(sg::access_of(bt::readonly_raw_buffer) == sg::view_class::readonly);
    CHECK(sg::shape_of(bt::readonly_raw_buffer) == sg::view_shape::raw);

    CHECK(sg::access_of(bt::readwrite_raw_buffer) == sg::view_class::readwrite);
    CHECK(sg::shape_of(bt::readwrite_raw_buffer) == sg::view_shape::raw);
}

TEST("sg bindings - accepts matches a bound view")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer);

    // A rw-structured view satisfies exactly a readwrite_structured_buffer binding.
    sg::raw_view const rw_structured = sg::buffer<particle>::from_raw(buf).as_readwrite_buffer();
    CHECK(sg::accepts(sg::binding_type::readwrite_structured_buffer, rw_structured));
    CHECK(!sg::accepts(sg::binding_type::readonly_structured_buffer, rw_structured)); // access mismatch
    CHECK(!sg::accepts(sg::binding_type::readwrite_raw_buffer, rw_structured));       // shape mismatch

    // A raw rw view satisfies readwrite_raw_buffer, not the structured one.
    sg::raw_view const rw_raw = buf->as_raw_readwrite();
    CHECK(sg::accepts(sg::binding_type::readwrite_raw_buffer, rw_raw));
    CHECK(!sg::accepts(sg::binding_type::readwrite_structured_buffer, rw_raw)); // shape mismatch

    // A read-only structured view.
    sg::raw_view const ro_structured = sg::buffer<particle>::from_raw(buf).as_readonly_buffer();
    CHECK(sg::accepts(sg::binding_type::readonly_structured_buffer, ro_structured));
    CHECK(!sg::accepts(sg::binding_type::readwrite_structured_buffer, ro_structured)); // access mismatch
}

TEST("sg bindings - compiled_shader holds reflection")
{
    // A compute shader reflected to one readwrite-structured binding named "Output".
    sg::compiled_shader shader;
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 1, .z = 1};
    shader.bindings.push_back(sg::binding{
        .name = "Output",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    });

    // The behavioral payload of reflection: the declared binding accepts a matching bound view (a
    // structured buffer carries no block_size, unlike a uniform block).
    REQUIRE(shader.bindings.size() == 1);
    auto const& b = shader.bindings[0];
    CHECK(!b.block_size.has_value());

    auto const buf = make_buffer(256, sg::buffer_usage::readwrite_buffer);
    CHECK(sg::accepts(b.type, sg::buffer<particle>::from_raw(buf).as_readwrite_buffer()));
}

TEST("sg bindings - merge_bindings unions stages by name")
{
    // Two stages of one pipeline: they share "frame", so the union has three entries.
    auto const raygen
        = cc::vector<sg::binding>{{.name = "frame", .index = 0, .type = sg::binding_type::uniform_buffer},
                                  {.name = "Output", .index = 0, .type = sg::binding_type::readwrite_texture}};
    auto const hit = cc::vector<sg::binding>{
        {.name = "frame", .index = 7, .type = sg::binding_type::uniform_buffer},
        {.name = "Vertices", .index = 1, .type = sg::binding_type::readonly_structured_buffer}};

    auto const merged = sg::merge_bindings({raygen, hit});
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].name == "frame");
    CHECK(merged[1].name == "Output");
    CHECK(merged[2].name == "Vertices");

    // First occurrence wins, so the hit stage's disagreeing index does not overwrite the raygen one.
    CHECK(merged[0].index == 0);

    // The accumulating overload is the same merge, one stage at a time.
    auto acc = cc::vector<sg::binding>();
    sg::merge_bindings(acc, raygen);
    sg::merge_bindings(acc, hit);
    REQUIRE(acc.size() == 3);
    CHECK(acc[2].name == "Vertices");

    // Merging a stage into itself changes nothing.
    sg::merge_bindings(acc, hit);
    CHECK(acc.size() == 3);
}

TEST("sg bindings - split_off_sampler_bindings partitions in order")
{
    auto bindings = cc::vector<sg::binding>{{.name = "Albedo", .index = 0, .type = sg::binding_type::readonly_texture},
                                            {.name = "sPoint", .index = 0, .type = sg::binding_type::sampler},
                                            {.name = "frame", .index = 0, .type = sg::binding_type::uniform_buffer},
                                            {.name = "sLinear", .index = 1, .type = sg::binding_type::sampler}};

    auto const samplers = sg::split_off_sampler_bindings(bindings);

    REQUIRE(samplers.size() == 2);
    CHECK(samplers[0].name == "sPoint");
    CHECK(samplers[1].name == "sLinear");

    REQUIRE(bindings.size() == 2);
    CHECK(bindings[0].name == "Albedo");
    CHECK(bindings[1].name == "frame");

    // A second split has nothing left to take.
    CHECK(sg::split_off_sampler_bindings(bindings).empty());
    CHECK(bindings.size() == 2);
}

TEST("sg bindings - named_view pairs a name with a bound view")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readwrite_buffer);

    // A typed view converts implicitly to the named_view's raw_view.
    sg::named_view const nv = {.name = "Output", .view = sg::buffer<particle>::from_raw(buf).as_readwrite_buffer()};
    CHECK(nv.name == "Output");
    CHECK(sg::access_of(nv.view) == sg::view_class::readwrite);
    CHECK(sg::shape_of(nv.view) == sg::view_shape::structured);
    CHECK(sg::accepts(sg::binding_type::readwrite_structured_buffer, nv.view));
}
