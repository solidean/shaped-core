#include "fake_compiler.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>                          // sg::max_binding_groups
#include <shaped-shader-library/binding/binding_groups.hh> // slib::inline_constants_space, slib::rewrite_binding_groups
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

    auto const& vs = value_of(slib_test::sgl_shaders::cube.main_vs->acquire(sg::shader_format::wgsl));
    auto const& ps = value_of(slib_test::sgl_shaders::cube.main_ps->acquire(sg::shader_format::wgsl));

    CHECK(vs.stage == sg::shader_stage::vertex);
    CHECK(vs.format == sg::shader_format::wgsl);
    CHECK(vs.entry_point == "main_vs");
    CHECK(ps.stage == sg::shader_stage::fragment); // `pixel` is the package's word, and sg has one stage for it
    CHECK(ps.entry_point == "main_ps");

    // Group 3, binding 0 reads as the inline-constants block: a constants buffer in no group.
    REQUIRE(vs.bindings.size() == 1);
    CHECK(vs.bindings[0].name == "constants");
    CHECK(vs.bindings[0].type == sg::binding_type::constants_buffer);
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

    auto const& cs = value_of(slib_test::sgl_shaders::double_values.main->acquire(sg::shader_format::wgsl));

    CHECK(cs.stage == sg::shader_stage::compute);
    CHECK(cs.entry_point == "main");
    REQUIRE(cs.workgroup_size.has_value());
    CHECK(cs.workgroup_size.value().x == 64);

    // The buffer is a group of its own, unlike the inline constants, so it carries a group index.
    REQUIRE(cs.bindings.size() == 1);
    CHECK(cs.bindings[0].type == sg::binding_type::buffer);
    CHECK(cs.bindings[0].access == sg::access_mode::read_write);
    REQUIRE(cs.bindings[0].group_index.has_value());
    CHECK(cs.bindings[0].group_index.value() == 0);

    auto const text = cc::string_view(reinterpret_cast<char const*>(cs.bytecode.data()), cs.bytecode.size());
    CHECK(text.contains("@compute @workgroup_size(64, 1, 1)"));
}

TEST("slib sgl compiler - an SGL shader states the features it needs, and a WGSL one cannot say",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_compiler(slib::create_wgsl_compiler());
    auto const wgsl = sg::shader_format::wgsl;

    // One entry point uses what its file requires and the other does not, so each states exactly its own.
    constexpr auto source = cc::string_view("require extended_image_formats\n"
                                            "\n"
                                            "binding narrow:\n"
                                            "    r: out image_2d[.r8_unorm]\n"
                                            "\n"
                                            "binding plain:\n"
                                            "    values: mut buffer[float]\n"
                                            "\n"
                                            "@compute(8, 8) fun narrow_cs(@thread_id id: int3){narrow}:\n"
                                            "    narrow.r.store(int2(id.x, id.y), 0.5)\n"
                                            "\n"
                                            "@compute(64) fun plain_cs(@thread_id id: int3){plain}:\n"
                                            "    plain.values[id.x] = 1.0\n");
    auto const options = slib::compile_source_options{.language = slib::shader_language::sgl, .label = "narrow.sgl"};
    auto const narrow_node = lib.compile_source(source, sg::shader_stage::compute, "narrow_cs", wgsl, options);
    CHECK(value_of(narrow_node).required_features == cc::optional<sg::feature_set>(sg::feature::extended_image_formats));
    auto const plain_node = lib.compile_source(source, sg::shader_stage::compute, "plain_cs", wgsl, options);
    auto const& plain = value_of(plain_node);
    CHECK(plain.required_features == cc::optional<sg::feature_set>(sg::feature_set()));

    // The same text handed in as WGSL has lost what SGL knew: unknown, and never mistaken for portable.
    auto const text
        = cc::string(cc::string_view(reinterpret_cast<char const*>(plain.bytecode.data()), plain.bytecode.size()));
    auto const raw_node = lib.compile_source(text, sg::shader_stage::compute, "plain_cs", wgsl,
                                             {.language = slib::shader_language::wgsl, .label = "plain.wgsl"});
    CHECK(!value_of(raw_node).required_features.has_value());
}

#if SLIB_HAS_DXC

ASYNC_TEST("slib sgl compiler - the cube becomes SPIR-V with a push-constant block", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);
    lib.add_package(slib_test::sgl_shaders::package());

    auto const vs_node = slib_test::sgl_shaders::cube.main_vs->acquire(sg::shader_format::spirv);
    auto const ps_node = slib_test::sgl_shaders::cube.main_ps->acquire(sg::shader_format::spirv);
    co_await cc::async_settled(vs_node);
    co_await cc::async_settled(ps_node);
    auto const& vs = value_of(vs_node);
    auto const& ps = value_of(ps_node);
    CHECK(vs.bytecode.size() > 0);
    CHECK(ps.bytecode.size() > 0);
    CHECK(ps.stage == sg::shader_stage::fragment);

    auto const* constants = find_binding(vs, "constants");
    REQUIRE(constants != nullptr);
    CHECK(constants->type == sg::binding_type::constants_buffer);
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

    auto const vs_node = slib_test::sgl_shaders::cube.main_vs->acquire(sg::shader_format::dxil);
    auto const ps_node = slib_test::sgl_shaders::cube.main_ps->acquire(sg::shader_format::dxil);
    co_await cc::async_settled(vs_node);
    co_await cc::async_settled(ps_node);
    auto const& vs = value_of(vs_node);
    auto const& ps = value_of(ps_node);
    CHECK(vs.bytecode.size() > 0);
    CHECK(ps.bytecode.size() > 0);

    auto const* constants = find_binding(vs, "constants");
    REQUIRE(constants != nullptr);
    CHECK(constants->type == sg::binding_type::constants_buffer);
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

namespace
{
/// Two groups, and in the second one resource of every kind, so a register class, a slot or a space off by one shows.
constexpr auto k_two_groups_source
    = cc::string_view("binding frame:\n"
                      "    values: buffer[float]\n"
                      "\n"
                      "binding post:\n"
                      "    texel_size: float2\n"
                      "    src: texture_2d[float4]\n"
                      "    dst: out image_2d[.rgba8_unorm]\n"
                      "    acc: mut image_2d[.r32_float]\n"
                      "    sampler bilinear:\n"
                      "        filter = .linear\n"
                      "        address = .clamp_edge\n"
                      "\n"
                      "@compute(8, 8) fun blur(@thread_id id: int3){frame, post}:\n"
                      "    let xy = int2(id.x, id.y)\n"
                      "    let uv = ((xy as float2) + float2(0.5, 0.5)) * post.texel_size * frame.values[0]\n"
                      "    let c = post.src.sample(uv, post.bilinear, level = 0.0)\n"
                      "    post.dst.store(xy, c)\n"
                      "    post.acc.store(xy, post.acc.load(xy) + c.x)\n");
} // namespace

ASYNC_TEST("slib sgl compiler - every compiler behind an edge reflects the group and slot SGL wrote",
           exclusive("slib-shader-library"))
{
    // `name group index` per binding, which is what a layout built from `sgl describe` assumes on every backend.
    constexpr auto expected = cc::string_view("frame.values 0 0\n"
                                              "post 1 0\n"
                                              "post.src 1 1\n"
                                              "post.dst 1 2\n"
                                              "post.acc 1 3\n"
                                              "post.bilinear 1 4\n");

    slib::shader_library lib;
    add_sgl_compilers(lib);

    for (auto const format : lib.supported_formats(slib::shader_language::sgl))
    {
        auto const node = lib.compile_source(k_two_groups_source, sg::shader_stage::compute, "blur", format,
                                             {.language = slib::shader_language::sgl, .label = "two-groups.sgl"});
        co_await cc::async_settled(node);
        auto const& cs = value_of(node);
        auto addresses = cc::string();
        for (auto const name : {"frame.values", "post", "post.src", "post.dst", "post.acc", "post.bilinear"})
        {
            auto const* b = find_binding(cs, name);
            REQUIRE(b != nullptr);
            // dx12 reads the group as the register space, vulkan and WebGPU as the set.
            auto const group = format == sg::shader_format::dxil ? b->space : b->group_index;
            REQUIRE(group.has_value());
            addresses.appendf("{} {} {}\n", name, group.value(), b->index);
        }
        CHECK(addresses == expected);
        CHECK(cs.bindings.size() == 6);
    }
}

namespace
{
/// A WGSL compiler that reads the text wrong: its reflection moves `post.src` to another slot.
class misreading_compiler final : public slib::shader_compiler
{
public:
    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::wgsl; }
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::wgsl; }
    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver) const override
    {
        return slib::preprocessed_source{.source = desc.source};
    }
    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        auto shader = sg::compiled_shader{
            .stage = desc.stage,
            .format = sg::shader_format::wgsl,
            .entry_point = desc.entry_point,
            .bytecode = cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(desc.source.as_bytes()))};
        shader.bindings.push_back({.name = "post_src",
                                   .group_index = 1u,
                                   .index = 7u,
                                   .type = sg::binding_type::texture,
                                   .texture_dimension = sg::texture_view_dimension::tex_2d});
        shader.workgroup_size = sg::compute_dimensions{.x = 1, .y = 1, .z = 1};
        shader.compiler = {.name = "misreading", .version = "1", .signature = "inner"};
        return cc::make_async_from_value(cc::move(shader));
    }
};
} // namespace

TEST("slib sgl compiler - the compiled shader is SGL's interface, and the compile adds only its bytecode",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    // Reflects nothing at all, so everything the shader states below came from SGL.
    lib.add_compiler(slib::create_sgl_compiler(
        std::make_unique<slib_test::fake_compiler>(slib::shader_language::wgsl, sg::shader_format::wgsl)));

    auto const node = lib.compile_source(k_two_groups_source, sg::shader_stage::compute, "blur", sg::shader_format::wgsl,
                                         {.language = slib::shader_language::sgl, .label = "two-groups.sgl"});
    auto const& cs = value_of(node);
    CHECK(cs.entry_point == "blur");
    REQUIRE(cs.workgroup_size.has_value());
    CHECK(cs.workgroup_size.value().x == 8);
    CHECK(cs.workgroup_size.value().y == 8);
    CHECK(cs.footprint.source == sg::footprint_source::exact);
    CHECK(cs.compiler.name == "fake");
    CHECK(cs.compiler.signature.ends_with(" sgl"));
    CHECK(slib_test::fake_compiler::source_of(cs).contains("@compute @workgroup_size(8, 8, 1)"));

    REQUIRE(cs.bindings.size() == 6);
    auto const* src = find_binding(cs, "post.src");
    REQUIRE(src != nullptr);
    CHECK(src->reflected_name == "post_src");
    CHECK(src->group_index == cc::optional<u32>(1u));
    CHECK(src->index == 1);
    CHECK(src->type == sg::binding_type::texture);
    CHECK(src->texture_dimension == cc::optional<sg::texture_view_dimension>(sg::texture_view_dimension::tex_2d));
    CHECK(src->sample_type == cc::optional<sg::texture_sample_type>(sg::texture_sample_type::filterable_float));
    CHECK(src->visibility.has(sg::shader_stage::compute));

    auto const* dst = find_binding(cs, "post.dst");
    REQUIRE(dst != nullptr);
    CHECK(dst->access == sg::access_mode::write);
    CHECK(dst->image_format == cc::optional<sg::pixel_format>(sg::pixel_format::rgba8_unorm));

    auto const* block = find_binding(cs, "post");
    REQUIRE(block != nullptr);
    CHECK(block->type == sg::binding_type::constants_buffer);
    CHECK(block->block_size == cc::optional<isize>(16));

    auto const* bilinear = find_binding(cs, "post.bilinear");
    REQUIRE(bilinear != nullptr);
    CHECK(bilinear->sampler_type == cc::optional<sg::sampler_binding_type>(sg::sampler_binding_type::filtering));
}

TEST("slib sgl compiler - a reflection that disagrees with SGL is logged, and SGL's interface is used",
     exclusive("slib-shader-library"))
{
    nx::expect_error("disagrees with SGL", nx::exactly(1));

    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(std::make_unique<misreading_compiler>()));

    auto const node = lib.compile_source(k_two_groups_source, sg::shader_stage::compute, "blur", sg::shader_format::wgsl,
                                         {.language = slib::shader_language::sgl, .label = "two-groups.sgl"});
    auto const& cs = value_of(node);
    auto const* src = find_binding(cs, "post.src");
    REQUIRE(src != nullptr);
    CHECK(src->index == 1);
    CHECK(cs.workgroup_size.value().x == 8);
    CHECK(cs.compiler.name == "misreading");
}

TEST("slib sgl compiler - a declared slot the code never reaches is not listed, and reflecting it is no mismatch",
     exclusive("slib-shader-library"))
{
    // slib's WGSL reader reports every declaration, so `frame.unused` reflects; the log rule is the mismatch check.
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));

    constexpr auto source = cc::string_view("binding frame:\n"
                                            "    values: mut buffer[float]\n"
                                            "    unused: buffer[float]\n"
                                            "\n"
                                            "@compute(64) fun main(@thread_id id: int3){frame}:\n"
                                            "    frame.values[id.x] = 1.0\n");
    auto const node = lib.compile_source(source, sg::shader_stage::compute, "main", sg::shader_format::wgsl,
                                         {.language = slib::shader_language::sgl, .label = "unused.sgl"});
    auto const& cs = value_of(node);
    REQUIRE(cs.bindings.size() == 1);
    CHECK(cs.bindings[0].name == "frame.values");
    CHECK(cs.bindings[0].access == sg::access_mode::read_write);
    auto const text = cc::string_view(reinterpret_cast<char const*>(cs.bytecode.data()), cs.bytecode.size());
    CHECK(text.contains("frame_unused"));
}

TEST("slib sgl compiler - SGL's text never goes through slib's HLSL binding pass", exclusive("slib-shader-library"))
{
    // The pass reads `#pragma sc` groups and renumbers registers; SGL's text carries its final addresses instead.
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(
        std::make_unique<slib_test::fake_compiler>(slib::shader_language::hlsl, sg::shader_format::spirv)));
    auto const node = lib.compile_source(k_two_groups_source, sg::shader_stage::compute, "blur", sg::shader_format::spirv,
                                         {.language = slib::shader_language::sgl, .label = "two-groups.sgl"});
    auto const text = slib_test::fake_compiler::source_of(value_of(node));
    CHECK(!text.contains("#pragma sc"));
    CHECK(text.contains("[[vk::binding(1, 1)]]"));
}

namespace
{
/// Every layout a target has to be made to follow: a vector in a row's tail, a nested struct, a tight buffer element.
constexpr auto k_layout_sources = {
    cc::string_view("struct tint:\n"
                    "    color: float3\n"
                    "    strength: float\n"
                    "\n"
                    "struct look_t:\n"
                    "    t: tint\n"
                    "    after: float\n"
                    "    dir: float3\n"
                    "\n"
                    "binding frame:\n"
                    "    scale: float\n"
                    "    shift: float3\n"
                    "    l: look_t\n"
                    "    c: float2\n"
                    "    out_values: mut buffer[float]\n"
                    "\n"
                    "@compute(64) fun main(@thread_id id: int3){frame}:\n"
                    "    let l = frame.l\n"
                    "    frame.out_values[id.x] = frame.scale + frame.shift.y + l.t.color.z + l.dir.x + frame.c.y\n"),
    cc::string_view("struct particle:\n"
                    "    mass: float\n"
                    "    velocity: float3\n"
                    "    color: float4\n"
                    "    alive: bool32\n"
                    "\n"
                    "binding work:\n"
                    "    scale: float\n"
                    "    items: mut buffer[particle]\n"
                    "    dirs: mut buffer[float3]\n"
                    "\n"
                    "@compute(64) fun main(@thread_id id: int3){work}:\n"
                    "    let p = work.items[id.x]\n"
                    "    if p.alive as bool:\n"
                    "        work.dirs[id.x] = work.dirs[id.x] + p.velocity * work.scale\n"
                    "        work.items[id.x] = p\n"),
};
} // namespace

ASYNC_TEST("slib sgl compiler - every layout SGL makes a target follow is accepted by every compiler behind an edge",
           exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    add_sgl_compilers(lib);

    for (auto const source : k_layout_sources)
        for (auto const format : lib.supported_formats(slib::shader_language::sgl))
        {
            auto const node = lib.compile_source(source, sg::shader_stage::compute, "main", format,
                                                 {.language = slib::shader_language::sgl, .label = "layout.sgl"});
            co_await cc::async_settled(node);
            CHECK(value_of(node).bytecode.size() > 0);
        }
}

ASYNC_TEST("slib sgl compiler - every compiler that reads a layout finds each field where SGL states it",
           exclusive("slib-shader-library"))
{
    // The comparison on every compile skips what it cannot match, so this holds that it matches every field.
    auto edges = cc::vector<std::unique_ptr<slib::shader_compiler>>();
    edges.push_back(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
#if SLIB_HAS_DXC
    auto spirv = slib::create_dxc_spirv_compiler();
    REQUIRE(spirv.has_value());
    edges.push_back(slib::create_sgl_compiler(cc::move(spirv.value())));
#endif
    auto resolve = [](cc::string_view) -> cc::optional<cc::string> { return cc::nullopt; };

    for (auto const source : k_layout_sources)
        for (auto const& edge : edges)
        {
            auto desc = slib::shader_source_description{.source = cc::string(source),
                                                        .entry_point = "main",
                                                        .stage = sg::shader_stage::compute};
            auto preprocessed = edge->preprocess(desc, resolve);
            REQUIRE(preprocessed.has_value());
            desc.source = preprocessed.value().source;
            auto const node = edge->compile(desc);
            co_await cc::async_settled(node);
            auto const reflected = edge->reflect_layouts(value_of(node));
            REQUIRE(reflected.has_value());

            auto const& stated = preprocessed.value().layouts;
            CHECK(!stated.empty());
            for (auto const& s : stated)
            {
                slib::block_layout const* r = nullptr;
                for (auto const& candidate : reflected.value())
                    if (candidate.global == s.global)
                        r = &candidate;
                // A compiler may drop a block the code never reads; these sources read every one.
                REQUIRE(r != nullptr);
                CHECK(r->stride == s.stride);
                for (auto const& f : s.fields)
                {
                    auto found = false;
                    for (auto const& g : r->fields)
                        if (g.name == f.name)
                        {
                            found = true;
                            CHECK(g.offset == f.offset);
                        }
                    CHECK(found);
                }
            }
        }
}

namespace
{
/// A WGSL compiler whose reading of the text puts every field of every block 4 bytes late.
class late_layout_compiler final : public slib::shader_compiler
{
public:
    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::wgsl; }
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::wgsl; }
    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver resolve) const override
    {
        return _inner->preprocess(desc, resolve);
    }
    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        return _inner->compile(desc);
    }
    [[nodiscard]] cc::optional<cc::vector<slib::block_layout>> reflect_layouts(sg::compiled_shader const& shader) const override
    {
        auto layouts = _inner->reflect_layouts(shader);
        for (auto& l : layouts.value())
            for (auto& f : l.fields)
                f.offset += 4;
        return layouts;
    }

private:
    std::unique_ptr<slib::shader_compiler> _inner = slib::create_wgsl_compiler();
};
} // namespace

TEST("slib sgl compiler - a layout the compiler reads elsewhere than SGL states is logged",
     exclusive("slib-shader-library"))
{
    nx::expect_error("disagrees with SGL", nx::exactly(1));
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(std::make_unique<late_layout_compiler>()));
    auto const node
        = lib.compile_source(*k_layout_sources.begin(), sg::shader_stage::compute, "main", sg::shader_format::wgsl,
                             {.language = slib::shader_language::sgl, .label = "layout.sgl"});
    CHECK(value_of(node).bytecode.size() > 0);
}

namespace
{
/// What a `pending_compiler` reports, owned by the test so it outlives the compiler.
struct pending_state
{
    cc::shared_async<sg::compiled_shader> node;
    bool is_alive = true;
    bool was_read_alive = false;
    bool was_read = false;
};

/// A WGSL compiler whose compile settles only when the test pushes it, and that says whether it was alive when read.
class pending_compiler final : public slib::shader_compiler
{
public:
    explicit pending_compiler(pending_state& state) : _state(state) {}
    ~pending_compiler() override { _state.is_alive = false; }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::wgsl; }
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::wgsl; }
    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver) const override
    {
        return slib::preprocessed_source{.source = desc.source};
    }
    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const&) const override
    {
        _state.node = cc::make_async_manual<sg::compiled_shader>();
        return _state.node;
    }
    [[nodiscard]] cc::optional<cc::vector<slib::block_layout>> reflect_layouts(sg::compiled_shader const&) const override
    {
        _state.was_read = true;
        _state.was_read_alive = _state.is_alive;
        return {};
    }

private:
    pending_state& _state;
};
} // namespace

ASYNC_TEST("slib sgl compiler - a compile still settling keeps the compiler it is checked through, even once replaced",
           exclusive("slib-shader-library"))
{
    auto state = pending_state();
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(std::make_unique<pending_compiler>(state)));

    auto const node = lib.compile_source(k_two_groups_source, sg::shader_stage::compute, "blur", sg::shader_format::wgsl,
                                         {.language = slib::shader_language::sgl, .label = "two-groups.sgl"});
    REQUIRE(state.node != nullptr);

    // Replacing the edge drops the library's reference, and the pending check holds the only one left.
    lib.add_compiler(slib::create_sgl_compiler(
        std::make_unique<slib_test::fake_compiler>(slib::shader_language::wgsl, sg::shader_format::wgsl)));
    CHECK(state.is_alive);

    state.node->push_value(
        sg::compiled_shader{.stage = sg::shader_stage::compute, .format = sg::shader_format::wgsl, .entry_point = "blur"});
    co_await cc::async_settled(node);
    CHECK(state.was_read);
    CHECK(state.was_read_alive);
    CHECK(value_of(node).workgroup_size.has_value());
    // Once the check has run, nothing keeps the replaced compiler.
    CHECK(!state.is_alive);
}

// sgl links neither sg nor slib, so its emitter repeats what they own; these hold each copy to the original.
static_assert(sgl::emit::impl::k_max_groups == sg::max_binding_groups);

TEST("slib sgl compiler - SGL spells every image format for SPIR-V as slib's pass does")
{
    for (auto const& f : sgl::check::k_image_formats)
    {
        auto const hlsl = cc::format("#pragma sc group 0\n"
                                     "namespace g\n"
                                     "{{\n"
                                     "#pragma sc format {}\n"
                                     "    RWTexture2D<float4> image;\n"
                                     "}}\n",
                                     f.name);
        auto const rewritten = slib::rewrite_binding_groups(hlsl, sg::shader_format::spirv);
        REQUIRE(rewritten.has_value());
        auto const text = cc::string_view(rewritten.value());
        if (f.spirv.empty())
            CHECK(!text.contains("vk::image_format"));
        else
            CHECK(text.contains(cc::format("[[vk::image_format(\"{}\")]]", f.spirv)));
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
