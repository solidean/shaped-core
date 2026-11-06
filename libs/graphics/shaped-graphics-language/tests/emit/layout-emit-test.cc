#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::emit::target;

// SGL places every value in GPU memory by one rule, and each target is made to follow it: every value at its natural
// alignment, a 3-vector at 16, and a struct nested in a constant block on a row of its own.
// `@padding(.none)` places as C++ does instead.
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

TEST("sgl emit layout - a float3 starts at 16, which WGSL and MSL place natively and HLSL pads to")
{
    auto const source = inline_block("    scale: float\n"
                                     "    tint: float3\n",
                                     "look.tint.x, look.tint.y, look.tint.z, look.scale");

    // HLSL would pack the float3 right behind the float, so a padding field fills the gap; vulkan states it too.
    auto const vulkan = text_of(source, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float scale;\n"
                          "    [[vk::offset(4)]] uint sgl_pad0;\n"
                          "    [[vk::offset(8)]] uint sgl_pad1;\n"
                          "    [[vk::offset(12)]] uint sgl_pad2;\n"
                          "    [[vk::offset(16)]] float3 tint;\n"));
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("    float scale;\n    uint sgl_pad0;\n    uint sgl_pad1;\n    uint sgl_pad2;\n    float3 "
                        "tint;\n"));

    // WGSL and MSL align a 3-vector at 16 by their own rules, so the block is written as it is.
    CHECK(text_of(source, target::wgsl).contains("struct look_data {\n    scale: f32,\n    tint: vec3f,\n}\n"));
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    float scale;\n    float3 tint;\n"));
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

TEST("sgl emit layout - a float2 aligns at 8, which HLSL would pack at 4 and so pads to")
{
    auto const source = inline_block("    a: float4\n"
                                     "    b: float\n"
                                     "    c: float2\n",
                                     "look.a.x, look.b, look.c.x, look.c.y");
    CHECK(text_of(source, target::hlsl_vulkan).contains("    [[vk::offset(24)]] float2 c;\n"));
    CHECK(text_of(source, target::hlsl_dx12).contains("    float b;\n    uint sgl_pad0;\n    float2 c;\n"));
    CHECK(text_of(source, target::wgsl).contains("    a: vec4f,\n    b: f32,\n    c: vec2f,\n"));
    CHECK(text_of(source, target::msl).contains("    float4 a;\n    float b;\n    float2 c;\n"));
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

TEST("sgl emit layout - a nested struct starts a row in a block, and what follows starts the next one")
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
    // HLSL's padding fields stand in the struct itself, its tail included, so a buffer would stride by it too.
    auto const vulkan = text_of(source, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] tint t;\n"
                          "    [[vk::offset(16)]] float after;\n"
                          "    [[vk::offset(20)]] uint sgl_pad0;\n"
                          "    [[vk::offset(24)]] uint sgl_pad1;\n"
                          "    [[vk::offset(28)]] uint sgl_pad2;\n"
                          "    [[vk::offset(32)]] float3 dir;\n"
                          "    [[vk::offset(44)]] uint sgl_pad3;\n"));
    CHECK(vulkan.contains("    [[vk::offset(16)]] look_t l;\n"));

    // WGSL's uniform rule is this one, so the block and its structs are written as they are.
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct look_t {\n    t: tint,\n    after: f32,\n    dir: vec3f,\n}\n"));
    CHECK(wgsl.contains("struct look_data {\n    scale: f32,\n    l: look_t,\n}\n"));
    CHECK(wgsl.contains("let l: look_t = look.l;\n"));

    // EMIT-170: MSL's float3 is 16 bytes, so `tint` packs the one `strength` sits in the tail of, and reads it back
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("struct alignas(16) tint\n{\n    packed_float3 color;\n    float strength;\n};\n"));
    CHECK(msl.contains("const look_t l = look.l;\n"));
    CHECK(msl.contains("float3(l.t.color).x"));

    auto const d = described(source);
    REQUIRE(d.memory_structs.size() == 2);
    CHECK(d.memory_structs[0].name == "tint");
    CHECK(d.memory_structs[1].name == "look_t");
    CHECK(d.memory_structs[1].space == "constants");
    CHECK(d.memory_structs[1].size == 48);
    CHECK(d.memory_structs[1].members[2].offset == 32);
    CHECK(d.bindings[0].block_size == 64);
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

TEST("sgl emit layout - a buffer's element aligns every vector to its size, as WGSL and MSL place it")
{
    // HLSL packs a structured buffer as C++ does, so the struct carries its padding, and a float3 element a field of it.
    auto const dx12 = text_of(k_particles, target::hlsl_dx12);
    CHECK(dx12.contains("struct particle\n{\n"
                        "    float mass;\n"
                        "    uint sgl_pad0;\n"
                        "    uint sgl_pad1;\n"
                        "    uint sgl_pad2;\n"
                        "    float3 velocity;\n"
                        "    uint sgl_pad3;\n"
                        "    float4 color;\n"
                        "};\n"));
    CHECK(dx12.contains("RWStructuredBuffer<particle> work_items : register(u1, space0);\n"));
    // HLSL wants a struct declared ahead of the buffer that holds it.
    CHECK(dx12.find("struct particle") < dx12.find("RWStructuredBuffer<particle>"));
    CHECK(dx12.contains("struct float3_memory\n{\n    float3 value;\n    uint _pad0;\n};\n"));
    CHECK(dx12.contains("RWStructuredBuffer<float3_memory> work_dirs : register(u2, space0);\n"));
    CHECK(dx12.contains("work_dirs[id.x].value = work_dirs[id.x].value + p.velocity * work.scale;\n"));

    auto const vulkan = text_of(k_particles, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float mass;\n"
                          "    [[vk::offset(4)]] uint sgl_pad0;\n"
                          "    [[vk::offset(8)]] uint sgl_pad1;\n"
                          "    [[vk::offset(12)]] uint sgl_pad2;\n"
                          "    [[vk::offset(16)]] float3 velocity;\n"
                          "    [[vk::offset(28)]] uint sgl_pad3;\n"
                          "    [[vk::offset(32)]] float4 color;\n"));

    // WGSL's storage rule and MSL's are this one, so both read the element as it is.
    auto const wgsl = text_of(k_particles, target::wgsl);
    CHECK(wgsl.contains("struct particle {\n    mass: f32,\n    velocity: vec3f,\n    color: vec4f,\n}\n"));
    CHECK(wgsl.contains("var<storage, read_write> work_items: array<particle>;\n"));
    CHECK(wgsl.contains("var<storage, read_write> work_dirs: array<vec3f>;\n"));
    auto const msl = text_of(k_particles, target::msl);
    CHECK(msl.contains("    device particle* work_items [[id(1)]];\n    device float3* work_dirs [[id(2)]];\n"));

    auto const d = described(k_particles);
    REQUIRE(d.memory_structs.size() == 1);
    CHECK(d.memory_structs[0].space == "storage");
    CHECK(d.memory_structs[0].size == 48);
    CHECK(d.memory_structs[0].members[1].offset == 16);
    CHECK(d.bindings[0].members[1].stride == 48);
    CHECK(d.bindings[0].members[2].stride == 16);
}

TEST("sgl emit layout - a @padding(.none) buffer packs its element tight, as a tg struct does")
{
    auto packed = cc::string(k_particles);
    packed.replace_first("    items: mut buffer[particle]\n    dirs: mut buffer[float3]\n",
                         "    @padding(.none) items: mut buffer[particle]\n    @padding(.none) dirs: mut "
                         "buffer[float3]\n");

    // HLSL's structured buffer packs as C++ does, so it needs nothing; vulkan states each offset.
    auto const dx12 = text_of(packed, target::hlsl_dx12);
    CHECK(dx12.contains("struct particle\n{\n    float mass;\n    float3 velocity;\n    float4 color;\n};\n"));
    CHECK(dx12.contains("RWStructuredBuffer<float3> work_dirs : register(u2, space0);\n"));
    CHECK(text_of(packed, target::hlsl_vulkan)
              .contains("    [[vk::offset(0)]] float mass;\n"
                        "    [[vk::offset(4)]] float3 velocity;\n"
                        "    [[vk::offset(16)]] float4 color;\n"));

    auto const wgsl = text_of(packed, target::wgsl);
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

    auto const d = described(packed);
    REQUIRE(d.memory_structs.size() == 1);
    CHECK(d.memory_structs[0].space == "storage");
    CHECK(d.memory_structs[0].size == 32);
    CHECK(d.memory_structs[0].members[1].offset == 4);
    CHECK(d.bindings[0].members[1].stride == 32);
    CHECK(d.bindings[0].members[2].stride == 12);
}

TEST("sgl emit layout - a struct placed packed and naturally has two layouts, which is an error")
{
    CHECK(errors_of("struct pair:\n"
                    "    a: float\n"
                    "    b: float3\n"
                    "\n"
                    "binding work:\n"
                    "    near: buffer[pair]\n"
                    "    @padding(.none) far: buffer[pair]\n"
                    "\n"
                    "@compute(64) fun main(@thread_id id: int3){work}:\n"
                    "    let x = work.near[id.x].a + work.far[id.x].a\n")
          == "layout-conflict 'pair' is in a storage buffer of 'work' and in a packed storage buffer of 'work'\n");
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
          == "layout-conflict 'pair' is in a constant block of 'work' and in a storage buffer of 'work'\n");

    // One conflict is one error, whichever of the two bindings finds it, and whichever order they are listed in.
    auto const across = [](cc::string_view list)
    {
        return errors_of(cc::format("struct pair:\n"
                                    "    a: float\n"
                                    "    b: float3\n"
                                    "\n"
                                    "binding one:\n"
                                    "    p: pair\n"
                                    "\n"
                                    "binding two:\n"
                                    "    items: mut buffer[pair]\n"
                                    "\n"
                                    "@compute(64) fun main(@thread_id id: int3){{{}}}:\n"
                                    "    two.items[id.x] = two.items[id.x + 1]\n",
                                    list));
    };
    auto const once
        = cc::string("layout-conflict 'pair' is in a constant block of 'one' and in a storage buffer of 'two'\n");
    CHECK(across("one, two") == once);
    CHECK(across("two, one") == once);
    // The struct has one layout for the whole module, so listing only the buffer's side still meets the conflict.
    CHECK(across("two") == once);
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
    CHECK(errors_of(in_storage)
          == "padding-forbidden in @no_padding 'particle', as a storage buffer places it: 'velocity' starts at byte "
             "16, 12 bytes past where 'mass' ends\n");
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

    // The gap is the struct's, so two bindings placing it find one error between them.
    auto const twice = cc::string_view("@no_padding struct particle:\n"
                                       "    mass: float\n"
                                       "    velocity: float4\n"
                                       "\n"
                                       "binding near:\n"
                                       "    one: particle\n"
                                       "\n"
                                       "binding far:\n"
                                       "    other: particle\n"
                                       "\n"
                                       "@compute(64) fun main(@thread_id id: int3){near, far}:\n"
                                       "    let x = near.one.mass + far.other.mass\n");
    CHECK(errors_of(twice)
          == "padding-forbidden in @no_padding 'particle', as a constant block places it: 'velocity' starts at byte "
             "16, 12 bytes past where 'mass' ends\n");
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
                                        "        work.flags[id.x] = (id.y == 0) as bool32\n");
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("RWStructuredBuffer<uint> work_flags"));
    CHECK(dx12.contains("if (work_flags[id.x] != 0u)"));
    CHECK(dx12.contains("work_flags[id.x] = (id.y == 0 ? 1u : 0u);"));
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("array<u32>"));
    CHECK(wgsl.contains("work_flags[id.x] = select(0u, 1u, id.y == 0);"));

    auto const d = described(source);
    CHECK(d.bindings[0].members[0].stride == 4);
}

namespace
{
// Placed as a C++ struct of tg types is: `tint` at 4, `bias` right behind it at 16, and `weights` from 28 across a row.
constexpr auto k_cpp_block = cc::string_view("@padding(.none)\n"
                                             "binding params:\n"
                                             "    scale: float\n"
                                             "    tint: float3\n"
                                             "    bias: float\n"
                                             "    shift: float2\n"
                                             "    weights: float4\n"
                                             "    values: mut buffer[float]\n"
                                             "\n"
                                             "@compute(1) fun main(){params}:\n"
                                             "    params.values[0] = params.tint.y + params.bias\n"
                                             "    params.values[1] = params.weights.w + params.shift.x\n");
} // namespace

TEST("sgl emit layout - a @padding(.none) block is placed as C++ places the host struct, on every target")
{
    auto const d = described(k_cpp_block);
    REQUIRE(d.bindings.size() == 1);
    CHECK(d.bindings[0].members[1].offset == 4);
    CHECK(d.bindings[0].members[2].offset == 16);
    CHECK(d.bindings[0].members[3].offset == 20);
    CHECK(d.bindings[0].members[4].offset == 28);
    CHECK(d.bindings[0].block_size == 44);

    // dx12's own packing lands every field of the memory form at its offset: each fits its row.
    auto const dx12 = text_of(k_cpp_block, target::hlsl_dx12);
    CHECK(dx12.contains("struct params_data\n"
                        "{\n"
                        "    float scale;\n"
                        "    float3 tint;\n"
                        "    float bias;\n"
                        "    float2 shift;\n"
                        "    float weights_x;\n"
                        "    float weights_y;\n"
                        "    float weights_z;\n"
                        "    float weights_w;\n"
                        "};\n"));
    CHECK(dx12.contains("params_values[1] = params.weights_w + params.shift.x;\n"));

    auto const vulkan = text_of(k_cpp_block, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float scale;\n"
                          "    [[vk::offset(4)]] float3 tint;\n"
                          "    [[vk::offset(16)]] float bias;\n"
                          "    [[vk::offset(20)]] float2 shift;\n"
                          "    [[vk::offset(28)]] float weights_x;\n"
                          "    [[vk::offset(32)]] float weights_y;\n"
                          "    [[vk::offset(36)]] float weights_z;\n"
                          "    [[vk::offset(40)]] float weights_w;\n"));

    auto const wgsl = text_of(k_cpp_block, target::wgsl);
    CHECK(wgsl.contains("struct params_data {\n"
                        "    scale: f32,\n"
                        "    tint_x: f32,\n"
                        "    tint_y: f32,\n"
                        "    tint_z: f32,\n"
                        "    bias: f32,\n"
                        "    shift_x: f32,\n"
                        "    shift_y: f32,\n"
                        "    weights_x: f32,\n"
                        "    weights_y: f32,\n"
                        "    weights_z: f32,\n"
                        "    weights_w: f32,\n"
                        "}\n"));

    // MSL packs what stays in its row, and splits the float4 that crosses one.
    auto const msl = text_of(k_cpp_block, target::msl);
    CHECK(msl.contains("    float scale;\n"
                       "    packed_float3 tint;\n"
                       "    float bias_;\n"
                       "    packed_float2 shift;\n"
                       "    float weights_x;\n"
                       "    float weights_y;\n"
                       "    float weights_z;\n"
                       "    float weights_w;\n"));
}

TEST("sgl emit layout - a @padding(.none) block places 16-bit values and matrices as C++ does")
{
    auto const source = cc::string_view("require shader_f16\n"
                                        "\n"
                                        "@padding(.none)\n"
                                        "binding look:\n"
                                        "    a: half\n"
                                        "    b: float\n"
                                        "    to_world: mat4\n"
                                        "\n"
                                        "binding sink:\n"
                                        "    values: mut buffer[float]\n"
                                        "\n"
                                        "@compute(1) fun main(){sink, look}:\n"
                                        "    sink.values[0] = (look.to_world * float4(look.b, 0.0, 0.0, 1.0)).x\n"
                                        "    sink.values[1] = look.a as float\n");
    auto const d = described(source);
    CHECK(d.bindings[0].members[1].offset == 4);
    CHECK(d.bindings[0].members[2].offset == 8);
    CHECK(d.bindings[0].block_size == 72);

    // A matrix off a row is split into its scalars; HLSL builds it row by row, and the fields hold it column by column.
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("    float16_t a;\n    uint16_t _pad0;\n    float b;\n    float to_world_0;\n"));
    CHECK(dx12.contains("float4x4(look.to_world_0, look.to_world_4, look.to_world_8, look.to_world_12, "
                        "look.to_world_1, "));
    CHECK(text_of(source, target::hlsl_vulkan).contains("    [[vk::offset(8)]] float to_world_0;\n"));
    CHECK(text_of(source, target::wgsl).contains("mat4x4f(look.to_world_0, look.to_world_1, "));
}

TEST("sgl emit layout - a @padding(.none) block holds no struct yet")
{
    CHECK(errors_of("struct pair:\n"
                    "    a: float\n"
                    "    b: float\n"
                    "\n"
                    "@padding(.none)\n"
                    "binding work:\n"
                    "    one: pair\n"
                    "\n"
                    "@compute(64) fun main(@thread_id id: int3){work}:\n"
                    "    let x = work.one.a\n")
          == "unsupported a struct member in a @padding(.none) block: 'work.one'\n");
}

TEST("sgl emit layout - an array of whole rows starts a row and packs alike everywhere")
{
    // CHK-291: an element of whole 16-byte rows has no padding on any target, so a runtime index reads it in place
    constexpr auto source = "binding lights:\n"
                            "    count: int\n"
                            "    pos: float4[4]\n"
                            "    xf: mat4[2]\n"
                            "\n"
                            "binding out_data:\n"
                            "    result: mut buffer[float4]\n"
                            "\n"
                            "@compute(1) fun main(@thread_id id: int3){lights, out_data}:\n"
                            "    out_data.result[0] = lights.xf[id.x % 2] * lights.pos[id.y]\n";

    auto const d = described(source);
    REQUIRE(d.bindings.size() == 2);
    CHECK(d.bindings[0].members[1].offset == 16);
    CHECK(d.bindings[0].members[1].size == 64);
    CHECK(d.bindings[0].members[2].offset == 80);
    CHECK(d.bindings[0].members[2].size == 128);
    CHECK(d.bindings[0].block_size == 208);

    CHECK(text_of(source, target::hlsl_dx12)
              .contains("    int count;\n    uint sgl_pad0;\n    uint sgl_pad1;\n    uint sgl_pad2;\n    float4 "
                        "pos[4];\n"
                        "    column_major float4x4 xf[2];\n"));
    CHECK(text_of(source, target::hlsl_vulkan)
              .contains("    [[vk::offset(16)]] float4 pos[4];\n"
                        "    [[vk::offset(80)]] column_major float4x4 xf[2];\n"));
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    count: i32,\n    pos: array<vec4f, 4>,\n    xf: array<mat4x4f, 2>,\n"));
    CHECK(wgsl.contains("lights.xf[id.x % 2] * lights.pos[id.y]"));
    CHECK(text_of(source, target::msl).contains("    int count;\n    array<float4, 4> pos;\n    array<float4x4, 2> xf;\n"));
}

TEST("sgl emit layout - an array in a memory form is one field, read in place")
{
    // MSL's float3 is 16 bytes, so `k` in its tail makes the block a memory form, and the array stays whole in it
    constexpr auto source = "binding lights:\n"
                            "    off: float3\n"
                            "    k: float\n"
                            "    pos: float4[3]\n"
                            "\n"
                            "binding out_data:\n"
                            "    result: mut buffer[float4]\n"
                            "\n"
                            "fun total(a: float4[3]) -> float4 => a[0] + a[1] + a[2]\n"
                            "\n"
                            "@compute(1) fun main(@thread_id id: int3){lights, out_data}:\n"
                            "    out_data.result[0] = lights.pos[id.x] * lights.k + total(lights.pos)\n";

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    off: vec3f,\n    k: f32,\n    pos: array<vec4f, 3>,\n"));
    CHECK(wgsl.contains("lights.pos[id.x] * lights.k"));
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    packed_float3 off;\n    float k;\n    array<float4, 3> pos;\n"));
    CHECK(msl.contains("lights.pos[id.x] * lights.k"));
    CHECK(!msl.contains("array<float4, 3>{}"));
}

TEST("sgl emit layout - a @padding(.none) block holds an array where a row starts")
{
    auto const fine = "@padding(.none)\n"
                      "binding work:\n"
                      "    s: float\n"
                      "    v: float3\n"
                      "    rows: float4[2]\n"
                      "\n"
                      "binding out_data:\n"
                      "    result: mut buffer[float4]\n"
                      "\n"
                      "@compute(1) fun main(@thread_id id: int3){work, out_data}:\n"
                      "    out_data.result[0] = work.rows[id.x]\n";
    for (auto const t : sgl::emit::all_targets())
        CHECK(text_of(fine, t).contains("work.rows[id.x]"));

    // C++ puts the array right after the float, at byte 4, where no target can declare one
    CHECK(errors_of("@padding(.none)\n"
                    "binding work:\n"
                    "    s: float\n"
                    "    rows: float4[2]\n"
                    "\n"
                    "@compute(64) fun main(@thread_id id: int3){work}:\n"
                    "    let x = work.rows[0]\n")
          == "unsupported an array at byte 4 of a @padding(.none) block, where C++ places it and no target can: "
             "'work.rows'\n");
}

TEST("sgl emit layout - a mat3 is three 16-byte columns, which HLSL holds as whole rows of a float4x3")
{
    constexpr auto source = "struct framed:\n"
                            "    basis: mat3\n"
                            "    scale: float\n"
                            "\n"
                            "binding work:\n"
                            "    xf: mat3\n"
                            "    after: float\n"
                            "    frames: mut buffer[framed]\n"
                            "    mats: mut buffer[mat3]\n"
                            "    @padding(.none) flat: mut buffer[mat3]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                            "    let f = work.frames[id.x]\n"
                            "    work.frames[id.x] = framed(work.xf * f.basis, f.scale + work.after)\n"
                            "    work.mats[id.x] = work.flat[id.x]\n"
                            "    work.flat[id.x] = f.basis\n";

    auto const d = described(source);
    REQUIRE(d.memory_structs.size() == 1);
    CHECK(d.memory_structs[0].size == 64);
    CHECK(d.memory_structs[0].members[1].offset == 48);
    CHECK(d.bindings[0].members[1].offset == 48);
    CHECK(d.bindings[0].members[3].stride == 48);
    CHECK(d.bindings[0].members[4].stride == 36);

    // EMIT-168: a read narrows the rows to the matrix, a write widens it back, and a packed buffer is HLSL's own
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("float4x3 sgl_widen_mat3(float3x3 m)\n"));
    CHECK(dx12.contains("struct framed\n{\n    column_major float4x3 basis;\n    float scale;\n"));
    CHECK(dx12.contains("struct work_data\n{\n    column_major float4x3 xf;\n    float after;\n};\n"));
    CHECK(dx12.contains("struct mat3_memory\n{\n    column_major float4x3 value;\n};\n"));
    CHECK(dx12.contains("RWStructuredBuffer<float3x3> work_flat"));
    CHECK(dx12.contains("framed_value.basis = sgl_widen_mat3(mul(((float3x3)work.xf), ((float3x3)f.basis)));\n"));
    CHECK(dx12.contains("work_mats[id.x].value = sgl_widen_mat3(work_flat[id.x]);\n"));

    // WGSL's mat3x3f and MSL's float3x3 are this layout, and a packed one is split into its scalars
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct framed {\n    basis: mat3x3f,\n    scale: f32,\n}\n"));
    CHECK(wgsl.contains("var<storage, read_write> work_mats: array<mat3x3f>;\n"));
    CHECK(wgsl.contains("work_mats[id.x] = mat3x3f(work_flat[id.x].value_0, work_flat[id.x].value_1, "));
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    device float3x3* work_mats [[id(2)]];\n"));
    CHECK(msl.contains("float3x3(float3(work_flat[id.x].value_0, work_flat[id.x].value_1, work_flat[id.x].value_2), "));
}

TEST("sgl emit layout - an array strides by its element rounded to its alignment, and HLSL holds a 3-vector's as "
     "4-vectors")
{
    constexpr auto source = "struct lamp:\n"
                            "    pos: float4\n"
                            "    color: float3\n"
                            "    radius: float\n"
                            "\n"
                            "struct track:\n"
                            "    corners: float3[3]\n"
                            "    weights: float[4]\n"
                            "\n"
                            "binding scene:\n"
                            "    lamps: lamp[2]\n"
                            "    tracks: mut buffer[track]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){scene}:\n"
                            "    let t = scene.tracks[id.x]\n"
                            "    let mut u = t\n"
                            "    u.corners[1] = t.corners[0] * scene.lamps[1].radius\n"
                            "    scene.tracks[id.x] = track(u.corners, t.weights)\n";

    auto const d = described(source);
    REQUIRE(d.memory_structs.size() == 2);
    CHECK(d.memory_structs[1].name == "track");
    CHECK(d.memory_structs[1].size == 64);
    CHECK(d.memory_structs[1].members[1].offset == 48);

    // EMIT-169: an element reads its first three lanes, writes them, and a whole read or write goes element by element
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("struct track\n{\n    float4 corners[3];\n    float weights[4];\n};\n"));
    CHECK(dx12.contains("u.corners[1].xyz = t.corners[0].xyz * scene.lamps[1].radius;\n"));
    CHECK(dx12.contains("narrowed[2] = u.corners[2].xyz;\n"));
    CHECK(dx12.contains("track_value.corners[2].xyz = widened[2];\n"));

    // WGSL strides it so natively, and MSL packs the 3-vector `radius` sits in the tail of (EMIT-170)
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct track {\n    corners: array<vec3f, 3>,\n    weights: array<f32, 4>,\n}\n"));
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("struct alignas(16) lamp\n{\n    float4 pos;\n    packed_float3 color;\n    float "
                       "radius;\n};\n"));
    CHECK(msl.contains("    device track* scene_tracks [[id(1)]];\n"));
}

TEST("sgl emit layout - a packed array of structs or of arrays waits alike on every target, and a nested array of mat3 "
     "stands in cells")
{
    auto const source_of = [](cc::string_view member, cc::string_view field)
    {
        return cc::format("struct holder:\n    {}\n\nbinding work:\n    {}\n\n"
                          "@compute(64) fun main(@thread_id id: int3){{work}}:\n"
                          "    let x = work.items[id.x]\n",
                          field, member);
    };
    auto const in_buffer
        = [&](cc::string_view member, cc::string_view field) { return errors_of(source_of(member, field)); };
    // EMIT-171: a multi-dimensional array nests its natural stride, and an array of mat3 is held as HLSL holds one
    for (auto const field : {"grid: float4[2, 2]", "weights: float[2, 3]", "turns: mat3[2]", "dirs: float3[2, 3]"})
        for (auto const t : sgl::emit::all_targets())
            CHECK(sgl::emit::dump_errors(emit_source(source_of("items: buffer[holder]", field), 0, t)) == "").context(field);
    // HLSL holds a 3-vector as a 4-vector at any depth
    CHECK(text_of(source_of("items: buffer[holder]", "dirs: float3[2, 3]"), target::hlsl_dx12)
              .contains("    float4 dirs[2][3];\n"));
    // a nested array's matrix stands in a struct of its own, since DXC decorates no matrix deeper in SPIR-V
    for (auto const t : sgl::emit::all_targets())
        CHECK(sgl::emit::dump_errors(emit_source(source_of("items: buffer[holder]", "turns: mat3[2, 2]"), 0, t)) == "");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const text = text_of(source_of("items: buffer[holder]", "turns: mat3[2, 2]"), t);
        CHECK(text.contains("struct sgl_mat3_cell\n{\n    column_major float4x3 m;\n};\n"));
        CHECK(text.contains("sgl_mat3_cell turns[2][2];\n"));
    }
    CHECK(!text_of(source_of("items: buffer[holder]", "turns: mat3[2]"), target::hlsl_dx12).contains("sgl_mat3_cell"));
    CHECK(in_buffer("@padding(.none) items: buffer[holder]", "rows: float4[2, 2]")
          == "unsupported an array in @padding(.none) memory: 'work.items'.rows\n");
}

TEST("sgl emit layout - a packed element's array strides by its element's scalars, split or packed where no array does")
{
    constexpr auto source = "struct tight:\n"
                            "    lead: float\n"
                            "    dirs: float3[3]\n"
                            "    colors: float4[2]\n"
                            "    turn: mat2\n"
                            "\n"
                            "binding work:\n"
                            "    @padding(.none) items: mut buffer[tight]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                            "    let t = work.items[id.x]\n"
                            "    let d = work.items[id.x].dirs[id.x % 3]\n"
                            "    work.items[id.x] = tight(t.lead + d.x, t.dirs, t.colors, t.turn)\n";
    auto const d = described(source);
    CHECK(d.memory_structs[0].members[2].offset == 40);
    CHECK(d.memory_structs[0].size == 88);

    // EMIT-178: WGSL's vec3f strides by 16 and its vec4f wants 16-byte offsets, so both arrays are their scalars
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    dirs: array<f32, 9>,\n    colors: array<f32, 8>,\n    turn: mat2x2f,\n"));
    CHECK(wgsl.contains("let at: i32 = id.x % 3;\n"));
    CHECK(wgsl.contains("vec3f(work_items[id.x].dirs[3 * at + 0], work_items[id.x].dirs[3 * at + 1], "
                        "work_items[id.x].dirs[3 * at + 2])"));
    CHECK(wgsl.contains("work_items[id.x].colors[4 * 1 + 3] = stored_5.w;\n"));
    // MSL's packed vectors stride by their scalars, so an array of them is the array itself
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    array<packed_float3, 3> dirs;\n    array<packed_float4, 2> colors;\n"));
    CHECK(msl.contains("float3(work_items[id.x].dirs[id.x % 3])"));
    // a structured buffer is tight already
    CHECK(text_of(source, target::hlsl_dx12).contains("    float3 dirs[3];\n    float4 colors[2];\n    float2x2 turn;\n"));
}

TEST("sgl emit layout - a mat2 is 16 bytes, which a constant block starts on a row and HLSL holds there as a float4")
{
    constexpr auto source = "struct twisted:\n"
                            "    turn: mat2\n"
                            "    tag: float\n"
                            "\n"
                            "binding spin:\n"
                            "    lead: float\n"
                            "    turn: mat2\n"
                            "    after: float\n"
                            "    twists: mut buffer[twisted]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){spin}:\n"
                            "    let t = spin.twists[id.x]\n"
                            "    spin.twists[id.x] = twisted(spin.turn * t.turn, t.tag + spin.lead + spin.after)\n";
    auto const d = described(source);
    CHECK(d.bindings[0].members[1].offset == 16);
    CHECK(d.bindings[0].members[2].offset == 32);
    CHECK(d.bindings[0].block_size == 36);
    CHECK(d.memory_structs[0].size == 24); // a buffer's element aligns it to its column

    // EMIT-175: HLSL's constant buffer starts each matrix column on a row, so the block holds the two as one float4
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("    float4 turn;\n"));
    CHECK(hlsl.contains("    float2x2 turn;\n    float tag;\n"));
    CHECK(hlsl.contains("mul(sgl_narrow_mat2(spin.turn), t.turn)"));
    CHECK(hlsl.contains("float2x2 sgl_narrow_mat2(float4 m)\n{\n    return float2x2(m.x, m.z, m.y, m.w);\n}\n"));
    // MSL aligns a float2x2 to 8, so a padding field keeps it on its row
    CHECK(text_of(source, target::msl)
              .contains("    float lead;\n    uint sgl_pad0;\n    uint sgl_pad1;\n    uint sgl_pad2;\n    float2x2 "
                        "turn;\n"));

    // a packed block's would be columns a row apart in HLSL's constant buffer, where C++ packs them
    CHECK(errors_of("@padding(.none)\nbinding work:\n    turn: mat2\n\n"
                    "@compute(64) fun main(@thread_id id: int3){work}:\n    let x = work.turn\n")
          == "unsupported a mat2 in @padding(.none) memory: 'work.turn'\n");
}

TEST("sgl emit layout - MSL starts a constant block's struct on a row, and strides an array of one by rows")
{
    constexpr auto source = "struct small:\n"
                            "    x: float\n"
                            "\n"
                            "binding work:\n"
                            "    lead: float\n"
                            "    held: small\n"
                            "    after: float\n"
                            "    out_data: mut buffer[float4]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                            "    work.out_data[id.x] = float4(work.lead, work.held.x, work.after, 0.0)\n";
    auto const d = described(source);
    CHECK(d.bindings[0].members[1].offset == 16);
    CHECK(d.bindings[0].members[2].offset == 32);
    // EMIT-176: MSL aligns the struct to its float alone, so it says the row itself, which also rounds its size up
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("struct alignas(16) small\n"));
    CHECK(msl.contains("    float lead;\n    small held;\n    float after;\n"));

    // EMIT-177: Metal's compiler checks each offset SGL placed a member at, and a struct's size, on every compile
    CHECK(msl.contains("static_assert(__builtin_offsetof(work_data, held) == 16, \"SGL places work_data.held at byte "
                       "16\");\n"));
    CHECK(msl.contains("static_assert(__builtin_offsetof(work_data, after) == 32, "));
    CHECK(msl.contains("static_assert(sizeof(small) == 16, \"SGL makes small 16 bytes\");\n"));
    // no other target is asked to check its layout this way
    CHECK(!text_of(source, target::wgsl).contains("static_assert"));
}

TEST("sgl emit layout - HLSL holds an array of mat3 as one of float4x3, each element narrowed and widened as one is")
{
    constexpr auto source = "struct posed:\n"
                            "    frames: mat3[2]\n"
                            "\n"
                            "binding work:\n"
                            "    poses: mut buffer[posed]\n"
                            "    out_data: mut buffer[float4]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                            "    let p = work.poses[id.x]\n"
                            "    let v = work.poses[id.x].frames[id.x % 2] * float3(1.0)\n"
                            "    work.out_data[id.x] = float4(v.x, v.y, v.z, 1.0)\n"
                            "    work.poses[id.x] = posed([p.frames[1], p.frames[0]])\n";
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("    column_major float4x3 frames[2];\n"));
    CHECK(hlsl.contains("mul(((float3x3)work_poses[id.x].frames[id.x % 2]), (float3)1.0)"));
    CHECK(hlsl.contains("posed_value.frames[1] = sgl_widen_mat3(widened[1]);\n"));
    // every other target's mat3 is three 16-byte columns already, so the array is its own
    CHECK(text_of(source, target::wgsl).contains("    frames: array<mat3x3f, 2>,\n"));
    CHECK(text_of(source, target::msl).contains("    array<float3x3, 2> frames;\n"));
}

TEST("sgl emit layout - an every_element array is one of 4-vectors on every target, read through its own lanes")
{
    constexpr auto source = "binding blur:\n"
                            "    tint: float3\n"
                            "    strength: float\n"
                            "    @padding(.every_element) weights: float[4]\n"
                            "    @padding(.every_element) offsets: float2[3]\n"
                            "    out_data: mut buffer[float4]\n"
                            "\n"
                            "fun total(w: float[4]) -> float => w[0] + w[1] + w[2] + w[3]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){blur}:\n"
                            "    let o = blur.offsets[id.x % 3]\n"
                            "    blur.out_data[id.x] = float4(blur.weights[id.y % 4], o.x, o.y, total(blur.weights))\n";

    auto const d = described(source);
    CHECK(d.bindings[0].members[2].offset == 16);
    CHECK(d.bindings[0].members[2].size == 64);
    CHECK(d.bindings[0].members[3].offset == 80);
    CHECK(d.bindings[0].block_size == 128);

    // EMIT-172: an element reads its own lanes, and a whole read rebuilds the array element by element
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("    float4 weights[4];\n    float4 offsets[3];\n"));
    CHECK(dx12.contains("const float2 o = blur.offsets[id.x % 3].xy;\n"));
    CHECK(dx12.contains("narrowed[3] = blur.weights[3].x;\n"));
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    weights: array<vec4f, 4>,\n    offsets: array<vec4f, 3>,\n"));
    CHECK(wgsl.contains("blur.weights[id.y % 4].x"));

    // EMIT-170: MSL packs the block's float3 whose tail `strength` sits in, so the block is no memory form there
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("struct blur_data\n{\n    packed_float3 tint;\n    float strength;\n    array<float4, 4> "
                       "weights;\n"));
}

TEST("sgl emit layout - an assignment writes a packed or widened member itself, and only its read converts")
{
    constexpr auto source = "struct tally:\n"
                            "    normal_sum: float3\n"
                            "    count: float\n"
                            "\n"
                            "struct framed:\n"
                            "    basis: mat3\n"
                            "    scale: float\n"
                            "\n"
                            "binding work:\n"
                            "    tallies: mut buffer[tally]\n"
                            "    frames: mut buffer[framed]\n"
                            "\n"
                            "@compute(64) fun main(@thread_id id: int3){work}:\n"
                            "    let mut t = work.tallies[id.x]\n"
                            "    t.normal_sum = t.normal_sum + float3(1.0)\n"
                            "    t.normal_sum.x = 2.0\n"
                            "    work.tallies[id.x] = t\n"
                            "    let mut f = work.frames[id.x]\n"
                            "    f.basis = f.basis * f.basis\n"
                            "    work.frames[id.x] = f\n";

    // EMIT-170: MSL's packed field is the place, and its read converts
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    t.normal_sum = float3(t.normal_sum) + float3(1.0);\n"));
    CHECK(msl.contains("    t.normal_sum.x = 2.0;\n"));
    // EMIT-168: HLSL's float4x3 field is the place, and the value widens into it
    CHECK(text_of(source, target::hlsl_dx12)
              .contains("    f.basis = sgl_widen_mat3(mul(((float3x3)f.basis), ((float3x3)f.basis)));\n"));
}
