#include "fake_compiler.hh"

#include <nexus/test.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

// The generated package (see sc_add_shader_package in this library's CMakeLists). These pin the codegen
// contract itself: the symbols exist and are typed, the table matches what was declared, and the include
// closure got embedded so a binary with no source tree still has its shaders.
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/raster/vertex_input.hh>
#include <slib_test_shaders.hh>

using namespace cc::primitive_defines;

using slib_test::fake_compiler;

TEST("slib - the generated package exposes a symbol per stage and entry point", exclusive("slib-shader-library"))
{
    // The payoff of generating C++: this is checked by the compiler, so a typo is a build error rather
    // than a runtime lookup that returns nothing.
    slib::shader_library lib;
    lib.add_package(slib_test::shaders::package());

    REQUIRE(slib_test::shaders::invert.compute.main != nullptr);
    REQUIRE(slib_test::shaders::blit.vertex.main_vs != nullptr);
    REQUIRE(slib_test::shaders::blit.fragment.main_ps != nullptr);

    CHECK(slib_test::shaders::invert.compute.main->stage() == sg::shader_stage::compute);
    CHECK(slib_test::shaders::blit.vertex.main_vs->stage() == sg::shader_stage::vertex);
    CHECK(slib_test::shaders::blit.fragment.main_ps->stage() == sg::shader_stage::fragment);

    CHECK(slib_test::shaders::invert.compute.main->entry_point() == "main");
    CHECK(slib_test::shaders::blit.vertex.main_vs->entry_point() == "main_vs");
    CHECK(slib_test::shaders::blit.fragment.main_ps->entry_point() == "main_ps");

    // The declared path is carried through, not rebuilt from folder + stem.
    CHECK(slib_test::shaders::invert.compute.main->virtual_path() == "slib_test_shaders/invert.hlsl");
    CHECK(slib_test::shaders::blit.vertex.main_vs->virtual_path() == "slib_test_shaders/blit.hlsl");
}

TEST("slib - the generated package describes itself", exclusive("slib-shader-library"))
{
    auto const& pkg = slib_test::shaders::package();

    CHECK(pkg.name == "slib_test_shaders");
    CHECK(pkg.language == slib::shader_language::hlsl);
    // One per declared entry point; a binding or vertex_input entry declares none, so neither adds one here.
    CHECK(pkg.definitions.size() == 6);
    CHECK(!pkg.source_dir.empty()); // baked absolute at configure; present in a dev build

    for (auto const& definition : pkg.definitions)
    {
        CHECK(!definition.path.empty());
        CHECK(!definition.entry_point.empty());
        CHECK(definition.asset != nullptr);
    }
}

TEST("slib - the generated package embeds its include closure", exclusive("slib-shader-library"))
{
    auto const& pkg = slib_test::shaders::package();

    // Four shader files, plus the two .hlsli they include -- one of which is also registered on its own as
    // a binding entry, and is embedded once either way.
    // The include is the point: ssc::dxc has no filesystem fallback, so a shipped build that embedded only the entry points could not resolve it.
    REQUIRE(pkg.embedded_files.size() == 6);

    bool has_invert = false;
    bool has_blit = false;
    bool has_include = false;
    for (auto const& file : pkg.embedded_files)
    {
        CHECK(!file.text.empty());
        if (file.path == "invert.hlsl")
            has_invert = true;
        if (file.path == "blit.hlsl")
            has_blit = true;
        if (file.path == "util/common.hlsli")
            has_include = true;
    }
    CHECK(has_invert);
    CHECK(has_blit);
    CHECK(has_include);
}

TEST("slib - the generated package compiles from its embedded sources alone", exclusive("slib-shader-library"))
{
    // What a shipped binary does: no source tree, only what the generator baked in.
    // Here we take the real package and prove the embedded copy is complete enough to build from.
    auto const& pkg = slib_test::shaders::package();

    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.add_package(pkg, std::make_shared<slib::embedded_filesystem>(pkg.embedded_files));

    auto const shader = slib_test::shaders::invert.compute.main->acquire(sg::shader_format::dxil);
    REQUIRE(shader->has_value());

    // The fake compiler inlines includes, so a flattened body proves the .hlsli resolved from the
    // embedded copy rather than the disk.
    auto const source = fake_compiler::source_of(*shader->try_value());
    CHECK(source.contains("slib_test_invert"));
    CHECK(!source.contains("#include"));
}

TEST("slib - a binding entry generates the group its namespace declares", exclusive("slib-shader-library"))
{
    // The typed half of the codegen: one named member per binding, and the addresses as constants rather
    // than something reflected out of a compiled shader.
    using group = slib_test::shaders::frame_bindings::group;

    CHECK(group::group_index == 0);

    auto const bindings = group::declared_bindings();
    REQUIRE(bindings.size() == 3);

    // Declaration order, and one counter across register classes — the sampler takes index 1, not s0.
    CHECK(bindings[0].name == "albedo");
    CHECK(bindings[0].index == 0);
    CHECK(bindings[0].type == sg::binding_type::readonly_texture);
    CHECK(bindings[0].texture_dimension.value() == sg::texture_view_dimension::tex_2d);

    CHECK(bindings[1].name == "linear_sampler");
    CHECK(bindings[1].index == 1);
    CHECK(bindings[1].type == sg::binding_type::sampler);

    CHECK(bindings[2].name == "histogram");
    CHECK(bindings[2].index == 2);
    CHECK(bindings[2].type == sg::binding_type::readwrite_structured_buffer);

    // The group number is both the SPIR-V set and the HLSL space, so every binding carries it twice.
    for (auto const& binding : bindings)
    {
        REQUIRE(binding.group_index.has_value());
        REQUIRE(binding.space.has_value());
        CHECK(binding.group_index.value() == 0);
        CHECK(binding.space.value() == 0);
    }
}

TEST("slib - a `static` sampler reaches the layout rather than the group", exclusive("slib-shader-library"))
{
    using group = slib_test::shaders::frame_bindings::group;

    // It still occupies its slot and its register: only where the state comes from changes.
    CHECK(group::declared_bindings()[1].name == "linear_sampler");
    CHECK(group::declared_bindings()[1].index == 1);

    auto const declared = group::declared_samplers();
    REQUIRE(declared.size() == 1);
    CHECK(declared[0].name == "linear_sampler");
    CHECK(declared[0].sampler.address_u == sg::sampler_address_mode::clamp_edge);
    CHECK(declared[0].sampler.address_v == sg::sampler_address_mode::clamp_edge);
    CHECK(declared[0].sampler.address_w == sg::sampler_address_mode::clamp_edge);

    // Everything omitted takes sg::sampler's default, which is a trilinear repeating sampler.
    CHECK(declared[0].sampler.min_filter == sg::sampler_filter::linear);
    CHECK(!declared[0].sampler.compare.has_value());

    // And the struct has no field for it: a caller cannot supply what the layout already bakes in.
    // `group{}` names every remaining member, so this stops compiling the day that changes.
    auto const g = group{.albedo = {}, .histogram = {}};
    (void)g;
}

TEST("slib - the generated table and the runtime pass read one shader the same way", exclusive("slib-shader-library"))
{
    // The self-check every package carries: the table came from the Python half of the pass, and this
    // parses the embedded bytes with the C++ half.
    // Its corpus is every shader anyone declares, so it grows without anyone remembering to extend it.
    auto const difference = slib_test::shaders::frame_bindings::group::self_check();
    if (!difference.empty())
        FAIL(difference);
    CHECK(difference.empty());
}

TEST("slib - a vertex input entry mirrors the struct and describes it to sg", exclusive("slib-shader-library"))
{
    // The mirror is what defines the buffer's byte layout, and the generated specialization states that same
    // layout — so the two cannot disagree, which is the failure a hand-written pair has no way to catch.
    using vertex = slib_test::shaders::vs_input;
    using instance = slib_test::shaders::instance_input;

    auto const layout = sg::vertex_input_layout::create<vertex, instance>();

    REQUIRE(layout.slots.size() == 2);
    CHECK(layout.slots[0].stride == isize(sizeof(vertex)));
    CHECK(!layout.slots[0].per_instance);
    CHECK(layout.slots[1].stride == isize(sizeof(instance)));
    CHECK(layout.slots[1].per_instance); // the attribute said so

    REQUIRE(layout.attributes.size() == 5);

    // Declaration order, each attribute naming the slot its struct took in create<>().
    CHECK(layout.attributes[0].semantic == "POSITION");
    CHECK(layout.attributes[0].format == sg::vertex_attribute_format::vec3f);
    CHECK(layout.attributes[0].offset == 0);
    CHECK(layout.attributes[0].slot == 0);

    CHECK(layout.attributes[2].semantic == "COLOR");
    CHECK(layout.attributes[2].offset == 24);

    // A semantic's trailing integer is its index, the way HLSL reads it.
    CHECK(layout.attributes[3].semantic == "TEXCOORD");
    CHECK(layout.attributes[3].semantic_index == 0);
    CHECK(layout.attributes[3].slot == 1);
    CHECK(layout.attributes[4].semantic == "TEXCOORD");
    CHECK(layout.attributes[4].semantic_index == 1);
    CHECK(layout.attributes[4].format == sg::vertex_attribute_format::u32);
}

TEST("slib - a payload entry mirrors the struct and sizes it", exclusive("slib-shader-library"))
{
    using payload = slib_test::shaders::trace_payload;

    // The number sv maintains by hand today, against a struct in another language.
    // 24 is natural alignment; a constant buffer's 16-byte rows would have made the same members 32.
    CHECK(payload::max_payload_size == 24);
    CHECK(isize(sizeof(payload)) == payload::max_payload_size);

    // The generator's own offsets are asserted in the generated header; this is the shape a caller sees.
    auto p = payload{};
    p.uv[0] = 1.0f;
    p.radiance[2] = 2.0f;
    p.depth = 3;
    CHECK(p.depth == 3u);
}

TEST("slib - a constants entry mirrors the block with HLSL's padding, not C++'s", exclusive("slib-shader-library"))
{
    using constants = slib_test::shaders::shade_constants;

    // `tint` cannot straddle the first 16-byte row, so it lands at 16 rather than at 8, which is the whole
    // reason the mirror is generated rather than transcribed.
    // The naive C++ struct would be 24 bytes.
    CHECK(sizeof(constants) == 32);
    CHECK(offsetof(constants, uv_scale) == 0);
    CHECK(offsetof(constants, tint) == 16);
    CHECK(offsetof(constants, exposure) == 28);

    // The generated header asserts all of that too; this is the shape a caller sees.
    auto c = constants{};
    c.uv_scale[0] = 1.0f;
    c.tint[2] = 2.0f;
    c.exposure = 3.0f;
    CHECK(c.exposure == 3.0f);
}

TEST("slib - the generated tables still describe the shaders they came from", exclusive("slib-shader-library"))
{
    // The generator's Python parse against the runtime C++ one, over the exact bytes the package embedded.
    // The corpus covers the cases we thought of; this covers every shader anyone declares, and it grows
    // without anyone remembering to extend it.
    //
    // Here rather than on the render path: it re-parses each source, which is a build-time property to check
    // once, not something a frame should pay for.
    auto const message = slib_test::shaders::self_check();
    CHECK(message.empty());
    if (!message.empty())
        FAIL(message);
}
