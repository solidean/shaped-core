#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::emit::target;

// `constants[T]`, a constant buffer of its own beside a group's block of plain members (CHK-539, EMIT-274).

namespace
{
/// Plain members, then a constant buffer, a buffer and another constant buffer: slots 0 to 3 of group 0.
constexpr cc::string_view k_blocks
    = "struct view_params:\n"
      "    scale: float4\n"
      "    offset: float4\n"
      "\n"
      "struct light_params:\n"
      "    color: float3\n"
      "    intensity: float\n"
      "\n"
      "binding work:\n"
      "    factor: float\n"
      "    view: constants[view_params]\n"
      "    values: mut buffer[float4]\n"
      "    light: constants[light_params]\n"
      "\n"
      "@compute fun run(@thread_id id: int3){work}:\n"
      "    let v = work.view\n"
      "    work.values[id.x] = v.scale * work.factor + work.view.offset * work.light.intensity\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> written(cc::string_view source,
                                                                  target t,
                                                                  sgl::emit::native_route route)
{
    return sgl::compile_to_text({.source = source, .entry_point = "run", .target = t, .native = route});
}
} // namespace

TEST("sgl emit - a constants[T] is a ConstantBuffer of its own in HLSL, at the slot its declaration gives it")
{
    auto const dx12 = text_of(k_blocks, target::hlsl_dx12);
    // the block of plain members at slot 0, and each constant buffer a block holding its one value
    CHECK(dx12.contains("ConstantBuffer<work_data> work : register(b0, space0);\n"));
    CHECK(dx12.contains("struct work_view_data\n{\n    view_params view;\n};\n"));
    CHECK(dx12.contains("ConstantBuffer<work_view_data> work_view : register(b1, space0);\n"));
    CHECK(dx12.contains("RWStructuredBuffer<float4> work_values : register(u2, space0);\n"));
    CHECK(dx12.contains("ConstantBuffer<work_light_data> work_light : register(b3, space0);\n"));
    // read whole, and by field in place
    CHECK(dx12.contains("const view_params v = work_view.view;\n"));
    CHECK(dx12.contains("work_view.view.offset * work_light.light.intensity"));

    auto const vulkan = text_of(k_blocks, target::hlsl_vulkan);
    CHECK(vulkan.contains("[[vk::binding(1, 0)]] ConstantBuffer<work_view_data> work_view;\n"));
    CHECK(vulkan.contains("[[vk::binding(3, 0)]] ConstantBuffer<work_light_data> work_light;\n"));
}

TEST("sgl emit - a constants[T] is a var<uniform> of its own in WGSL")
{
    auto const wgsl = text_of(k_blocks, target::wgsl);
    CHECK(wgsl.contains("@group(0) @binding(0) var<uniform> work: work_data;\n"));
    CHECK(wgsl.contains("@group(0) @binding(1) var<uniform> work_view: work_view_data;\n"));
    CHECK(wgsl.contains("@group(0) @binding(2) var<storage, read_write> work_values: array<vec4f>;\n"));
    CHECK(wgsl.contains("@group(0) @binding(3) var<uniform> work_light: work_light_data;\n"));
    CHECK(wgsl.contains("let v: view_params = work_view.view;\n"));
}

TEST("sgl emit - a constants[T] is a constant pointer of its group's argument buffer in MSL, in slot order")
{
    auto const msl = text_of(k_blocks, target::msl);
    CHECK(msl.contains("struct work_arguments\n{\n"
                       "    constant work_data* work [[id(0)]];\n"
                       "    constant work_view_data* work_view [[id(1)]];\n"
                       "    device float4* work_values [[id(2)]];\n"
                       "    constant work_light_data* work_light [[id(3)]];\n"
                       "    uint sgl_lengths [[id(4)]][4];\n"));
    CHECK(msl.contains("    constant auto& work_view = *work_group.work_view;\n"));
}

TEST("sgl emit - SGL's own SPIR-V and DXIL write a constants[T] as any constant block")
{
    for (auto const t : {target::hlsl_vulkan, target::hlsl_dx12})
    {
        auto const e = written(k_blocks, t, sgl::emit::native_route::require);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        auto const is_vulkan = t == target::hlsl_vulkan;
        auto const has_binary = is_vulkan ? !e.value().spirv.empty() : !e.value().dxil.empty();
        CHECK(has_binary);
    }
}

TEST("sgl emit - the interface lists each constants[T] as a constant buffer at its slot, and its footprint by name")
{
    auto const e = written(k_blocks, target::hlsl_dx12, sgl::emit::native_route::text);
    REQUIRE(e.has_value());
    auto slots = cc::string();
    for (auto const& b : e.value().bindings)
        slots += cc::format("{} {} {}:{} {}\n", b.name,
                            b.kind == sgl::described_member_kind::constant ? "block" : "resource", b.group, b.slot,
                            b.block_size);
    CHECK(slots
          == "work block 0:0 4\n"
             "work.view block 0:1 32\n"
             "work.values resource 0:2 0\n"
             "work.light block 0:3 16\n");

    auto const d = sgl::describe({.source = k_blocks});
    REQUIRE(d.has_value());
    REQUIRE(d.value().entry_points.size() == 1);
    CHECK(sgl::check::footprint_text(d.value().entry_points[0].footprint)
          == "work: read, work.view: read, work.values: write, work.light: read");
}

TEST("sgl emit - a constants[T] fills whole 16-byte rows, which sg binds (EMIT-274)")
{
    constexpr auto narrow = "struct tint:\n"
                            "    strength: float\n"
                            "\n"
                            "binding work:\n"
                            "    t: constants[tint]\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute fun run(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = work.t.strength\n";
    CHECK(sgl::emit::dump_errors(emit_source(narrow, 0, target::wgsl)).contains("pad 'tint' to 16 bytes"));
}
