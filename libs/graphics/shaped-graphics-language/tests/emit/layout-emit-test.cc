#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::emit::target;

// SGL places every value in GPU memory by one rule per address space, and each target is made to follow it:
// a constant block by HLSL's constant-buffer packing, a buffer's element by dx12's structured-buffer packing.
// See the spec's emitting file, "Layout".

namespace
{
constexpr cc::string_view k_edges = "@pixel struct frame:\n"
                                    "    color: float4\n"
                                    "\n"
                                    "struct pixel_input:\n"
                                    "    @position position: hpos4\n"
                                    "\n";

/// An `@inline` block of `members` and a pixel entry point that reads `reads` from it.
cc::string inline_block(cc::string_view members, cc::string_view reads)
{
    return cc::format("{}@inline binding look:\n{}\n@pixel fun main_ps(p: pixel_input){{look}} -> frame:\n"
                      "    return {{\n        color = float4({})\n    }}\n",
                      k_edges, members, reads);
}

cc::string text_of(cc::string_view user, target t)
{
    auto const e = emit_source(user, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

/// The errors of entry point 0, which must be the same for every target.
cc::string errors_of(cc::string_view user)
{
    auto const first = emit_source(user, 0, target::hlsl_dx12);
    CHECK(first.text == "");
    for (auto const t : sgl::emit::all_targets())
        CHECK(emit_source(user, 0, t) == first);
    return sgl::emit::dump_errors(first);
}

sgl::module_description described(cc::string_view source)
{
    auto const d = sgl::describe({.source = source});
    if (d.has_error())
        FAIL(d.error());
    return d.value();
}
} // namespace

TEST("sgl emit layout - a vector HLSL packs into a row's tail is split in WGSL and packed in MSL")
{
    auto const source = inline_block("    scale: float\n"
                                     "    tint: float3\n",
                                     "look.tint.x, look.tint.y, look.tint.z, look.scale");

    auto const vulkan = text_of(source, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float scale;\n"
                          "    [[vk::offset(4)]] float3 tint;\n"));

    // dx12 needs no attribute: its own packing is the rule.
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("    float scale;\n    float3 tint;\n"));

    // WGSL aligns a vec3f at 16, so its three floats become fields of their own, and a read takes them one by one.
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct look_data {\n"
                        "    scale: f32,\n"
                        "    tint_x: f32,\n"
                        "    tint_y: f32,\n"
                        "    tint_z: f32,\n"
                        "}\n"));
    CHECK(wgsl.contains("vec4f(look.tint_x, look.tint_y, look.tint_z, look.scale)"));

    // MSL's packed_float3 sits at any 4-byte offset.
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    float scale;\n    packed_float3 tint;\n"));
    CHECK(msl.contains("float4(look.tint.x, look.tint.y, look.tint.z, look.scale)"));
}

TEST("sgl emit layout - a float after a float3 fills its tail, which MSL alone has to be told")
{
    auto const source = inline_block("    tint: float3\n"
                                     "    scale: float\n",
                                     "look.tint.x, look.tint.y, look.tint.z, look.scale");
    // WGSL places it natively, so the block is written as it is.
    CHECK(text_of(source, target::wgsl).contains("struct look_data {\n    tint: vec3f,\n    scale: f32,\n}\n"));
    // MSL's float3 is 16 bytes long, so the block is its memory form there.
    CHECK(text_of(source, target::msl).contains("    packed_float3 tint;\n    float scale;\n"));
}

TEST("sgl emit layout - a vector that would cross a row starts the next one on every target")
{
    auto const source = inline_block("    dir: float3\n"
                                     "    uv: float2\n",
                                     "look.dir.x, look.dir.y, look.uv.x, look.uv.y");
    CHECK(text_of(source, target::hlsl_vulkan).contains("    [[vk::offset(16)]] float2 uv;\n"));
    // WGSL and MSL put a float2 after a float3 at 16 by their own rules, so neither needs a memory form.
    CHECK(text_of(source, target::wgsl).contains("struct look_data {\n    dir: vec3f,\n    uv: vec2f,\n}\n"));
    CHECK(text_of(source, target::msl).contains("    float3 dir;\n    float2 uv;\n"));

    auto const d = described(source);
    REQUIRE(d.bindings.size() == 1);
    CHECK(d.bindings[0].members[1].offset == 16);
    CHECK(d.bindings[0].block_size == 24);
}

TEST("sgl emit layout - a float2 HLSL keeps at a 4-byte offset is split where WGSL aligns it at 8")
{
    auto const source = inline_block("    a: float4\n"
                                     "    b: float\n"
                                     "    c: float2\n",
                                     "look.a.x, look.b, look.c.x, look.c.y");
    CHECK(text_of(source, target::hlsl_vulkan).contains("    [[vk::offset(20)]] float2 c;\n"));
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    a: vec4f,\n    b: f32,\n    c_x: f32,\n    c_y: f32,\n"));
    CHECK(wgsl.contains("look.c_x, look.c_y"));
    CHECK(text_of(source, target::msl).contains("    float4 a;\n    float b;\n    packed_float2 c;\n"));
}

TEST("sgl emit layout - a block of full rows packs alike everywhere, and vulkan states each offset")
{
    auto const source = inline_block("    tint: float3\n"
                                     "    to_world: mat4\n"
                                     "    scale: float\n",
                                     "look.tint.x, look.tint.y, look.tint.z, look.scale");
    auto const hlsl = text_of(source, target::hlsl_vulkan);
    CHECK(hlsl.contains("    [[vk::offset(0)]] float3 tint;\n"));
    CHECK(hlsl.contains("    [[vk::offset(16)]] column_major float4x4 to_world;\n"));
    CHECK(hlsl.contains("    [[vk::offset(80)]] float scale;\n"));
    CHECK(text_of(source, target::wgsl).contains("    tint: vec3f,\n    to_world: mat4x4f,\n    scale: f32,\n"));
}

TEST("sgl emit layout - a nested struct starts a row, and what follows packs against its last member")
{
    auto const source = cc::format("struct tint:\n"
                                   "    color: float3\n"
                                   "    strength: float\n"
                                   "\n"
                                   "struct look_t:\n"
                                   "    t: tint\n"
                                   "    after: float\n"
                                   "    dir: float3\n"
                                   "\n"
                                   "{}"
                                   "@inline binding look:\n"
                                   "    scale: float\n"
                                   "    l: look_t\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input){{look}} -> frame:\n"
                                   "    let l = look.l\n"
                                   "    return {{\n"
                                   "        color = float4(l.t.color.x, l.dir.y, look.l.after, look.scale)\n"
                                   "    }}\n",
                                   k_edges);

    // vulkan states each struct's own offsets on it, and the struct's place in the block on the block.
    auto const vulkan = text_of(source, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] tint t;\n"
                          "    [[vk::offset(16)]] float after;\n"
                          "    [[vk::offset(20)]] float3 dir;\n"));
    CHECK(vulkan.contains("    [[vk::offset(16)]] look_t l;\n"));

    // WGSL's uniform rule would round the nested struct to a whole row, so the block is flattened, padding and all.
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct look_data {\n"
                        "    scale: f32,\n"
                        "    _pad0: u32,\n"
                        "    _pad1: u32,\n"
                        "    _pad2: u32,\n"
                        "    l_t_color: vec3f,\n"
                        "    l_t_strength: f32,\n"
                        "    l_after: f32,\n"
                        "    l_dir_x: f32,\n"
                        "    l_dir_y: f32,\n"
                        "    l_dir_z: f32,\n"
                        "}\n"));
    // A read of the whole struct rebuilds it from its pieces.
    CHECK(wgsl.contains("let l: look_t = look_t(tint(look.l_t_color, look.l_t_strength), look.l_after, "
                        "vec3f(look.l_dir_x, look.l_dir_y, look.l_dir_z));\n"));
    CHECK(text_of(source, target::msl)
              .contains("const look_t l = look_t{tint{float3(look.l_t_color), look.l_t_strength}, look.l_after, "
                        "float3(look.l_dir)};\n"));

    auto const d = described(source);
    REQUIRE(d.memory_structs.size() == 2);
    CHECK(d.memory_structs[0].name == "tint");
    CHECK(d.memory_structs[1].name == "look_t");
    CHECK(d.memory_structs[1].space == "constants");
    CHECK(d.memory_structs[1].size == 32);
    CHECK(d.memory_structs[1].members[2].offset == 20);
    CHECK(d.bindings[0].block_size == 48);
}

namespace
{
constexpr auto k_particles = cc::string_view("struct particle:\n"
                                             "    mass: float\n"
                                             "    velocity: float3\n"
                                             "    color: float4\n"
                                             "\n"
                                             "binding work:\n"
                                             "    scale: float\n"
                                             "    items: mut buffer[particle]\n"
                                             "    dirs: mut buffer[float3]\n"
                                             "\n"
                                             "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                             "    let p = work.items[id.x]\n"
                                             "    work.dirs[id.x] = work.dirs[id.x] + p.velocity * work.scale\n"
                                             "    work.items[id.x] = p\n");
} // namespace

TEST("sgl emit layout - a buffer's element packs tight, as a tg struct does")
{
    auto const dx12 = text_of(k_particles, target::hlsl_dx12);
    CHECK(dx12.contains("RWStructuredBuffer<particle> work_items : register(u1, space0);\n"));
    // HLSL wants a struct declared ahead of the buffer that holds it.
    CHECK(dx12.find("struct particle") < dx12.find("RWStructuredBuffer<particle>"));

    auto const vulkan = text_of(k_particles, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float mass;\n"
                          "    [[vk::offset(4)]] float3 velocity;\n"
                          "    [[vk::offset(16)]] float4 color;\n"));

    auto const wgsl = text_of(k_particles, target::wgsl);
    CHECK(wgsl.contains("struct particle_memory {\n"
                        "    mass: f32,\n"
                        "    velocity_x: f32,\n"
                        "    velocity_y: f32,\n"
                        "    velocity_z: f32,\n"
                        "    color: vec4f,\n"
                        "}\n"));
    CHECK(wgsl.contains("var<storage, read_write> work_items: array<particle_memory>;\n"));
    // A float3 element strides by 12, which WGSL's array<vec3f> would not, so it is three floats.
    CHECK(wgsl.contains("struct float3_memory {\n    value_x: f32,\n    value_y: f32,\n    value_z: f32,\n}\n"));
    CHECK(wgsl.contains("var<storage, read_write> work_dirs: array<float3_memory>;\n"));

    // A store of a split value evaluates it once, then stores each piece.
    CHECK(wgsl.contains("    work_dirs[id.x].value_x = stored.x;\n"
                        "    work_dirs[id.x].value_y = stored.y;\n"
                        "    work_dirs[id.x].value_z = stored.z;\n"));
    CHECK(wgsl.contains("    work_items[id.x].mass = stored_1.mass;\n"));

    auto const d = described(k_particles);
    REQUIRE(d.memory_structs.size() == 1);
    CHECK(d.memory_structs[0].space == "storage");
    CHECK(d.memory_structs[0].size == 32);
    CHECK(d.memory_structs[0].members[1].offset == 4);
    CHECK(d.bindings[0].members[1].stride == 32);
    CHECK(d.bindings[0].members[2].stride == 12);
}

TEST("sgl emit layout - a struct in both a constant block and a buffer has two layouts, which is an error")
{
    CHECK(errors_of("struct pair:\n"
                    "    a: float\n"
                    "    b: float3\n"
                    "\n"
                    "binding work:\n"
                    "    one: pair\n"
                    "    many: buffer[pair]\n"
                    "\n"
                    "@compute(64) fun main(@thread_id id: int3){work}:\n"
                    "    let x = work.many[id.x].a + work.one.a\n")
          == "layout-conflict 'pair' is in a constant block of 'work' and in a storage buffer of 'work'\n"
             "layout-conflict 'pair' is in a storage buffer of 'work' and in a constant block of 'work'\n");
}

TEST("sgl emit layout - @no_padding refuses a gap before a member, and never the tail")
{
    auto const padded = cc::format("{}@no_padding @inline binding look:\n"
                                   "    dir: float3\n"
                                   "    uv: float2\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input){{look}} -> frame:\n"
                                   "    return {{\n        color = float4(look.uv.x, 1.0, 1.0, 1.0)\n    }}\n",
                                   k_edges);
    CHECK(errors_of(padded)
          == "padding-forbidden in the block of @no_padding 'look': 'uv' starts at byte 16, 4 bytes past where 'dir' "
             "ends\n");

    // The rest of the last row follows no member, so it is no padding.
    auto const tail = cc::format("{}@no_padding @inline binding look:\n"
                                 "    dir: float3\n"
                                 "\n"
                                 "@pixel fun main_ps(p: pixel_input){{look}} -> frame:\n"
                                 "    return {{\n        color = float4(look.dir.x, 1.0, 1.0, 1.0)\n    }}\n",
                                 k_edges);
    CHECK(text_of(tail, target::wgsl).contains("dir: vec3f"));

    // On a struct it judges the struct's own members, in the space the struct is placed in.
    auto const in_storage = cc::string_view("@no_padding struct particle:\n"
                                            "    mass: float\n"
                                            "    velocity: float4\n"
                                            "\n"
                                            "binding work:\n"
                                            "    items: buffer[particle]\n"
                                            "\n"
                                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                            "    let x = work.items[id.x].mass\n");
    CHECK(text_of(in_storage, target::wgsl).contains("particle_memory"));
    auto const in_constants = cc::string_view("@no_padding struct particle:\n"
                                              "    mass: float\n"
                                              "    velocity: float4\n"
                                              "\n"
                                              "binding work:\n"
                                              "    one: particle\n"
                                              "\n"
                                              "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                              "    let x = work.one.mass\n");
    CHECK(errors_of(in_constants)
          == "padding-forbidden in @no_padding 'particle', as a constant block places it: 'velocity' starts at byte "
             "16, "
             "12 bytes past where 'mass' ends\n");
}

TEST("sgl emit layout - a bool has no layout, and bool32 is the bool GPU memory holds")
{
    auto const with_bool = cc::format("{}@inline binding look:\n"
                                      "    lit: bool\n"
                                      "\n"
                                      "@pixel fun main_ps(p: pixel_input){{look}} -> frame:\n"
                                      "    return {{\n        color = float4(1.0, 1.0, 1.0, 1.0)\n    }}\n",
                                      k_edges);
    CHECK(errors_of(with_bool)
          == "unsupported a member of type 'bool' in an @inline binding: 'look.lit', whose bool has no layout; bool32 "
             "has one\n");

    auto const source = cc::string_view("binding work:\n"
                                        "    flags: mut buffer[bool32]\n"
                                        "\n"
                                        "@compute(64) fun main(@thread_id id: int3){work}:\n"
                                        "    if work.flags[id.x] as bool:\n"
                                        "        work.flags[id.x] = false as bool32\n");
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("RWStructuredBuffer<uint> work_flags"));
    CHECK(dx12.contains("if (work_flags[id.x] != 0u)"));
    CHECK(dx12.contains("work_flags[id.x] = (false ? 1u : 0u);"));
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("array<u32>"));
    CHECK(wgsl.contains("work_flags[id.x] = select(0u, 1u, false);"));

    auto const d = described(source);
    CHECK(d.bindings[0].members[0].stride == 4);
}
