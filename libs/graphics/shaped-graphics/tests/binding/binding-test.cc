#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/raw_buffer.hh>

#include <memory>
#include <type_traits>

// The bind-path handles are shared_ptr to immutable backend objects.
static_assert(std::is_same_v<sg::binding_layout_handle, std::shared_ptr<sg::binding_layout const>>);
static_assert(std::is_same_v<sg::compute_pipeline_handle, std::shared_ptr<sg::compute_pipeline const>>);
static_assert(std::is_same_v<sg::binding_group_handle, std::shared_ptr<sg::binding_group const>>);

// The binding vocabulary + compiled_shader data model are pure CPU value types — no GPU backend. A
// minimal concrete buffer subclass produces real views to validate bindings against.

namespace
{
struct test_buffer final : sg::raw_buffer
{
    test_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) : sg::raw_buffer(size_in_bytes, usage) {}
};

struct particle
{
    sg::u32 a, b, c, d;
};

std::shared_ptr<test_buffer> make_buffer(sg::isize size, sg::buffer_usage usage)
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
    sg::raw_view const rw_structured = buf->as_readwrite_buffer<particle>();
    CHECK(sg::accepts(sg::binding_type::readwrite_structured_buffer, rw_structured));
    CHECK(!sg::accepts(sg::binding_type::readonly_structured_buffer, rw_structured)); // access mismatch
    CHECK(!sg::accepts(sg::binding_type::readwrite_raw_buffer, rw_structured));       // shape mismatch

    // A raw rw view satisfies readwrite_raw_buffer, not the structured one.
    sg::raw_view const rw_raw = buf->as_raw_readwrite();
    CHECK(sg::accepts(sg::binding_type::readwrite_raw_buffer, rw_raw));
    CHECK(!sg::accepts(sg::binding_type::readwrite_structured_buffer, rw_raw)); // shape mismatch

    // A read-only structured view.
    sg::raw_view const ro_structured = buf->as_readonly_buffer<particle>();
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
    CHECK(sg::accepts(b.type, buf->as_readwrite_buffer<particle>()));
}

TEST("sg bindings - named_view pairs a name with a bound view")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readwrite_buffer);

    // A typed view converts implicitly to the named_view's raw_view.
    sg::named_view const nv{.name = "Output", .view = buf->as_readwrite_buffer<particle>()};
    CHECK(nv.name == "Output");
    CHECK(nv.view.access == sg::view_class::readwrite);
    CHECK(nv.view.shape == sg::view_shape::structured);
    CHECK(sg::accepts(sg::binding_type::readwrite_structured_buffer, nv.view));
}
