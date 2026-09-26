#include "../emit/emit-test-support.hh"

#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::check::stage;

// `describe` is what a build generates host C++ from, so what it says has to be exactly what the emitter builds.
// Everything below is therefore pinned against a shader the emitter already compiles, or against one it refuses.

namespace
{
sgl::module_description described(cc::string_view source)
{
    auto const r = sgl::describe({.source = source, .source_name = "t.sgl"});
    if (r.has_error())
        FAIL(r.error());
    return r.value();
}

cc::string error_of(cc::string_view source)
{
    auto const r = sgl::describe({.source = source, .source_name = "t.sgl"});
    REQUIRE(r.has_error());
    return r.error();
}
} // namespace

TEST("sgl describe - the cube: an inline block, a vertex input, a target set and two entry points")
{
    auto const d = described(read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));

    REQUIRE(d.bindings.size() == 1);
    auto const& constants = d.bindings[0];
    CHECK(constants.name == "constants");
    CHECK(constants.is_inline);
    CHECK(constants.block_size == 64);
    REQUIRE(constants.members.size() == 1);
    CHECK(constants.members[0].kind == sgl::described_member_kind::constant);
    CHECK(constants.members[0].type == "mat4");
    CHECK(constants.members[0].offset == 0);
    CHECK(constants.members[0].size == 64);

    REQUIRE(d.structs.size() == 2);
    CHECK(d.structs[0].name == "cube_vertex");
    CHECK(d.structs[0].edge == stage::vertex);
    REQUIRE(d.structs[0].members.size() == 3);
    // The SGL spelling, not a host one: three different types that are all three `vec3f` to sg.
    CHECK(d.structs[0].members[0].type == "pos3");
    CHECK(d.structs[0].members[1].type == "vec3");
    CHECK(d.structs[0].members[2].type == "float3");
    CHECK(d.structs[0].members[2].location == 2);
    CHECK(d.structs[1].name == "target");
    CHECK(d.structs[1].edge == stage::pixel);

    REQUIRE(d.entry_points.size() == 2);
    CHECK(d.entry_points[0].name == "main_vs");
    CHECK(d.entry_points[0].stage == stage::vertex);
    REQUIRE(d.entry_points[0].bindings.size() == 1);
    CHECK(d.entry_points[0].bindings[0] == "constants");
    CHECK(d.entry_points[1].stage == stage::pixel);
    CHECK(d.entry_points[1].bindings.empty());
}

TEST("sgl describe - a buffer group numbers its buffers and names each by its path")
{
    auto const d = described(R"(binding work:
    src: buffer[float]
    dst: mut buffer[float]

@compute(64, 2) fun main(@thread_id id: int3){work}:
    work.dst[id.x] = work.src[id.x]
)");

    REQUIRE(d.bindings.size() == 1);
    auto const& work = d.bindings[0];
    CHECK(!work.is_inline);
    REQUIRE(work.members.size() == 2);
    CHECK(work.members[0].kind == sgl::described_member_kind::buffer);
    CHECK(work.members[0].type == "float");
    CHECK(work.members[0].access == "read");
    CHECK(work.members[0].slot == 0);
    CHECK(work.members[0].host_name == "work.src");
    CHECK(work.members[1].access == "read_write");
    CHECK(work.members[1].slot == 1);
    CHECK(work.members[1].host_name == "work.dst");

    REQUIRE(d.entry_points.size() == 1);
    CHECK(d.entry_points[0].stage == stage::compute);
    CHECK(d.entry_points[0].workgroup[0] == 64);
    CHECK(d.entry_points[0].workgroup[1] == 2);
    CHECK(d.entry_points[0].workgroup[2] == 1);
}

TEST("sgl describe - a group no entry point lists is still described, and still judged")
{
    // A shader file is a library, so the host may bind a group this file's own entry points never name.
    auto const d = described(R"(binding spare:
    values: buffer[float]
)");
    REQUIRE(d.bindings.size() == 1);
    CHECK(d.bindings[0].name == "spare");
    CHECK(d.entry_points.empty());

    // And an unlisted group the emitter could not write is refused as if something listed it.
    auto const error = error_of(R"(binding spare:
    lit: bool
)");
    CHECK(error.contains("unsupported"));
    CHECK(error.contains("spare.lit"));
}

TEST("sgl describe - what the emitter refuses is refused here, in the emitter's words")
{
    // An @inline binding that does not stand last would move every group behind it under the host.
    auto const error = error_of(R"(@inline binding c:
    scale: float

binding work:
    values: mut buffer[float]

@compute(64) fun main(@thread_id id: int3){c, work}:
    work.values[id.x] = c.scale
)");
    CHECK(error.contains("not the last of the list"));
}

TEST("sgl describe - a source with errors describes nothing, and says why")
{
    auto const error = error_of("fun f() -> float => nope\n");
    CHECK(error.contains("t.sgl:1:"));
    CHECK(error.contains("unknown-name"));
}

TEST("sgl describe - a group's plain members are its constant buffer, at slot 0 and under the binding's name")
{
    auto const d = described(R"(binding affine:
    scale: float
    bias: float
    values: mut buffer[float]

@compute(64) fun main(@thread_id id: int3){affine}:
    affine.values[id.x] = affine.values[id.x] * affine.scale + affine.bias
)");
    REQUIRE(d.bindings.size() == 1);
    auto const& affine = d.bindings[0];
    CHECK(affine.block_slot == 0);
    CHECK(affine.block_host_name == "affine");
    CHECK(affine.block_size == 8);
    REQUIRE(affine.members.size() == 3);
    CHECK(affine.members[1].kind == sgl::described_member_kind::constant);
    CHECK(affine.members[1].offset == 4);
    // The buffers follow the block.
    CHECK(affine.members[2].slot == 1);
}

TEST("sgl describe - a vertex input's members say which buffer they come from, and how it steps")
{
    auto const d = described(read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));
    // Unmarked: one buffer, stepped per vertex.
    CHECK(d.structs[0].members[0].stream == "per_vertex");
    CHECK(!d.structs[0].members[0].is_per_instance);
    // A render target struct has no streams.
    CHECK(d.structs[1].members[0].stream.empty());

    auto const edges = cc::string(R"(struct pixel_input:
    @position position: hpos4

@pixel struct target:
    color: float4

@pixel fun main_ps(p: pixel_input) -> target:
    return {color = float4(1.0, 1.0, 1.0, 1.0)}
)");
    auto const split = described(cc::string(R"(@vertex struct mesh:
    position: pos3
    @per_instance offset: vec3
    @stream(normals) normal: vec3

)") + edges);
    REQUIRE(split.structs.size() == 2);
    auto const& mesh = split.structs[0];
    CHECK(mesh.members[0].stream == "per_vertex");
    CHECK(mesh.members[1].stream == "per_instance");
    CHECK(mesh.members[1].is_per_instance);
    CHECK(mesh.members[2].stream == "normals");
    CHECK(mesh.members[2].location == 2); // a stream moves no location

    // Where a stream means nothing, it is refused rather than ignored.
    CHECK(error_of(R"(@vertex struct v:
    position: pos3

struct pixel_input:
    @position position: hpos4

@pixel struct target:
    @per_instance color: float4

@pixel fun main_ps(p: pixel_input) -> target:
    return {color = float4(1.0, 1.0, 1.0, 1.0)}
)")
              .contains("@per_instance or @stream in a render target struct"));
    CHECK(error_of(cc::string(R"(@vertex struct mixed:
    @stream(a) x: vec3
    @per_instance @stream(a) y: vec3

)") + edges)
              .contains("mixes per-vertex and per-instance members"));
    CHECK(error_of(cc::string(R"(@vertex struct bad:
    @stream(1) x: vec3

)") + edges)
              .contains("@stream takes one name"));
}

TEST("sgl describe - a pipeline: its stages, its layout, its settings in order, and what the host states")
{
    auto const d = described(read_text(cc::string(SGL_SAMPLES_DIR) + "/pipeline.sgl"));

    REQUIRE(d.pipelines.size() == 1);
    auto const& p = d.pipelines[0];
    CHECK(p.name == "pipeline");
    CHECK(p.vertex == "main_vs");
    CHECK(p.pixel == "main_ps");
    // `constants` is @inline, so it is no group of the layout.
    CHECK(p.layout.empty());
    CHECK(p.inline_constants == "constants");
    CHECK(p.vertex_input == "cube_vertex");
    CHECK(p.target_set == "target");
    REQUIRE(p.targets.size() == 1);
    CHECK(p.targets[0] == "color");

    // The @pixel struct's attribute first, then the declaration's lines.
    REQUIRE(p.settings.size() == 5);
    CHECK(p.settings[0].path == "color_targets.color.format");
    CHECK(p.settings[0].kind == sgl::check::setting_kind::host);
    CHECK(p.settings[1].path == "rasterization.cull");
    CHECK(p.settings[1].enum_case == "back");
    CHECK(p.settings[2].path == "depth_stencil.depth_test");
    CHECK(p.settings[2].integer == 1);
    CHECK(p.settings[4].path == "depth_stencil_format");
    CHECK(p.settings[4].enum_case == "depth32_float");

    REQUIRE(p.open.size() == 1);
    CHECK(p.open[0] == "color_targets.color.format");
}

TEST("sgl describe - a later setting takes a part back from the host")
{
    auto const d = described(read_text(cc::string(SGL_SAMPLES_DIR) + "/pipeline.sgl")
                             + "pipeline fixed:\n"
                               "    vertex = main_vs\n    pixel = main_ps\n    format = .bgra8_unorm\n");
    REQUIRE(d.pipelines.size() == 2);
    CHECK(d.pipelines[1].name == "fixed");
    CHECK(d.pipelines[1].open.empty());
}

TEST("sgl describe - a struct's shape is its members, and not its name")
{
    // A pipeline's host code is built against these shapes, so a hot reload compares them.
    auto const shape_of = [](cc::string_view vertex_struct)
    {
        auto const d = described(cc::string(vertex_struct)
                                 + "struct link:\n    @position p: hpos4\n"
                                   "@vertex fun vs(v: vin) -> link:\n    return { p = hpos4(..v.p, 1.0) }\n");
        REQUIRE(d.structs.size() == 1);
        return d.structs[0].shape;
    };
    auto const base = shape_of("@vertex struct vin:\n    p: pos3\n");
    CHECK(base.size() == 32);

    // Only the same file described again gives the same shape.
    CHECK(shape_of("@vertex struct vin:\n    p: pos3\n") == base);
    // A member added, renamed or retyped is a new shape, which is what a reload must not miss.
    CHECK(shape_of("@vertex struct vin:\n    p: pos3\n    q: float4\n") != base);
    CHECK(shape_of("@vertex struct vin:\n    p: pos3\n    q: float4\n")
          != shape_of("@vertex struct vin:\n    p: pos3\n    r: float4\n"));
    CHECK(shape_of("@vertex struct vin:\n    p: float3\n") != base);
    // An attribute the host lays buffers out by is part of it.
    CHECK(shape_of("@vertex struct vin:\n    @per_instance p: pos3\n") != base);

    // Bindings have one too, over their members.
    auto const binding_shape = [](cc::string_view member)
    {
        auto const d
            = described(cc::string("@inline binding constants:\n") + member
                        + "@vertex struct vin:\n    p: pos3\n"
                          "struct link:\n    @position p: hpos4\n"
                          "@vertex fun vs(v: vin){constants} -> link:\n    return { p = hpos4(..v.p, 1.0) }\n");
        REQUIRE(d.bindings.size() == 1);
        return d.bindings[0].shape;
    };
    CHECK(binding_shape("    scale: float\n") == binding_shape("    scale: float\n"));
    CHECK(binding_shape("    scale: float\n") != binding_shape("    scale: float\n    bias: float\n"));
}

TEST("sgl describe - a group's shape holds every fact of its textures, images and samplers")
{
    // A reload that missed one of these would keep a layout the new shader no longer matches.
    auto const shape_of
        = [](cc::string_view name, cc::string_view t, cc::string_view i, cc::string_view s, cc::string_view filter)
    {
        auto const d = described(cc::format(
            "binding {}:\n    {}\n    {}\n    {}\n    sampler st:\n        filter = .{}\n", name, t, i, s, filter));
        REQUIRE(d.bindings.size() == 1);
        return d.bindings[0].shape;
    };
    auto const t = "t: texture_2d[float4]";
    auto const i = "i: out image_2d[.r32_float]";
    auto const s = "s: sampler";
    auto const base = shape_of("set", t, i, s, "linear");
    CHECK(base.size() == 32);
    CHECK(shape_of("set", t, i, s, "linear") == base);
    // The binding's name is no part of it, as a struct's is not.
    CHECK(shape_of("other", t, i, s, "linear") == base);

    CHECK(shape_of("set", "t: texture_2d[float2]", i, s, "linear") != base);
    CHECK(shape_of("set", "t: texture_2d_array[float4]", i, s, "linear") != base);
    CHECK(shape_of("set", "t: texture_2d_depth", i, s, "linear") != base);
    CHECK(shape_of("set", "@unfilterable t: texture_2d[float4]", i, s, "linear") != base);
    CHECK(shape_of("set", t, "i: out image_2d[.rgba8_unorm]", s, "linear") != base);
    CHECK(shape_of("set", t, "i: mut image_2d[.r32_float]", s, "linear") != base);
    CHECK(shape_of("set", t, i, "s: comparison_sampler", "linear") != base);
    CHECK(shape_of("set", t, i, "@non_filtering s: sampler", "linear") != base);
    CHECK(shape_of("set", t, i, s, "nearest") != base);
}

TEST("sgl describe - a texture's sample type and a sampler's binding type, as the declaration states them")
{
    auto const d = described(R"(binding set:
    f: texture_2d[float4]
    u: texture_2d[uint4]
    n: texture_2d[int]
    z: texture_2d_depth
    @unfilterable r: texture_2d[float4]
    ms: texture_2d_ms[float4]
    strip: texture_1d[float]
    bound: sampler
    compares: comparison_sampler
    sampler crisp:
        filter = .nearest
    sampler shadow:
        compare = .less
)");
    REQUIRE(d.bindings.size() == 1);
    auto const& set = d.bindings[0];
    REQUIRE(set.members.size() == 11);
    CHECK(set.members[0].sample_type == "filterable_float");
    CHECK(set.members[1].sample_type == "uint");
    CHECK(set.members[2].sample_type == "sint");
    CHECK(set.members[3].sample_type == "depth");
    CHECK(set.members[4].sample_type == "unfilterable_float");
    CHECK(set.members[5].sample_type == "unfilterable_float");
    CHECK(set.members[5].texture_dimension == "tex_2d_ms");
    // WGSL writes a 1D texture as a 2D one, and the host still creates a 1D one: sg's backend makes it 2D.
    CHECK(set.members[6].texture_dimension == "tex_1d");
    CHECK(set.members[7].sampler_type == "filtering");
    CHECK(set.members[8].sampler_type == "comparison");
    CHECK(set.members[9].sampler_type == "non_filtering");
    CHECK(set.members[9].static_sampler.has_value());
    CHECK(set.members[10].sampler_type == "comparison");
    REQUIRE(set.members[10].static_sampler.has_value());
    CHECK(set.members[10].static_sampler.value().compare == "less");

    // No plain member, so no constant block: the resources number from slot 0.
    CHECK(set.block_slot == -1);
    CHECK(set.members[0].slot == 0);
    CHECK(set.members[10].slot == 10);
    CHECK(set.members[0].host_name == "set.f");
}

TEST("sgl describe - an image store is refused in a vertex stage, which core WebGPU gives no writable storage")
{
    auto const error = error_of(R"(binding tex:
    dst: out image_2d[.rgba8_unorm]

@vertex struct vin:
    p: pos3

struct link:
    @position p: hpos4

@vertex fun vs(v: vin){tex} -> link:
    DEBUG_store(tex.dst, int2(0, 0), float4(1.0, 1.0, 1.0, 1.0))
    return { p = hpos4(..v.p, 1.0) }
)");
    CHECK(error.contains("stage-not-allowed"));
    CHECK(error.contains("DEBUG_store is @stages without it"));
}
