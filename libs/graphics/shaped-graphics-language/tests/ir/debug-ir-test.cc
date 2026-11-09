#include "ir-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace cc::primitive_defines;

// EMIT-183 below a text: a binary writes the record a text writes, and describes the same site.
// What a record looks like on the GPU is sg's compute-diagnostics test and the routes test beside it.

namespace
{
constexpr cc::string_view k_source
    = "enum tone:\n"
      "    low\n"
      "    high\n"
      "\n"
      "binding work:\n"
      "    values: mut buffer[int]\n"
      "\n"
      "struct link:\n"
      "    @position position: hpos4\n"
      "\n"
      "@pixel struct target:\n"
      "    color: float4\n"
      "\n"
      "@compute(4) fun cs(@thread_id id: int3){work}:\n"
      "    let v = work.values[id.x]\n"
      "    let t = tone.high\n"
      "    print \"id $id, f $(float3(1.0, 2.0, 3.0)), b $(v > 0), n $(uint(v)), t $t\"\n"
      "    assert v >= 0, \"a negative input $v\"\n"
      "    work.values[id.x] = v * 2\n"
      "\n"
      "@vertex fun vs(@vertex_index v: int) -> link:\n"
      "    assert v < 2, \"vertex $v\"\n"
      "    return { position = hpos4(0.0, 0.0, 0.0, 1.0) }\n"
      "\n"
      "@pixel fun ps(l: link) -> target:\n"
      "    print \"pixel at $(l.position.x)\"\n"
      "    assert l.position.x < 100.0, \"far right\"\n"
      "    return { color = float4(1.0) }\n";

struct route
{
    sgl::emit::target target;
    cc::string_view entry;
};

constexpr route k_routes[] = {
    {sgl::emit::target::hlsl_dx12, "cs"},   {sgl::emit::target::hlsl_dx12, "vs"},
    {sgl::emit::target::hlsl_dx12, "ps"},   {sgl::emit::target::hlsl_vulkan, "cs"},
    {sgl::emit::target::hlsl_vulkan, "vs"}, {sgl::emit::target::hlsl_vulkan, "ps"},
};

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> written(route const& r, sgl::emit::native_route native)
{
    return sgl::compile_to_text({.source = k_source,
                                 .entry_point = r.entry,
                                 .target = r.target,
                                 .diagnostics = true,
                                 .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                                 .native = native});
}
} // namespace

TEST("sgl ir - a binary describes the debug sites its text describes")
{
    for (auto const& r : k_routes)
    {
        auto const text = written(r, sgl::emit::native_route::text);
        auto const native = written(r, sgl::emit::native_route::require);
        REQUIRE(text.has_value());
        REQUIRE(native.has_value());
        REQUIRE(!text.value().debug_sites.empty());
        CHECK((!native.value().spirv.empty() || !native.value().dxil.empty()));
        REQUIRE(native.value().debug_sites.size() == text.value().debug_sites.size());
        for (auto i = isize(0); i < text.value().debug_sites.size(); ++i)
            CHECK((native.value().debug_sites[i] == text.value().debug_sites[i]));
    }
}

TEST("sgl ir - a record reserves its words once, in the buffer sg binds")
{
    auto const dxil = written({sgl::emit::target::hlsl_dx12, "ps"}, sgl::emit::native_route::require);
    REQUIRE(dxil.has_value());
    CHECK(dxil.value().text.contains("atomic_iadd"));
    CHECK(dxil.value().text.contains("sg_debug"));
}

TEST("sgl ir - a stage the device lets write no storage drops its prints, and declares no buffer")
{
    auto const e = sgl::compile_to_text({.source = k_source,
                                         .entry_point = "vs",
                                         .target = sgl::emit::target::hlsl_vulkan,
                                         .diagnostics = true,
                                         .native = sgl::emit::native_route::require});
    REQUIRE(e.has_value());
    CHECK(!e.value().spirv.empty());
    CHECK(e.value().debug_sites.empty());
    CHECK(!e.value().text.contains("sg_debug"));
}

TEST("sgl ir - no program reaches a reserved binding")
{
    constexpr cc::string_view source = "@compute(1) fun cs(){sg_debug}:\n"
                                       "    let a = 1\n";
    CHECK(
        !sgl::compile_to_text({.source = source, .entry_point = "cs", .target = sgl::emit::target::hlsl_dx12}).has_value());
}

TEST("sgl ir - a compile that reaches no record declares no window")
{
    constexpr cc::string_view source = "@compute(1) fun cs():\n"
                                       "    let a = 1\n";
    auto const e = sgl::compile_to_text(
        {.source = source, .entry_point = "cs", .target = sgl::emit::target::hlsl_dx12, .diagnostics = true});
    REQUIRE(e.has_value());
    CHECK(!e.value().text.contains("sg_debug"));
}

TEST("sgl ir - a robustness report writes through the same record")
{
    constexpr cc::string_view source = "binding work:\n"
                                       "    values: mut buffer[int]\n"
                                       "\n"
                                       "@compute(4) fun cs(@thread_id id: int3){work}:\n"
                                       "    work.values[id.x] = 12 / work.values[0]\n";
    for (auto const target : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const e = sgl::compile_to_text({.source = source,
                                             .entry_point = "cs",
                                             .target = target,
                                             .diagnostics = true,
                                             .guards_undefined = sgl::check::undefined_guard::report,
                                             .native = sgl::emit::native_route::require});
        REQUIRE(e.has_value());
        CHECK(e.value().debug_sites.size() >= 1);
        CHECK(e.value().text.contains("sg_debug"));
    }
}

// An unpack takes one word whatever its result's width, so the call's scalar is never splatted to the result.
TEST("sgl ir - an unpack of a word lowers to SPIR-V, and the DXIL writer declines it for the text route")
{
    constexpr auto source = cc::string_view("binding work:\n"
                                            "    words: buffer[uint]\n"
                                            "    out_values: mut buffer[float4]\n"
                                            "\n"
                                            "@compute(1) fun cs(@thread_id id: int3){work}:\n"
                                            "    let w = work.words[id.x]\n"
                                            "    let h = unpack_half2x16(w)\n"
                                            "    work.out_values[id.x] = float4(h.x, h.y, unpack_unorm4x8(w).x, "
                                            "unpack_snorm4x8(w).x)\n");
    for (auto const t : {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12})
    {
        auto const e = sgl::compile_to_text(
            {.source = source, .entry_point = "cs", .target = t, .native = sgl::emit::native_route::prefer});
        REQUIRE(e.has_value());
        if (t == sgl::emit::target::hlsl_vulkan)
            CHECK(!e.value().spirv.empty());
        else
            CHECK(e.value().dxil.empty()); // the text route, where DXC writes the unpack
    }
}

// An if whose branches all return leaves its merge unreached, and a nested one's merge then reaches only that: a block
// with predecessors and no dominator, which the simplifier must step over.
TEST("sgl ir - an entry point that returns from every branch of a nested if lowers")
{
    constexpr auto source = cc::string_view("struct v:\n"
                                            "    @position position: hpos4\n"
                                            "    uv: float2\n"
                                            "@pixel struct target:\n"
                                            "    color: float4\n"
                                            "@pixel fun ps(p: v) -> target:\n"
                                            "    if p.uv.x > 0.0:\n"
                                            "        return {color = float4(1.0)}\n"
                                            "    else:\n"
                                            "        if p.uv.y > 0.0:\n"
                                            "            return {color = float4(0.5)}\n"
                                            "        else:\n"
                                            "            return {color = float4(0.25)}\n");
    auto const e = sgl::compile_to_text({.source = source,
                                         .entry_point = "ps",
                                         .target = sgl::emit::target::hlsl_vulkan,
                                         .native = sgl::emit::native_route::prefer});
    REQUIRE(e.has_value());
    CHECK(!e.value().spirv.empty());
}
