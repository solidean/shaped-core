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
    CHECK(sg::view_class_of(bt::constants_buffer, sg::access_mode::read) == sg::view_class::constants);
    CHECK(sg::shape_of(bt::constants_buffer) == sg::view_shape::constants_block);

    CHECK(sg::view_class_of(bt::buffer, sg::access_mode::read) == sg::view_class::readonly);
    CHECK(sg::shape_of(bt::buffer) == sg::view_shape::structured);

    CHECK(sg::view_class_of(bt::buffer, sg::access_mode::read_write) == sg::view_class::readwrite);
    CHECK(sg::shape_of(bt::buffer) == sg::view_shape::structured);

    CHECK(sg::view_class_of(bt::bytes, sg::access_mode::read) == sg::view_class::readonly);
    CHECK(sg::shape_of(bt::bytes) == sg::view_shape::bytes);

    CHECK(sg::view_class_of(bt::bytes, sg::access_mode::read_write) == sg::view_class::readwrite);
    CHECK(sg::shape_of(bt::bytes) == sg::view_shape::bytes);
}

TEST("sg bindings - only an image is write-only, and only buffers, bytes and images write")
{
    using bt = sg::binding_type;
    using am = sg::access_mode;
    for (auto const a : {am::read, am::write, am::read_write})
        CHECK(sg::is_valid_access(bt::image, a));

    CHECK(sg::is_valid_access(bt::buffer, am::read_write));
    CHECK(sg::is_valid_access(bt::bytes, am::read_write));
    CHECK(!sg::is_valid_access(bt::buffer, am::write)); // no target has a write-only buffer
    CHECK(!sg::is_valid_access(bt::bytes, am::write));

    for (auto const t : {bt::constants_buffer, bt::texture, bt::sampler, bt::acceleration_structure})
    {
        CHECK(sg::is_valid_access(t, am::read));
        CHECK(!sg::is_valid_access(t, am::read_write));
        CHECK(!sg::is_valid_access(t, am::write));
    }
}

TEST("sg bindings - an image is one view class and one kind whatever its access")
{
    using am = sg::access_mode;
    for (auto const a : {am::read, am::write, am::read_write})
        CHECK(sg::view_class_of(sg::binding_type::image, a) == sg::view_class::image);

    // One UAV serves an image's three accesses, so two stages disagreeing on it still share a descriptor.
    CHECK(sg::is_same_kind({.type = sg::binding_type::image, .access = am::read},
                           {.type = sg::binding_type::image, .access = am::write}));
    // A buffer's access picks SRV or UAV, so there it is part of the kind.
    CHECK(!sg::is_same_kind({.type = sg::binding_type::buffer, .access = am::read},
                            {.type = sg::binding_type::buffer, .access = am::read_write}));
}

TEST("sg bindings - accepts matches a bound view")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer);

    // A rw-structured view satisfies exactly a read_write buffer binding.
    sg::raw_view const rw_structured = sg::buffer<particle>::from_raw(buf).as_readwrite_buffer();
    CHECK(sg::accepts({.type = sg::binding_type::buffer, .access = sg::access_mode::read_write}, rw_structured));
    CHECK(!sg::accepts({.type = sg::binding_type::buffer}, rw_structured)); // access mismatch
    CHECK(!sg::accepts({.type = sg::binding_type::bytes, .access = sg::access_mode::read_write},
                       rw_structured)); // shape mismatch

    // A raw rw view satisfies read_write bytes, not a buffer.
    sg::raw_view const rw_raw = buf->as_raw_readwrite();
    CHECK(sg::accepts({.type = sg::binding_type::bytes, .access = sg::access_mode::read_write}, rw_raw));
    CHECK(!sg::accepts({.type = sg::binding_type::buffer, .access = sg::access_mode::read_write}, rw_raw)); // shape mismatch

    // A read-only structured view.
    sg::raw_view const ro_structured = sg::buffer<particle>::from_raw(buf).as_readonly_buffer();
    CHECK(sg::accepts({.type = sg::binding_type::buffer}, ro_structured));
    CHECK(!sg::accepts({.type = sg::binding_type::buffer, .access = sg::access_mode::read_write},
                       ro_structured)); // access mismatch
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
        .index = 0,
        .count = 1,
        .type = sg::binding_type::buffer,
        .access = sg::access_mode::read_write,
    });

    // The behavioral payload of reflection: the declared binding accepts a matching bound view (a
    // structured buffer carries no block_size, unlike a constants block).
    REQUIRE(shader.bindings.size() == 1);
    auto const& b = shader.bindings[0];
    CHECK(!b.block_size.has_value());

    auto const buf = make_buffer(256, sg::buffer_usage::readwrite_buffer);
    CHECK(sg::accepts(b, sg::buffer<particle>::from_raw(buf).as_readwrite_buffer()));
}

TEST("sg bindings - group_index_of agrees or is absent")
{
    // Nothing declares a group index: the bind slot alone decides, which is the HLSL path.
    auto const spaced
        = cc::vector<sg::binding>{{.name = "Tex", .space = 3, .index = 0, .type = sg::binding_type::texture},
                                  {.name = "Buf", .space = 7, .index = 0, .type = sg::binding_type::bytes}};
    CHECK(!sg::group_index_of(spaced).has_value()); // a space is a register namespace, never a bind slot

    // A declaring binding pins the group even when its neighbours stay silent.
    auto const mixed
        = cc::vector<sg::binding>{{.name = "Tex", .index = 0, .type = sg::binding_type::texture},
                                  {.name = "Buf", .group_index = 2, .index = 1, .type = sg::binding_type::bytes}};
    CHECK(sg::group_index_of(mixed) == 2u);
}

TEST("sg bindings - merge_bindings unions stages by name")
{
    // Two stages of one pipeline: they share "frame", so the union has three entries.
    auto const raygen = cc::vector<sg::binding>{
        {.name = "frame", .index = 0, .type = sg::binding_type::constants_buffer},
        {.name = "Output", .index = 0, .type = sg::binding_type::image, .access = sg::access_mode::read_write}};
    auto const hit = cc::vector<sg::binding>{{.name = "frame", .index = 7, .type = sg::binding_type::constants_buffer},
                                             {.name = "Vertices", .index = 1, .type = sg::binding_type::buffer}};

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

TEST("sg bindings - merge_bindings unions an image's access across stages")
{
    auto const image = [](sg::access_mode access)
    {
        return cc::vector<sg::binding>{{.name = "Target", .index = 0, .type = sg::binding_type::image, .access = access}};
    };
    auto const merged_access = [&](sg::access_mode first, sg::access_mode second)
    { return sg::merge_bindings({image(first), image(second)})[0].access; };

    // A layout must permit what every stage does, so a read in one stage and a write in the next is both.
    CHECK(merged_access(sg::access_mode::read, sg::access_mode::write) == sg::access_mode::read_write);
    CHECK(merged_access(sg::access_mode::write, sg::access_mode::read) == sg::access_mode::read_write);
    CHECK(merged_access(sg::access_mode::read, sg::access_mode::read_write) == sg::access_mode::read_write);
    CHECK(merged_access(sg::access_mode::read_write, sg::access_mode::write) == sg::access_mode::read_write);

    // Agreement leaves it alone, which is what keeps a write-only image valid on WebGPU for any format.
    CHECK(merged_access(sg::access_mode::write, sg::access_mode::write) == sg::access_mode::write);
    CHECK(merged_access(sg::access_mode::read, sg::access_mode::read) == sg::access_mode::read);
}

TEST("sg bindings - split_off_sampler_bindings partitions in order")
{
    auto bindings = cc::vector<sg::binding>{{.name = "Albedo", .index = 0, .type = sg::binding_type::texture},
                                            {.name = "sPoint", .index = 0, .type = sg::binding_type::sampler},
                                            {.name = "frame", .index = 0, .type = sg::binding_type::constants_buffer},
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

TEST("sg bindings - named_view pairs a name with bound views")
{
    auto const buf = make_buffer(256, sg::buffer_usage::readwrite_buffer);

    // A typed view converts implicitly, and a single view stores inline — no vector, no braces.
    sg::named_view const nv = {.name = "Output", .view = sg::buffer<particle>::from_raw(buf).as_readwrite_buffer()};
    CHECK(nv.name == "Output");
    CHECK(nv.view.size() == 1);
    CHECK(sg::view_class_of(nv.view.span()[0]) == sg::view_class::readwrite);
    CHECK(sg::shape_of(nv.view.span()[0]) == sg::view_shape::structured);
    CHECK(sg::accepts({.type = sg::binding_type::buffer, .access = sg::access_mode::read_write}, nv.view.span()[0]));

    // An array binding carries a vector, one view per element; a vacant element is the sg::vacant_view
    // marker, which satisfies every view kind — the backend synthesizes its null descriptor from the binding.
    auto elements = cc::vector<sg::raw_view>();
    for (isize i = 0; i < 3; ++i)
        elements.push_back(sg::vacant_view{});
    sg::named_view const array = {.name = "Textures", .view = cc::move(elements)};
    CHECK(array.view.size() == 3);
    CHECK(sg::is_vacant(array.view.span()[1]));
    CHECK(sg::accepts({.type = sg::binding_type::texture}, array.view.span()[1]));
    CHECK(sg::accepts({.type = sg::binding_type::bytes}, array.view.span()[1]));
    CHECK(!sg::accepts({.type = sg::binding_type::sampler}, array.view.span()[1]));

    // is_array and the reflected texture dimension are what a backend reads off an array binding.
    auto const b = sg::binding{.name = "Textures",
                               .count = 4,
                               .type = sg::binding_type::texture,
                               .texture_dimension = sg::texture_view_dimension::cube};
    CHECK(b.is_array());
    auto const scalar = sg::binding{.name = "One", .count = 1};
    CHECK(!scalar.is_array());
    CHECK(b.texture_dimension == sg::texture_view_dimension::cube);
}

// Visibility is the one field merge_bindings accumulates rather than deduplicating.
// A binding declared by two stages arrives twice — once per compiled_shader, each carrying its own stage — and the
// layout built from the merge has to be visible to both or one stage reads nothing.
TEST("sg::binding - visibility unions across stages")
{
    auto vs = cc::vector<sg::binding>();
    vs.push_back({.name = "camera", .index = 0, .type = sg::binding_type::constants_buffer});
    sg::apply_stage_visibility(vs, sg::shader_stage::vertex);

    auto ps = cc::vector<sg::binding>();
    ps.push_back({.name = "camera", .index = 0, .type = sg::binding_type::constants_buffer});
    ps.push_back({.name = "albedo", .index = 1, .type = sg::binding_type::texture});
    sg::apply_stage_visibility(ps, sg::shader_stage::fragment);

    CHECK(vs[0].visibility.has(sg::shader_stage::vertex));
    CHECK(!vs[0].visibility.has(sg::shader_stage::fragment));

    auto merged = cc::vector<sg::binding>();
    sg::merge_bindings(merged, vs);
    sg::merge_bindings(merged, ps);

    REQUIRE(merged.size() == 2);

    // Shared between the stages, so both bits survive the merge.
    CHECK(merged[0].name == "camera");
    CHECK(merged[0].visibility.has(sg::shader_stage::vertex));
    CHECK(merged[0].visibility.has(sg::shader_stage::fragment));

    // Declared by one stage only, so it stays narrow — which is the whole point: WebGPU rejects a storage binding
    // marked visible to a stage that never declared it.
    CHECK(merged[1].name == "albedo");
    CHECK(!merged[1].visibility.has(sg::shader_stage::vertex));
    CHECK(merged[1].visibility.has(sg::shader_stage::fragment));
}

// Empty means "never said", and stays that way: a hand-written binding is visible everywhere by a backend's choice,
// not by this vocabulary pretending to know.
TEST("sg::binding - visibility defaults to empty")
{
    auto const b = sg::binding{.name = "hand_written", .index = 0, .type = sg::binding_type::constants_buffer};
    CHECK(b.visibility.is_empty());
    CHECK(!b.image_format.has_value());
    CHECK(!b.sample_type.has_value());
    CHECK(!b.sampler_type.has_value());
}
