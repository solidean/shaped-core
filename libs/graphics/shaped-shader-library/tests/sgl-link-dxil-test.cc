#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/link.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

using namespace cc::primitive_defines;

// LINK-6: an entry of a link group reads the group's static sampler and calls the group's callable record.
// SGL's own DXIL says so in its resource table and its callShader, and DXC's validator takes it.
// The group is the deck's (Quest3D's plan D108): trace.sgl's raygen numbers samplers.clamp at s2 and shadow.tests at
// record 1, where light.sgl alone numbers them s1 and 0.

namespace
{
constexpr cc::string_view k_bindings = "module bindings\n"
                                       "binding gTex:\n"
                                       "    map: texture_2d[float4]\n"
                                       "    written: mut buffer[float4]\n";

constexpr cc::string_view k_border = "module border\n"
                                     "sampler gBorder:\n"
                                     "    filter = .linear\n"
                                     "    address = .clamp_edge\n";

constexpr cc::string_view k_samplers = "module samplers\n"
                                       "sampler linear:\n"
                                       "    filter = .linear\n"
                                       "    address = .repeat\n"
                                       "sampler clamp:\n"
                                       "    filter = .linear\n"
                                       "    address = .clamp_edge\n";

constexpr cc::string_view k_shadow = "module shadow\n"
                                     "require raytracing_pipeline\n"
                                     "struct ShadowCall:\n"
                                     "    lit: float = 1.0\n"
                                     "@callable fun testCall(c: mut ShadowCall):\n"
                                     "    c.lit = 1.0\n"
                                     "callables tests = (testCall)\n"
                                     "fun runTests(c: mut ShadowCall):\n"
                                     "    tests[0](mut c)\n";

constexpr cc::string_view k_deep = "module deep\n"
                                   "require raytracing_pipeline\n"
                                   "use border\n"
                                   "struct FogCall:\n"
                                   "    density: float = 0.0\n"
                                   "@callable fun fogCall(c: mut FogCall):\n"
                                   "    c.density = 0.25\n"
                                   "callables fogCalls = (fogCall)\n";

constexpr cc::string_view k_light = "module light\n"
                                    "require raytracing_pipeline\n"
                                    "use bindings\n"
                                    "use samplers\n"
                                    "use shadow\n"
                                    "struct Pick:\n"
                                    "    v: float4 = float4(0.0)\n"
                                    "@callable fun pickCall(c: mut Pick){bindings.gTex}:\n"
                                    "    c.v = bindings.gTex.map.sample(float2(0.5), samplers.clamp, level = 0.0)\n"
                                    "    let mut s = shadow.ShadowCall()\n"
                                    "    shadow.runTests(mut s)\n"
                                    "    c.v *= s.lit\n"
                                    "callables picks = (pickCall)\n"
                                    "fun pick(c: mut Pick):\n"
                                    "    picks[0](mut c)\n";

constexpr cc::string_view k_trace = "require raytracing_pipeline\n"
                                    "use bindings\n"
                                    "use border\n"
                                    "use deep\n"
                                    "use light\n"
                                    "@raygen fun raygen(@launch_id id: int3){bindings.gTex}:\n"
                                    "    let mut c = light.Pick()\n"
                                    "    light.pick(mut c)\n"
                                    "    bindings.gTex.written[id.x] = c.v + bindings.gTex.map.sample(float2(0.5), "
                                    "border.gBorder, level = 0.0)\n";

constexpr sgl::library_file k_library[] = {
    {.name = "bindings.sgl", .source = k_bindings}, {.name = "border.sgl", .source = k_border},
    {.name = "samplers.sgl", .source = k_samplers}, {.name = "shadow.sgl", .source = k_shadow},
    {.name = "deep.sgl", .source = k_deep},         {.name = "light.sgl", .source = k_light},
    {.name = "trace.sgl", .source = k_trace},
};
} // namespace

TEST("slib - a linked callable reads the group's sampler and calls the group's record, which the validator takes")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    sgl::link_entry const entries[] = {
        {.source_name = "trace.sgl", .entry_point = "raygen", .stage = sgl::check::stage::raygen},
        {.source_name = "light.sgl", .entry_point = "pickCall", .stage = sgl::check::stage::callable},
        {.source_name = "shadow.sgl", .entry_point = "testCall", .stage = sgl::check::stage::callable},
    };
    sgl::emit::target const targets[] = {sgl::emit::target::hlsl_dx12};
    auto const linked = sgl::link({.name = "deck", .entries = entries, .library = k_library, .targets = targets});
    if (linked.has_error())
        FAIL(linked.error());
    auto const& map = linked.value().map();
    // trace.sgl's numbering: border's, then samplers', deep's table before shadow's and light's
    REQUIRE(map.samplers.size() == 3);
    CHECK((map.samplers[2].symbol == "samplers.clamp" && map.samplers[2].slot == 2));
    REQUIRE(map.tables.size() == 3);
    CHECK((map.tables[1].symbol == "shadow.tests" && map.tables[1].first_record == 1));
    CHECK((map.tables[2].symbol == "light.picks" && map.tables[2].first_record == 2));

    auto const dxil_of = [&](isize entry) -> cc::string
    {
        auto const e
            = linked.value().emit(entry, sgl::emit::target::hlsl_dx12, {.native = sgl::emit::native_route::require});
        if (e.has_error())
            FAIL(e.error());
        auto const refused = compiler.value().validate(e.value().dxil);
        if (refused.has_value())
            FAIL(refused.value());
        auto const text = compiler.value().disassemble(e.value().dxil);
        REQUIRE(text.has_value());
        return text.value();
    };
    // the light pick: samplers.clamp at s2 of space 10, and shadow.tests[0] at record 1
    auto const pick = dxil_of(1);
    CHECK(pick.contains("s2,space10"));
    CHECK(!pick.contains("s0,space10"));
    CHECK(pick.contains("@dx.op.callShader"));
    CHECK(pick.contains("(i32 159, i32 1,"));
    // the raygen as compiled alone: border at s0, light.picks[0] at record 2
    auto const raygen = dxil_of(0);
    CHECK(raygen.contains("s0,space10"));
    CHECK(raygen.contains("(i32 159, i32 2,"));
    // a callable that reaches nothing numbered is valid as ever
    CHECK(!dxil_of(2).empty());

    // compiled alone, the light pick reads s1 and calls record 0: what the link moved
    auto const own = sgl::compile_to_text({.source = k_light,
                                           .source_name = "light.sgl",
                                           .library = k_library,
                                           .entry_point = "pickCall",
                                           .target = sgl::emit::target::hlsl_dx12,
                                           .native = sgl::emit::native_route::require});
    REQUIRE(own.has_value());
    auto const own_text = compiler.value().disassemble(own.value().dxil);
    REQUIRE(own_text.has_value());
    CHECK(own_text.value().contains("s1,space10"));
    CHECK(own_text.value().contains("(i32 159, i32 0,"));
}
