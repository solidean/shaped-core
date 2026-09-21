#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/binding/binding_groups.hh> // slib::inline_constants_space
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <slib_test_sgl_shaders.hh>

using namespace cc::primitive_defines;

// The SGL edge against the real compilers behind it.
// The WGSL arm runs everywhere, since slib reflects WGSL itself; the two HLSL arms run where DXC exists.
// A WGSL compile and a failed preprocess hand back a node that is settled already, so only the DXC arms await.

namespace
{
/// The value of a compile that has SETTLED; every test awaits the node first, since a DXC node is cold until something runs it.
sg::compiled_shader const& value_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}

cc::string error_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    REQUIRE(shader->has_error());
    return shader->try_error()->underlying().to_string();
}

[[nodiscard]] sg::binding const* find_binding(sg::compiled_shader const& shader, cc::string_view name)
{
    for (auto const& binding : shader.bindings)
        if (binding.name == name)
            return &binding;
    return nullptr;
}

/// Every SGL edge this build can make, over the real compiler of each format.
void add_sgl_compilers(slib::shader_library& lib)
{
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
#if SLIB_HAS_DXC
    // DXIL reflection needs the Windows SDK, so that edge is Windows' alone.
#ifdef CC_OS_WINDOWS
    auto dxil = slib::create_dxc_compiler();
    REQUIRE(dxil.has_value());
    lib.add_compiler(slib::create_sgl_compiler(cc::move(dxil.value())));
#endif
    auto spirv = slib::create_dxc_spirv_compiler();
    REQUIRE(spirv.has_value());
    lib.add_compiler(slib::create_sgl_compiler(cc::move(spirv.value())));
#endif
}

constexpr auto k_broken_source = cc::string_view("@pixel struct target:\n"
                                                 "    color: float4\n"
                                                 "\n"
                                                 "struct pixel_input:\n"
                                                 "    @position position: hpos4\n"
                                                 "\n"
                                                 "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                                 "    return {\n"
                                                 "        color = float4(missing, 0.0, 0.0, 1.0)\n"
                                                 "    }\n");
/// Stands in for the MSL -> metallib compiler slib does not have; only its format is ever asked for.
class no_metal_compiler final : public slib::shader_compiler
{
public:
    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::hlsl; }
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::metal_lib; }
    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const&,
                                                                   slib::include_resolver) const override
    {
        return cc::error("never called");
    }
    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const&) const override
    {
        return nullptr;
    }
};
} // namespace

TEST("slib sgl compiler - over a metal_lib compiler the flattened source is MSL")
{
    auto const compiler = slib::create_sgl_compiler(std::make_unique<no_metal_compiler>());
    CHECK(compiler->target_format() == sg::shader_format::metal_lib);
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };

    auto const text = compiler->preprocess({.source = "@pixel struct frame:\n"
                                                      "    color: float4\n"
                                                      "struct pixel_input:\n"
                                                      "    @position position: hpos4\n"
                                                      "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                                      "    return {\n"
                                                      "        color = float4(1.0, 0.0, 0.0, 1.0)\n"
                                                      "    }\n",
                                            .entry_point = "main_ps",
                                            .stage = sg::shader_stage::fragment},
                                           resolve);
    REQUIRE(text.has_value());
    CHECK(text.value().source.contains("using namespace metal;"));
    CHECK(text.value().source.contains("fragment frame main_ps(pixel_input p [[stage_in]])"));
}

TEST("slib sgl compiler - the edge is sgl to whatever the inner compiler builds")
{
    auto const compiler = slib::create_sgl_compiler(slib::create_wgsl_compiler());
    CHECK(compiler->source_language() == slib::shader_language::sgl);
    CHECK(compiler->target_format() == sg::shader_format::wgsl);
}

TEST("slib sgl compiler - the flattened source is the emitted text, with its final addresses")
{
    auto const compiler = slib::create_sgl_compiler(slib::create_wgsl_compiler());
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };

    auto const text = compiler->preprocess({.source = "@inline binding constants:\n"
                                                      "    view_projection: mat4\n"
                                                      "@vertex struct cube_vertex:\n"
                                                      "    position: pos3\n"
                                                      "struct pixel_input:\n"
                                                      "    @position position: hpos4\n"
                                                      "@vertex fun main_vs(v: cube_vertex){constants} -> pixel_input:\n"
                                                      "    return {\n"
                                                      "        position = constants.view_projection * v.position\n"
                                                      "    }\n",
                                            .entry_point = "main_vs",
                                            .stage = sg::shader_stage::vertex},
                                           resolve);
    REQUIRE(text.has_value());
    CHECK(text.value().source.contains("@group(3) @binding(0) var<uniform> constants: constants_data;"));
    CHECK(text.value().source.contains("fn main_vs(v: cube_vertex) -> pixel_input"));
}

TEST("slib sgl compiler - the cube becomes WGSL that slib's own reader reflects", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_package(slib_test::sgl_shaders::package());

    auto const& vs = value_of(slib_test::sgl_shaders::cube.vertex.main_vs->acquire(sg::shader_format::wgsl));
    auto const& ps = value_of(slib_test::sgl_shaders::cube.pixel.main_ps->acquire(sg::shader_format::wgsl));

    CHECK(vs.stage == sg::shader_stage::vertex);
    CHECK(vs.format == sg::shader_format::wgsl);
    CHECK(vs.entry_point == "main_vs");
    CHECK(ps.stage == sg::shader_stage::fragment); // `pixel` is the package's word, and sg has one stage for it
    CHECK(ps.entry_point == "main_ps");

    // Group 3, binding 0 reads as the inline-constants block: a uniform buffer in no group.
    REQUIRE(vs.bindings.size() == 1);
    CHECK(vs.bindings[0].name == "constants");
    CHECK(vs.bindings[0].type == sg::binding_type::uniform_buffer);
    CHECK(!vs.bindings[0].group_index.has_value());
    CHECK(ps.bindings.empty());

    // The bytecode of a WGSL shader is its text, so this is what WebGPU compiles.
    auto const text = cc::string_view(reinterpret_cast<char const*>(vs.bytecode.data()), vs.bytecode.size());
    CHECK(text.contains("@location(2) color: vec3f"));
}

TEST("slib sgl compiler - a compute entry point reaches WGSL with its buffer reflected", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_package(slib_test::sgl_shaders::package());

    auto const& cs = value_of(slib_test::sgl_shaders::double_values.compute.main->acquire(sg::shader_format::wgsl));

    CHECK(cs.stage == sg::shader_stage::compute);
    CHECK(cs.entry_point == "main");
    REQUIRE(cs.workgroup_size.has_value());
    CHECK(cs.workgroup_size.value().x == 64);

    // The buffer is a group of its own, unlike the inline constants, so it carries a group index.
    REQUIRE(cs.bindings.size() == 1);
    CHECK(cs.bindings[0].type == sg::binding_type::readwrite_structured_buffer);
    REQUIRE(cs.bindings[0].group_index.has_value());
    CHECK(cs.bindings[0].group_index.value() == 0);

    auto const text = cc::string_view(reinterpret_cast<char const*>(cs.bytecode.data()), cs.bytecode.size());
    CHECK(text.contains("@compute @workgroup_size(64, 1, 1)"));
}

#if SLIB_HAS_DXC

ASYNC_TEST("slib sgl compiler - the cube becomes SPIR-V with a push-constant block", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_package(slib_test::sgl_shaders::package());

    auto const vs_node = slib_test::sgl_shaders::cube.vertex.main_vs->acquire(sg::shader_format::spirv);
    auto const ps_node = slib_test::sgl_shaders::cube.pixel.main_ps->acquire(sg::shader_format::spirv);
    co_await cc::async_settled(vs_node);
    co_await cc::async_settled(ps_node);
    auto const& vs = value_of(vs_node);
    auto const& ps = value_of(ps_node);
    CHECK(vs.bytecode.size() > 0);
    CHECK(ps.bytecode.size() > 0);
    CHECK(ps.stage == sg::shader_stage::fragment);

    auto const* constants = find_binding(vs, "constants");
    REQUIRE(constants != nullptr);
    CHECK(constants->type == sg::binding_type::uniform_buffer);
    CHECK(!constants->group_index.has_value());
    CHECK(!constants->space.has_value());
    REQUIRE(constants->block_size.has_value());
    CHECK(constants->block_size.value() == 64);
    CHECK(ps.bindings.empty());
}

#ifdef CC_OS_WINDOWS
ASYNC_TEST("slib sgl compiler - the cube becomes DXIL with its block at b0 of the inline-constants space",
           exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_package(slib_test::sgl_shaders::package());

    auto const vs_node = slib_test::sgl_shaders::cube.vertex.main_vs->acquire(sg::shader_format::dxil);
    auto const ps_node = slib_test::sgl_shaders::cube.pixel.main_ps->acquire(sg::shader_format::dxil);
    co_await cc::async_settled(vs_node);
    co_await cc::async_settled(ps_node);
    auto const& vs = value_of(vs_node);
    auto const& ps = value_of(ps_node);
    CHECK(vs.bytecode.size() > 0);
    CHECK(ps.bytecode.size() > 0);

    auto const* constants = find_binding(vs, "constants");
    REQUIRE(constants != nullptr);
    CHECK(constants->type == sg::binding_type::uniform_buffer);
    CHECK(constants->index == 0);
    REQUIRE(constants->space.has_value());
    CHECK(constants->space.value() == slib::inline_constants_space);
    REQUIRE(constants->block_size.has_value());
    CHECK(constants->block_size.value() == 64);
    CHECK(ps.bindings.empty());
}
#endif

#endif

namespace
{
/// What SGL's legalizer writes where the source has helpers and exits: result variables, flags, `once`, moved conditions.
/// Each construct is one a target's own flow analysis could refuse, which is what this source is for.
constexpr auto k_control_flow_source
    = cc::string_view("@pixel struct target:\n"
                      "    color: float4\n"
                      "\n"
                      "struct pixel_input:\n"
                      "    @position position: hpos4\n"
                      "    uv: float3\n"
                      "\n"
                      "fun halve(x: float) -> float => x * 0.5\n"
                      "\n"
                      "// a return from inside two loops\n"
                      "fun search(limit: float) -> float:\n"
                      "    let mut total = 0.0\n"
                      "    for i in 0 ..< 4:\n"
                      "        for j in 0 ..< 4:\n"
                      "            total += 0.125\n"
                      "            if total > limit => return total\n"
                      "    return 0.0\n"
                      "\n"
                      "// a loop nothing breaks out of: every exit is a return\n"
                      "fun settle(start: float) -> float:\n"
                      "    let mut x = start\n"
                      "    loop:\n"
                      "        x = halve x\n"
                      "        if x < 0.25 => return x\n"
                      "\n"
                      "@pixel fun main_ps(p: pixel_input) -> target:\n"
                      "    let a = search p.uv.x\n"
                      "    let mut b = p.uv.y\n"
                      "    while b > 0.0 and settle(b) > 0.125:\n"
                      "        b -= 0.25\n"
                      "        if b > 8.0 => continue\n"
                      "        b -= 0.25\n"
                      "    let c = loop:\n"
                      "        b += 0.5\n"
                      "        if b > 2.0 or settle(b) < 0.2 => break b\n"
                      "    if 0.5 < a <= settle(c):\n"
                      "        return {\n"
                      "            color = float4(a, b, c, 1.0)\n"
                      "        }\n"
                      "    // the entry point itself ends in a loop that every exit of is a return\n"
                      "    let mut d = c\n"
                      "    loop:\n"
                      "        d = halve d\n"
                      "        if d < 0.125:\n"
                      "            return {\n"
                      "                color = float4(a, b, d, 1.0)\n"
                      "            }\n");
} // namespace

ASYNC_TEST("slib sgl compiler - the control flow the legalizer writes is accepted by every compiler behind an edge",
           exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);

    for (auto const format : lib.supported_formats(slib::shader_language::sgl))
    {
        auto const node = lib.compile_source(k_control_flow_source, sg::shader_stage::fragment, "main_ps", format,
                                             {.language = slib::shader_language::sgl, .label = "control-flow.sgl"});
        co_await cc::async_settled(node);
        CHECK(value_of(node).bytecode.size() > 0);
    }
}

TEST("slib sgl compiler - a broken source fails with the place, the kind and the name", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);

    for (auto const format : lib.supported_formats(slib::shader_language::sgl))
    {
        auto const e = error_of(lib.compile_source(k_broken_source, sg::shader_stage::fragment, "main_ps", format,
                                                   {.language = slib::shader_language::sgl, .label = "broken.sgl"}));
        CHECK(e.contains("preprocessing 'broken.sgl' failed"));
        CHECK(e.contains("broken.sgl:9:24: error: unknown-name: missing"));
    }
}

TEST("slib sgl compiler - an entry point of the wrong stage, and a stage SGL does not have, are errors",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    auto const options = slib::compile_source_options{.language = slib::shader_language::sgl, .label = "flat.sgl"};
    constexpr auto source = cc::string_view("@pixel struct target:\n"
                                            "    color: float4\n"
                                            "\n"
                                            "struct pixel_input:\n"
                                            "    @position position: hpos4\n"
                                            "\n"
                                            "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                            "    return {\n"
                                            "        color = float4(1.0, 0.0, 0.0, 1.0)\n"
                                            "    }\n");

    auto const wgsl = sg::shader_format::wgsl;
    CHECK(value_of(lib.compile_source(source, sg::shader_stage::fragment, "main_ps", wgsl, options)).entry_point
          == "main_ps");

    auto const wrong_stage = error_of(lib.compile_source(source, sg::shader_stage::vertex, "main_ps", wgsl, options));
    CHECK(wrong_stage.contains("flat.sgl: error: entry point 'main_ps' is a pixel entry point"));

    auto const missing = error_of(lib.compile_source(source, sg::shader_stage::fragment, "main_fs", wgsl, options));
    CHECK(missing.contains("no entry point named 'main_fs' (the source holds: pixel 'main_ps')"));

    // Compute is a stage SGL has now, so asking for it here is the wrong stage rather than an unknown one.
    auto const wrong_kind = error_of(lib.compile_source(source, sg::shader_stage::compute, "main_ps", wgsl, options));
    CHECK(wrong_kind.contains("entry point 'main_ps' is a pixel entry point"));

    // A stage SGL still has none of.
    auto const no_stage = error_of(lib.compile_source(source, sg::shader_stage::geometry, "main_ps", wgsl, options));
    CHECK(no_stage.contains("SGL has vertex, pixel and compute entry points only"));
}
