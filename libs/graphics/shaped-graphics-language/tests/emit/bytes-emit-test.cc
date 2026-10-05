#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// `bytes` on every target: raw memory a word at a time, at a byte offset.

namespace
{
constexpr cc::string_view k_bytes = "binding work:\n"
                                    "    src: bytes\n"
                                    "    dst: mut bytes\n"
                                    "\n"
                                    "@compute(64) fun copy(@thread_id id: int3){work}:\n"
                                    "    let at = (id.x as uint) * 16u\n"
                                    "    let one = work.src.load(at)\n"
                                    "    let four = work.src.load4(at + 4u)\n"
                                    "    work.dst.store(at, one)\n"
                                    "    work.dst.store(at + 4u, four.xyz)\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - bytes are a byte address buffer in HLSL, read in t and written in u")
{
    auto const hlsl = text_of(k_bytes, target::hlsl_dx12);
    CHECK(hlsl.contains("\nByteAddressBuffer work_src : register(t0, space0);\n"));
    CHECK(hlsl.contains("\nRWByteAddressBuffer work_dst : register(u1, space0);\n"));
    CHECK(hlsl.contains("work_src.Load(at)"));
    CHECK(hlsl.contains("work_src.Load4(at + 4u)"));
    CHECK(hlsl.contains("work_dst.Store(at, one);"));
    CHECK(hlsl.contains("work_dst.Store3(at + 4u, four.xyz);"));

    auto const vulkan = text_of(k_bytes, target::hlsl_vulkan);
    CHECK(vulkan.contains("\n[[vk::binding(0, 0)]] ByteAddressBuffer work_src;\n"));
    CHECK(vulkan.contains("\n[[vk::binding(1, 0)]] RWByteAddressBuffer work_dst;\n"));
}

TEST("sgl emit - bytes are an array of words in WGSL and MSL, indexed by the offset over 4")
{
    auto const wgsl = text_of(k_bytes, target::wgsl);
    CHECK(wgsl.contains("@group(0) @binding(0) var<storage, read> work_src: array<u32>;\n"));
    CHECK(wgsl.contains("@group(0) @binding(1) var<storage, read_write> work_dst: array<u32>;\n"));
    CHECK(wgsl.contains("work_src[at / 4u]"));
    // several words bind their first index once, and read or write each word beside it
    CHECK(wgsl.contains("= (at + 4u) / 4u;"));
    CHECK(wgsl.contains("vec4<u32>(work_src["));
    CHECK(wgsl.contains("work_dst[at / 4u] = one;"));

    auto const msl = text_of(k_bytes, target::msl);
    CHECK(msl.contains("const device uint* work_src [[id(0)]];"));
    CHECK(msl.contains("    device uint* work_dst [[id(1)]];"));
    CHECK(msl.contains("uint4("));
}

TEST("sgl emit - an array of bytes is indexed nonuniform like any binding array")
{
    constexpr auto arrays = "require binding_arrays\n"
                            "binding tables:\n"
                            "    raw: bytes[4]\n"
                            "    sink: mut buffer[uint]\n"
                            "\n"
                            "@compute(64) fun gather(@thread_id id: int3){tables}:\n"
                            "    let i = id.x % 4\n"
                            "    tables.sink[id.x] = tables.raw[nonuniform i].load(0u)\n";
    auto const hlsl = text_of(arrays, target::hlsl_dx12);
    CHECK(hlsl.contains("ByteAddressBuffer tables_raw[4] : register(t0, space0);\n"));
    CHECK(hlsl.contains("tables_raw[NonUniformResourceIndex(i)].Load(0u)"));
}

TEST("sgl emit - a store of bytes writes them")
{
    // the footprint: what reads bytes reads them, and what stores them writes the storage view
    constexpr auto pinned = "binding work:\n"
                            "    src: bytes\n"
                            "    dst: mut bytes\n"
                            "\n"
                            "@expect(footprint = \"work.src: read, work.dst: read write\")\n"
                            "@compute(64) fun copy(@thread_id id: int3){work}:\n"
                            "    work.dst.store(0u, work.src.load(0u))\n";
    CHECK(sgl::emit::dump_errors(emit_source(pinned, 0, target::hlsl_dx12)) == "");
}
