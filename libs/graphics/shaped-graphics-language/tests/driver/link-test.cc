#include "../check/check-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/async_parallel_for.hh>
#include <nexus/async-test.hh>
#include <shaped-graphics-language/check/dump.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics-language/driver/link.hh>

using namespace sgl_test;
using namespace cc::primitive_defines;

// LINK-1 to LINK-9: a group of entries from several files shares one numbering of samplers and callables tables.
// The deck's example (Quest3D's plan D108): trace.sgl's raygen keeps its file's numbering, the hit and the callables
// of other files take the group's, and every output says so in its registers and its callable records.

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

constexpr cc::string_view k_raysets = "module raysets\n"
                                      "require raytracing_pipeline\n"
                                      "struct Payload:\n"
                                      "    color: float4 = float4(0.0)\n"
                                      "struct ShadowPayload:\n"
                                      "    lit: float = 1.0\n"
                                      "rays R:\n"
                                      "    surface: Payload\n"
                                      "    shadow: ShadowPayload\n";

constexpr cc::string_view k_deep = "module deep\n"
                                   "require raytracing_pipeline\n"
                                   "use border\n"
                                   "struct FogCall:\n"
                                   "    density: float = 0.0\n"
                                   "@callable fun fogCall(c: mut FogCall):\n"
                                   "    c.density = 0.25\n"
                                   "callables fogCalls = (fogCall)\n";

constexpr cc::string_view k_shadow = "module shadow\n"
                                     "require raytracing_pipeline\n"
                                     "struct ShadowCall:\n"
                                     "    lit: float = 1.0\n"
                                     "@callable fun testCall(c: mut ShadowCall):\n"
                                     "    c.lit = 1.0\n"
                                     "callables tests = (testCall)\n"
                                     "fun runTests(c: mut ShadowCall):\n"
                                     "    tests[0](mut c)\n";

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

constexpr cc::string_view k_sky = "module sky\n"
                                  "require raytracing_pipeline\n"
                                  "use deep\n"
                                  "struct SkyCall:\n"
                                  "    color: float4 = float4(0.0)\n"
                                  "@callable fun skyCall(c: mut SkyCall):\n"
                                  "    c.color = float4(0.5)\n"
                                  "callables skyCalls = (skyCall)\n"
                                  "fun sky(c: mut SkyCall):\n"
                                  "    skyCalls[0](mut c)\n";

constexpr cc::string_view k_hits
    = "module hits\n"
      "require raytracing_pipeline\n"
      "use bindings\n"
      "use raysets\n"
      "use samplers\n"
      "sampler gWater:\n"
      "    filter = .nearest\n"
      "    address = .mirror_repeat\n"
      "@closest_hit fun water(h: triangle_hit, p: mut raysets.Payload){bindings.gTex}:\n"
      "    p.color = bindings.gTex.map.sample(float2(h.t), gWater, level = 0.0)\n"
      "    p.color += bindings.gTex.map.sample(float2(h.t), samplers.linear, level = 0.0)\n"
      "@any_hit fun waterShadow(c: triangle_candidate, p: mut raysets.ShadowPayload) -> hit_decision:\n"
      "    p.lit = 0.0\n"
      "    return hit_decision.accept\n";

constexpr cc::string_view k_trace = "require raytracing_pipeline\n"
                                    "use bindings\n"
                                    "use border\n"
                                    "use light\n"
                                    "use raysets\n"
                                    "use sky\n"
                                    "@raygen fun raygen(@launch_id id: int3){bindings.gTex}:\n"
                                    "    let mut c = light.Pick()\n"
                                    "    light.pick(mut c)\n"
                                    "    let mut s = sky.SkyCall()\n"
                                    "    sky.sky(mut s)\n"
                                    "    bindings.gTex.written[id.x] = c.v + s.color + bindings.gTex.map.sample("
                                    "float2(0.5), border.gBorder, level = 0.0)\n"
                                    "@miss fun surfaceMiss(p: mut raysets.Payload):\n"
                                    "    p.color = float4(0.0)\n"
                                    "@miss fun shadowMiss(p: mut raysets.ShadowPayload):\n"
                                    "    p.lit = 1.0\n";

constexpr sgl::library_file k_deck_library[] = {
    {.name = "bindings.sgl", .source = k_bindings}, {.name = "border.sgl", .source = k_border},
    {.name = "samplers.sgl", .source = k_samplers}, {.name = "raysets.sgl", .source = k_raysets},
    {.name = "deep.sgl", .source = k_deep},         {.name = "shadow.sgl", .source = k_shadow},
    {.name = "light.sgl", .source = k_light},       {.name = "sky.sgl", .source = k_sky},
    {.name = "hits.sgl", .source = k_hits},         {.name = "trace.sgl", .source = k_trace},
};

/// The deck's group, in the prototype's listing order: raygen, misses, the hit group's stages, the callables.
constexpr sgl::link_entry k_deck[] = {
    {.source_name = "trace.sgl", .entry_point = "raygen", .stage = sgl::check::stage::raygen},
    {.source_name = "trace.sgl", .entry_point = "surfaceMiss", .stage = sgl::check::stage::miss},
    {.source_name = "trace.sgl", .entry_point = "shadowMiss", .stage = sgl::check::stage::miss},
    {.source_name = "hits.sgl", .entry_point = "water", .stage = sgl::check::stage::closest_hit},
    {.source_name = "hits.sgl", .entry_point = "waterShadow", .stage = sgl::check::stage::any_hit},
    {.source_name = "deep.sgl", .entry_point = "fogCall", .stage = sgl::check::stage::callable},
    {.source_name = "shadow.sgl", .entry_point = "testCall", .stage = sgl::check::stage::callable},
    {.source_name = "light.sgl", .entry_point = "pickCall", .stage = sgl::check::stage::callable},
    {.source_name = "sky.sgl", .entry_point = "skyCall", .stage = sgl::check::stage::callable},
};

constexpr sgl::emit::target k_all[]
    = {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan, sgl::emit::target::wgsl, sgl::emit::target::msl};
/// The targets with ray-tracing pipelines; WebGPU has none.
constexpr sgl::emit::target k_traced[]
    = {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan, sgl::emit::target::msl};

/// `names`, comma-separated.
cc::string joined(cc::span<cc::string const> names)
{
    auto result = cc::string();
    for (auto const& n : names)
        result.appendf("{}{}", result.empty() ? "" : ", ", n);
    return result;
}

sgl::link_result linked(cc::span<sgl::link_entry const> entries,
                        cc::span<sgl::library_file const> library = k_deck_library,
                        cc::span<sgl::emit::target const> targets = k_all)
{
    auto r = sgl::link({.name = "deck", .entries = entries, .library = library, .targets = targets});
    if (r.has_error())
        FAIL(r.error());
    return cc::move(r.value());
}

/// The map's samplers as `symbol@slot`.
cc::string samplers_of(sgl::link_map const& map)
{
    auto result = cc::vector<cc::string>();
    for (auto const& s : map.samplers)
        result.push_back(cc::format("{}@{}", s.symbol, s.slot));
    return joined(result);
}

/// The map's tables as `symbol@first_record`.
cc::string tables_of(sgl::link_map const& map)
{
    auto result = cc::vector<cc::string>();
    for (auto const& t : map.tables)
        result.push_back(cc::format("{}@{}", t.symbol, t.first_record));
    return joined(result);
}

/// The bindings of vulkan's text in set 3, the static samplers', each a slot plus one (EMIT-133), in text order.
cc::string static_bindings(cc::string_view text)
{
    auto result = cc::vector<cc::string>();
    constexpr auto head = cc::string_view("[[vk::binding(");
    for (auto at = text.find(head); at >= 0; at = text.find(head, at + 1))
    {
        auto i = at + head.size();
        auto const start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
            ++i;
        if (text.subview(i).starts_with(", 3)]]"))
            result.push_back(cc::string(text.subview({.offset = start, .size = i - start})));
    }
    return joined(result);
}

/// Every number after `head` in `text`, up to a character no digit is, comma-separated in text order.
cc::string numbers_after(cc::string_view text, cc::string_view head)
{
    auto result = cc::vector<cc::string>();
    for (auto at = text.find(head); at >= 0; at = text.find(head, at + 1))
    {
        auto i = at + head.size();
        auto const start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
            ++i;
        result.push_back(cc::string(text.subview({.offset = start, .size = i - start})));
    }
    return joined(result);
}

sgl::emitted_source emitted(sgl::link_result const& r,
                            isize entry,
                            sgl::emit::target t,
                            sgl::emit::native_route native = sgl::emit::native_route::text)
{
    auto e = r.emit(entry, t, {.native = native});
    if (e.has_error())
        FAIL(e.error());
    return cc::move(e.value());
}

sgl::emitted_source alone(cc::string_view file,
                          cc::string_view entry,
                          sgl::emit::target t,
                          sgl::emit::native_route native = sgl::emit::native_route::text,
                          cc::span<sgl::library_file const> library = k_deck_library)
{
    auto source = cc::string_view();
    for (auto const& f : library)
        if (f.name == file)
            source = f.source;
    auto e = sgl::compile_to_text(
        {.source = source, .source_name = file, .library = library, .entry_point = entry, .target = t, .native = native});
    if (e.has_error())
        FAIL(e.error());
    return cc::move(e.value());
}

/// SPIR-V's set-3 bindings of the module's samplers, each a static sampler's slot plus one (EMIT-133), ascending.
cc::string sampler_bindings(cc::span<u32 const> words)
{
    // OpDecorate (71) of DescriptorSet (34) 3, then the Binding (33) of the same target
    auto in_set = cc::vector<u32>();
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 71 && words[at + 2] == 34 && words[at + 3] == 3)
            in_set.push_back(words[at + 1]);
    auto bindings = cc::vector<u32>();
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 71 && words[at + 2] == 33 && in_set.values().contains(words[at + 1]))
            bindings.push_back(words[at + 3]);
    cc::sort(bindings);
    auto result = cc::vector<cc::string>();
    for (auto const b : bindings)
        result.push_back(cc::format("{}", b));
    return joined(result);
}

/// The shader-table index each OpExecuteCallableKHR (4446) calls, where it is a constant (43), in word order.
cc::string callable_records(cc::span<u32 const> words)
{
    struct constant
    {
        u32 id = 0;
        u32 value = 0;
    };
    auto constants = cc::vector<constant>();
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 43 && (words[at] >> 16) == 4)
            constants.push_back({.id = words[at + 2], .value = words[at + 3]});
    // the index is a uint, an OpBitcast (124) of the int constant where the source wrote an int
    struct bitcast
    {
        u32 id = 0;
        u32 operand = 0;
    };
    auto casts = cc::vector<bitcast>();
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 124)
            casts.push_back({.id = words[at + 2], .operand = words[at + 3]});
    auto result = cc::vector<cc::string>();
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 4446)
        {
            auto id = words[at + 1];
            for (auto const& c : casts)
                if (c.id == id)
                    id = c.operand;
            auto text = cc::string("?");
            for (auto const& c : constants)
                if (c.id == id)
                    text = cc::format("{}", c.value);
            result.push_back(text);
        }
    return joined(result);
}

template <class T>
bool same_bytes(cc::span<T const> a, cc::span<T const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
template <class T>
bool same_bytes(cc::vector<T> const& a, cc::vector<T> const& b)
{
    return same_bytes(cc::span<T const>(a), cc::span<T const>(b));
}

/// The deck's entry `name` by its position.
isize deck_entry(cc::string_view name)
{
    for (auto i = isize(0); i < cc::span<sgl::link_entry const>(k_deck).size(); ++i)
        if (k_deck[i].entry_point == name)
            return i;
    return -1;
}
} // namespace

TEST("sgl link - the deck's group numbers its samplers as the raygen's file does, and appends gWater")
{
    auto const r = linked(k_deck);
    auto const& map = r.map();
    CHECK(map.name == "deck");
    CHECK(samplers_of(map) == "border.gBorder@0, samplers.linear@1, samplers.clamp@2, hits.gWater@3");
    CHECK(map.samplers[3].settings.sampler_type == "non_filtering");
    CHECK(map.samplers[3].settings.settings.address_u == "mirror_repeat");
    CHECK(map.samplers[3].settings.index == 3);
    // the raygen's file's callable section, whole: deep's table comes first by name, before shadow's (CHK-562)
    CHECK(tables_of(map) == "deep.fogCalls@0, shadow.tests@1, light.picks@2, sky.skyCalls@3");
    CHECK(joined(map.tables[2].callables) == "light.pickCall");
    CHECK(!map.has_host_callables);
    CHECK(map.ray_set == "raysets.R");
    CHECK(map.multiplier == 2);
    REQUIRE(map.rays.size() == 2);
    CHECK((map.rays[1].name == "shadow" && map.rays[1].contribution == 1 && map.rays[1].miss == 1));
    // what the hit and the light pick reach moves; what sky's callable and shadow's reach does not (LINK-4)
    CHECK(joined(map.renumbered) == "hits.sgl:water, light.sgl:pickCall");
}

TEST("sgl link - each entry of the deck reads the group's registers and calls the group's records, on every route")
{
    auto const r = linked(k_deck);
    auto const raygen = deck_entry("raygen");
    auto const water = deck_entry("water");
    auto const pick = deck_entry("pickCall");

    // dx12: `s<slot>, space10`, and CallShader at the group's record
    auto const dx12 = [&](isize e) { return emitted(r, e, sgl::emit::target::hlsl_dx12).text; };
    CHECK(numbers_after(dx12(raygen), ": register(s") == "0");
    CHECK(numbers_after(dx12(raygen), "CallShader(uint(") == "2, 3");
    CHECK(numbers_after(dx12(water), ": register(s") == "1, 3");
    CHECK(numbers_after(dx12(pick), ": register(s") == "2");
    CHECK(numbers_after(dx12(pick), "CallShader(uint(") == "1");
    // vulkan's text: set 3, binding slot + 1
    auto const vulkan = [&](isize e) { return emitted(r, e, sgl::emit::target::hlsl_vulkan).text; };
    CHECK(static_bindings(vulkan(water)) == "2, 4");
    CHECK(static_bindings(vulkan(pick)) == "3");
    CHECK(numbers_after(vulkan(pick), "CallShader(uint(") == "1");
    // metal's callable table, at the group's record
    CHECK(numbers_after(emitted(r, pick, sgl::emit::target::msl).text, "sgl_t.callable[uint(") == "1");

    // SGL's own SPIR-V: the same bindings and the same records
    auto const spirv = [&](isize e)
    { return emitted(r, e, sgl::emit::target::hlsl_vulkan, sgl::emit::native_route::require).spirv; };
    CHECK(sampler_bindings(spirv(raygen)) == "1");
    CHECK(callable_records(spirv(raygen)) == "2, 3");
    CHECK(sampler_bindings(spirv(water)) == "2, 4");
    CHECK(sampler_bindings(spirv(pick)) == "3");
    CHECK(callable_records(spirv(pick)) == "1");

    // SGL's own DXIL: the interface says the group's slot, and the binary is the group's (slib's DXC test reads it)
    auto const dxil = emitted(r, pick, sgl::emit::target::hlsl_dx12, sgl::emit::native_route::require);
    CHECK(!dxil.dxil.empty());
    auto slots = cc::vector<cc::string>();
    for (auto const& b : dxil.bindings)
        if (b.is_file_sampler)
            slots.push_back(cc::format("{}@{}", b.name, b.slot));
    CHECK(joined(slots) == "samplers.clamp@2");
}

TEST("sgl link - an entry whose numbers are its file's is the binary it is compiled alone, byte for byte")
{
    auto const r = linked(k_deck);
    for (auto const name : {"raygen", "surfaceMiss", "shadowMiss", "waterShadow", "fogCall", "testCall", "skyCall"})
    {
        auto const e = deck_entry(name);
        auto const& file = k_deck[e].source_name;
        for (auto const t : k_traced)
            CHECK(emitted(r, e, t).text == alone(file, name, t).text);
        auto const vulkan = sgl::emit::target::hlsl_vulkan;
        auto const dx12 = sgl::emit::target::hlsl_dx12;
        auto const require = sgl::emit::native_route::require;
        CHECK(same_bytes(emitted(r, e, vulkan, require).spirv, alone(file, name, vulkan, require).spirv));
        CHECK(same_bytes(emitted(r, e, dx12, require).dxil, alone(file, name, dx12, require).dxil));
    }
    // and a renumbered one is not
    auto const pick = deck_entry("pickCall");
    CHECK(emitted(r, pick, sgl::emit::target::hlsl_dx12).text
          != alone("light.sgl", "pickCall", sgl::emit::target::hlsl_dx12).text);
}

TEST("sgl link - an unlinked compile flattens a callable call to its table's symbolic base, resolved when written")
{
    // LINK-5: the flat tree names the table, and no number; the text still calls the program's section's record
    cc::string_view const library[] = {k_bindings, k_samplers, k_shadow};
    auto const checked = check_with_library(library, k_light);
    auto const* pick = static_cast<sgl::check::flat_entry_point const*>(nullptr);
    for (auto const& e : checked.module.entry_points)
        if (e.name == "pickCall")
            pick = &e;
    REQUIRE(pick != nullptr);
    CHECK(sgl::check::dump_entry_point(checked.module, *pick).contains("(callable_base tests"));
    CHECK(numbers_after(alone("light.sgl", "pickCall", sgl::emit::target::hlsl_dx12).text, "CallShader(uint(") == "0");
}

TEST("sgl link - raygens of two files: the first keeps its numbering, the second appends and is reported")
{
    constexpr cc::string_view m = "module m\nsampler x:\n    filter = .linear\nsampler y:\n    filter = .nearest\n";
    constexpr cc::string_view n = "module n\nsampler z:\n    filter = .linear\n    address = .mirror_repeat\n";
    constexpr cc::string_view a
        = "require raytracing_pipeline\nuse bindings\nuse m\n"
          "@raygen fun ra(@launch_id id: int3){bindings.gTex}:\n"
          "    bindings.gTex.written[id.x] = bindings.gTex.map.sample(float2(0.5), m.x, level = 0.0)\n";
    constexpr cc::string_view b
        = "require raytracing_pipeline\nuse bindings\nuse n\n"
          "@raygen fun rb(@launch_id id: int3){bindings.gTex}:\n"
          "    bindings.gTex.written[id.x] = bindings.gTex.map.sample(float2(0.5), n.z, level = 0.0)\n";
    sgl::library_file const library[] = {{.name = "bindings.sgl", .source = k_bindings},
                                         {.name = "m.sgl", .source = m},
                                         {.name = "n.sgl", .source = n},
                                         {.name = "a.sgl", .source = a},
                                         {.name = "b.sgl", .source = b}};
    sgl::link_entry const entries[] = {
        {.source_name = "a.sgl", .entry_point = "ra", .stage = sgl::check::stage::raygen},
        {.source_name = "b.sgl", .entry_point = "rb", .stage = sgl::check::stage::raygen},
    };
    auto const r = linked(entries, library);
    // dense: z is appended after a's two, whose slot 0 it held in its own file
    CHECK(samplers_of(r.map()) == "m.x@0, m.y@1, n.z@2");
    CHECK(joined(r.map().renumbered) == "b.sgl:rb");
    CHECK(r.map().ray_set.empty());
    CHECK(numbers_after(emitted(r, 1, sgl::emit::target::hlsl_dx12).text, ": register(s") == "2");
    CHECK(emitted(r, 0, sgl::emit::target::hlsl_dx12).text
          == alone("a.sgl", "ra", sgl::emit::target::hlsl_dx12, sgl::emit::native_route::text, library).text);

    // listed the other way round, b keeps its numbering and a is the one moved
    sgl::link_entry const swapped[] = {entries[1], entries[0]};
    auto const s = linked(swapped, library);
    CHECK(samplers_of(s.map()) == "n.z@0, m.x@1, m.y@2");
    CHECK(joined(s.map().renumbered) == "a.sgl:ra");
}

TEST("sgl link - a vertex and a pixel stage of two files: the vertex stage's file keeps its table, the pixel appends")
{
    constexpr cc::string_view m = "module m\nsampler x:\n    filter = .linear\nsampler y:\n    filter = .nearest\n";
    constexpr cc::string_view edge = "module edge\n"
                                     "@vertex struct corner:\n    at: pos3\n"
                                     "struct shaded:\n    @position position: hpos4\n    uv: float2\n"
                                     "@pixel struct target:\n    color: float4\n";
    constexpr cc::string_view vs
        = "use bindings\nuse edge\nuse m\n"
          "@vertex fun vs(v: edge.corner){bindings.gTex} -> edge.shaded:\n"
          "    let h = bindings.gTex.map.sample(float2(0.5), m.x, level = 0.0)\n"
          "    return {position = hpos4(v.at.x, v.at.y + h.x, v.at.z, 1.0), uv = float2(0.0)}\n";
    constexpr cc::string_view ps = "use bindings\nuse edge\n"
                                   "sampler gOwn:\n    filter = .linear\n    address = .mirror_repeat\n"
                                   "@pixel fun ps(p: edge.shaded){bindings.gTex} -> edge.target:\n"
                                   "    return {color = bindings.gTex.map.sample(p.uv, gOwn)}\n";
    sgl::library_file const library[] = {{.name = "bindings.sgl", .source = k_bindings},
                                         {.name = "m.sgl", .source = m},
                                         {.name = "edge.sgl", .source = edge},
                                         {.name = "draw/vs.sgl", .source = vs},
                                         {.name = "draw/ps.sgl", .source = ps}};
    // listed pixel first: the pipeline's order decides, not the listing (LINK-3)
    sgl::link_entry const entries[] = {
        {.source_name = "draw/ps.sgl", .entry_point = "ps"},
        {.source_name = "draw/vs.sgl", .entry_point = "vs"},
    };
    auto const r = linked(entries, library);
    // a program file's own sampler is named by its library path (LINK-2)
    CHECK(samplers_of(r.map()) == "m.x@0, m.y@1, draw/ps.sgl:gOwn@2");
    CHECK(r.map().tables.empty());
    CHECK(r.map().ray_set.empty());
    CHECK(joined(r.map().renumbered) == "draw/ps.sgl:ps");
    CHECK(numbers_after(emitted(r, 0, sgl::emit::target::hlsl_dx12).text, ": register(s") == "2");
    CHECK(static_bindings(emitted(r, 0, sgl::emit::target::hlsl_vulkan).text) == "3");
    CHECK(numbers_after(emitted(r, 1, sgl::emit::target::hlsl_dx12).text, ": register(s") == "0");
    auto const spirv = emitted(r, 0, sgl::emit::target::hlsl_vulkan, sgl::emit::native_route::require).spirv;
    CHECK(sampler_bindings(spirv) == "3");
    // WebGPU and Metal number the same slots
    CHECK(emitted(r, 0, sgl::emit::target::wgsl).text.contains("@binding(3)"));
    CHECK(numbers_after(emitted(r, 0, sgl::emit::target::msl).text, "[[sampler(") == "2");
    CHECK(numbers_after(emitted(r, 1, sgl::emit::target::msl).text, "[[sampler(") == "0");
}

TEST("sgl link - every refusal names the group and what broke")
{
    auto const refused = [](cc::span<sgl::link_entry const> entries,
                            cc::span<sgl::library_file const> library = k_deck_library) -> cc::string
    {
        auto const r = sgl::link({.name = "deck", .entries = entries, .library = library, .targets = k_all});
        if (r.has_value())
            return "linked";
        return r.error();
    };
    CHECK(refused({}).starts_with("deck: error: link-empty:"));

    sgl::link_entry const missing[] = {{.source_name = "trace.sgl", .entry_point = "nothing"}};
    CHECK(refused(missing).starts_with("deck: error: link-no-entry-point: trace.sgl holds no entry point named "
                                       "'nothing' (it holds: raygen 'raygen', miss 'surfaceMiss', miss 'shadowMiss')"));
    sgl::link_entry const wrong_stage[]
        = {{.source_name = "trace.sgl", .entry_point = "raygen", .stage = sgl::check::stage::miss}};
    CHECK(refused(wrong_stage).contains("link-no-entry-point: 'raygen' of trace.sgl is a raygen entry point, and a miss"));
    sgl::link_entry const no_file[] = {{.source_name = "absent.sgl", .entry_point = "raygen"}};
    CHECK(refused(no_file).contains("link-no-entry-point: 'absent.sgl' names no file of the library"));

    // a vertex stage is no ray-tracing pipeline's, and a compute entry point is a group of its own
    constexpr cc::string_view raster = "@vertex struct corner:\n    at: pos3\n"
                                       "@vertex fun vs(v: corner) -> hpos4 => hpos4(v.at.x, v.at.y, v.at.z, 1.0)\n"
                                       "binding work:\n    values: mut buffer[float]\n"
                                       "@compute(64) fun cs(@thread_id id: int3){work}:\n    work.values[id.x] = 1.0\n";
    sgl::library_file const with_raster[]
        = {{.name = "bindings.sgl", .source = k_bindings}, {.name = "border.sgl", .source = k_border},
           {.name = "samplers.sgl", .source = k_samplers}, {.name = "raysets.sgl", .source = k_raysets},
           {.name = "deep.sgl", .source = k_deep},         {.name = "shadow.sgl", .source = k_shadow},
           {.name = "light.sgl", .source = k_light},       {.name = "sky.sgl", .source = k_sky},
           {.name = "trace.sgl", .source = k_trace},       {.name = "raster.sgl", .source = raster}};
    sgl::link_entry const mixed[]
        = {{.source_name = "trace.sgl", .entry_point = "raygen"}, {.source_name = "raster.sgl", .entry_point = "vs"}};
    CHECK(refused(mixed, with_raster)
              .contains("link-mixed-stages: 'trace.sgl:raygen' is a raygen entry point, and "
                        "'raster.sgl:vs' a vertex one"));
    sgl::link_entry const computes[]
        = {{.source_name = "raster.sgl", .entry_point = "cs"}, {.source_name = "raster.sgl", .entry_point = "cs"}};
    CHECK(refused(computes, with_raster).contains("link-mixed-stages:"));
    sgl::link_entry const one_compute[] = {{.source_name = "raster.sgl", .entry_point = "cs"}};
    CHECK(refused(one_compute, with_raster) == "linked");

    // two misses of two ray sets
    constexpr cc::string_view other = "module other\nrequire raytracing_pipeline\n"
                                      "struct Probe:\n    hit: float = 0.0\nrays Q:\n    probe: Probe\n";
    constexpr cc::string_view misses = "require raytracing_pipeline\nuse raysets\nuse other\n"
                                       "@miss fun a(p: mut raysets.Payload):\n    p.color = float4(1.0)\n"
                                       "@miss fun b(p: mut other.Probe):\n    p.hit = 1.0\n";
    sgl::library_file const sets[] = {{.name = "raysets.sgl", .source = k_raysets},
                                      {.name = "other.sgl", .source = other},
                                      {.name = "misses.sgl", .source = misses}};
    sgl::link_entry const two_sets[]
        = {{.source_name = "misses.sgl", .entry_point = "a"}, {.source_name = "misses.sgl", .entry_point = "b"}};
    CHECK(refused(two_sets, sets)
              .contains("deck: error: link-ray-set-mismatch: 'misses.sgl:b' uses ray set 'other.Q', "
                        "and 'misses.sgl:a' another"));

    // seventeen samplers: the raygen's file holds sixteen, and the hit reaches its own as the seventeenth
    auto many = cc::string("module many\n");
    for (auto i = 0; i < 16; ++i)
        many += cc::format("sampler s{}:\n    filter = .linear\n    address = .{}\n", i,
                           i % 2 == 0 ? "repeat" : "clamp_edge");
    constexpr cc::string_view wide
        = "require raytracing_pipeline\nuse bindings\nuse many\n"
          "@raygen fun r(@launch_id id: int3){bindings.gTex}:\n"
          "    bindings.gTex.written[id.x] = bindings.gTex.map.sample(float2(0.5), many.s15, "
          "level = 0.0)\n";
    sgl::library_file const crowded[]
        = {{.name = "bindings.sgl", .source = k_bindings}, {.name = "samplers.sgl", .source = k_samplers},
           {.name = "raysets.sgl", .source = k_raysets},   {.name = "many.sgl", .source = many},
           {.name = "wide.sgl", .source = wide},           {.name = "hits.sgl", .source = k_hits}};
    sgl::link_entry const past[]
        = {{.source_name = "wide.sgl", .entry_point = "r"}, {.source_name = "hits.sgl", .entry_point = "water"}};
    CHECK(refused(past, crowded)
              .contains("deck: error: too-many-samplers: group 'deck' gives sampler 'samplers.linear' "
                        "slot 16, which 'hits.sgl:water' reaches, and a stage holds 16"));

    // the raygen's file ends its section in a table that takes `.host`, and a callable of another file appends one
    constexpr cc::string_view hosted = "require raytracing_pipeline\nuse bindings\n"
                                       "struct Call:\n    x: float = 0.0\n"
                                       "@callable fun mine(c: mut Call):\n    c.x = 1.0\n"
                                       "callables own = (mine, .host)\n"
                                       "@raygen fun r(@launch_id id: int3){bindings.gTex}:\n"
                                       "    let mut c = Call()\n"
                                       "    own[id.x](mut c)\n"
                                       "    bindings.gTex.written[id.x] = float4(c.x)\n";
    sgl::library_file const host[] = {{.name = "bindings.sgl", .source = k_bindings},
                                      {.name = "border.sgl", .source = k_border},
                                      {.name = "deep.sgl", .source = k_deep},
                                      {.name = "hosted.sgl", .source = hosted}};
    sgl::link_entry const after_host[]
        = {{.source_name = "hosted.sgl", .entry_point = "r"}, {.source_name = "deep.sgl", .entry_point = "fogCall"}};
    CHECK(refused(after_host, host)
              .contains("deck: error: link-host-callables: table 'hosted.sgl:own' takes .host, and "
                        "table 'deep.fogCalls' follows it"));
    sgl::link_entry const host_alone[] = {{.source_name = "hosted.sgl", .entry_point = "r"}};
    auto const kept = sgl::link({.name = "deck", .entries = host_alone, .library = host});
    REQUIRE(kept.has_value());
    CHECK(kept.value().map().has_host_callables);
}

TEST("sgl link - a link is deterministic, and its numbering hash is equal exactly where an entry's binary is")
{
    auto const first = linked(k_deck);
    auto const second = linked(k_deck);
    CHECK(sgl::to_json(first.map()) == sgl::to_json(second.map()));
    for (auto i = isize(0); i < first.entry_count(); ++i)
    {
        CHECK(first.numbering_hash(i) == second.numbering_hash(i));
        CHECK(emitted(first, i, sgl::emit::target::hlsl_dx12).text
              == emitted(second, i, sgl::emit::target::hlsl_dx12).text);
    }

    // the light pick beside the raygen alone: the raygen's file numbers what it reaches as the deck does
    auto const pick = deck_entry("pickCall");
    sgl::link_entry const beside[] = {k_deck[0], k_deck[pick]};
    auto const same = linked(beside);
    CHECK(same.numbering_hash(1) == first.numbering_hash(pick));
    auto const require = sgl::emit::native_route::require;
    auto const dx12 = sgl::emit::target::hlsl_dx12;
    CHECK(same_bytes(emitted(same, 1, dx12, require).dxil, emitted(first, pick, dx12, require).dxil));
    // and alone it numbers from 0: another hash, another binary
    sgl::link_entry const lone[] = {k_deck[pick]};
    auto const other = linked(lone);
    CHECK(other.numbering_hash(0) != first.numbering_hash(pick));
    CHECK(!same_bytes(emitted(other, 0, dx12, require).dxil, emitted(first, pick, dx12, require).dxil));
    // sky's callable reaches no number, so every group gives it one hash and one binary
    auto const sky = deck_entry("skyCall");
    sgl::link_entry const sky_alone[] = {k_deck[sky]};
    auto const sky_link = linked(sky_alone);
    CHECK(sky_link.numbering_hash(0) == first.numbering_hash(sky));
    CHECK(same_bytes(emitted(sky_link, 0, dx12, require).dxil, emitted(first, sky, dx12, require).dxil));
}

ASYNC_TEST("sgl link - entries emit from several threads at once as they do one by one")
{
    auto const r = linked(k_deck);
    auto serial = cc::vector<cc::string>();
    for (auto i = isize(0); i < r.entry_count(); ++i)
        for (auto const t : k_traced)
            serial.push_back(emitted(r, i, t).text);
    auto parallel = cc::vector<cc::string>::create_filled(serial.size(), cc::string());
    auto const count = isize(cc::span<sgl::emit::target const>(k_traced).size());
    co_await cc::async_parallel_for(serial.size(),
                                    [&](isize job)
                                    {
                                        auto const e = r.emit(job / count, k_traced[job % count]);
                                        parallel[job] = e.has_value() ? e.value().text : e.error();
                                    });
    for (auto i = isize(0); i < serial.size(); ++i)
        CHECK(serial[i] == parallel[i]);

    // and so do SGL's own binaries, SPIR-V and DXIL
    constexpr sgl::emit::target natives[] = {sgl::emit::target::hlsl_vulkan, sgl::emit::target::hlsl_dx12};
    auto const binary_of = [&](isize entry, sgl::emit::target t) -> cc::vector<byte>
    {
        auto const e = r.emit(entry, t, {.native = sgl::emit::native_route::require});
        if (e.has_error())
            return {};
        auto const& source = e.value();
        return t == sgl::emit::target::hlsl_vulkan
                 ? cc::vector<byte>::create_copy_of(cc::as_bytes(cc::span<u32 const>(source.spirv)))
                 : cc::vector<byte>::create_copy_of(cc::span<byte const>(source.dxil));
    };
    auto serial_binaries = cc::vector<cc::vector<byte>>();
    for (auto i = isize(0); i < r.entry_count(); ++i)
        for (auto const t : natives)
            serial_binaries.push_back(binary_of(i, t));
    auto parallel_binaries = cc::vector<cc::vector<byte>>::create_defaulted(serial_binaries.size());
    co_await cc::async_parallel_for(serial_binaries.size(),
                                    [&](isize job) { parallel_binaries[job] = binary_of(job / 2, natives[job % 2]); });
    for (auto i = isize(0); i < serial_binaries.size(); ++i)
    {
        CHECK(!serial_binaries[i].empty());
        CHECK(same_bytes(serial_binaries[i], parallel_binaries[i]));
    }
}

TEST("sgl link - describe names the tables and the ray set each entry reaches")
{
    auto const r = sgl::describe({.source = k_trace, .source_name = "trace.sgl", .library = k_deck_library});
    if (r.has_error())
        FAIL(r.error());
    REQUIRE(r.value().entry_points.size() == 3);
    CHECK(joined(r.value().entry_points[0].callables_tables) == "light.picks, sky.skyCalls");
    CHECK(r.value().entry_points[0].ray_set.empty());
    CHECK(r.value().entry_points[1].callables_tables.empty());

    constexpr cc::string_view tracing
        = "require raytracing_pipeline\nuse raysets\n"
          "binding frame:\n    world: acceleration_structure[.triangles]\n"
          "    output: mut buffer[float4]\n"
          "@raygen fun r(@launch_id id: int3){frame}:\n"
          "    let mut p = raysets.Payload()\n"
          "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, "
          "0.0, 1.0), t_min = 0.0, t_max = 1.0), raysets.R.surface, mut p)\n"
          "    frame.output[id.x] = p.color\n";
    auto const traced = sgl::describe({.source = tracing, .source_name = "tracing.sgl", .library = k_deck_library});
    if (traced.has_error())
        FAIL(traced.error());
    REQUIRE(traced.value().entry_points.size() == 1);
    CHECK(traced.value().entry_points[0].ray_set == "raysets.R");
}

TEST("sgl link - one file under two sets of options is two files of the group, its symbols one")
{
    // LINK-1: each set of options checks the file once; LINK-2: its own samplers are named by its path either way
    constexpr cc::string_view lit = "require raytracing_pipeline\nuse bindings\nuse raysets\n"
                                    "@option const fancy = false\n"
                                    "sampler gPlain:\n    filter = .linear\n"
                                    "sampler gFancy:\n    filter = .nearest\n    address = .mirror_repeat\n"
                                    "@miss fun sky(p: mut raysets.Payload){bindings.gTex}:\n"
                                    "    const if fancy:\n"
                                    "        p.color = bindings.gTex.map.sample(float2(0.5), gFancy, level = 0.0)\n"
                                    "    else:\n"
                                    "        p.color = bindings.gTex.map.sample(float2(0.5), gPlain, level = 0.0)\n";
    constexpr cc::string_view bare = "require raytracing_pipeline\nuse bindings\n"
                                     "@raygen fun r(@launch_id id: int3){bindings.gTex}:\n"
                                     "    bindings.gTex.written[id.x] = float4(1.0)\n";
    sgl::library_file const library[] = {{.name = "bindings.sgl", .source = k_bindings},
                                         {.name = "raysets.sgl", .source = k_raysets},
                                         {.name = "lit.sgl", .source = lit},
                                         {.name = "bare.sgl", .source = bare}};
    sgl::check::option_value const fancy[] = {{.name = "fancy", .value = "true"}};
    sgl::check::option_value const plain[] = {{.name = "fancy", .value = "false"}};
    sgl::link_entry const entries[] = {
        {.source_name = "bare.sgl", .entry_point = "r"},
        {.source_name = "lit.sgl", .entry_point = "sky", .options = fancy},
        {.source_name = "lit.sgl", .entry_point = "sky", .options = plain},
        {.source_name = "lit.sgl", .entry_point = "sky", .options = fancy},
    };
    auto const r = linked(entries, library);
    // the fancy miss is numbered first, so each takes the other slot than its file gives it
    CHECK(samplers_of(r.map()) == "lit.sgl:gFancy@0, lit.sgl:gPlain@1");
    CHECK(r.entry_name(1) == "lit.sgl:sky:fancy=true");
    CHECK(r.entry_name(2) == "lit.sgl:sky:fancy=false");
    // an entry listed twice is one entry of the map, as it is one build
    CHECK(joined(r.map().renumbered) == "lit.sgl:sky:fancy=true, lit.sgl:sky:fancy=false");
    CHECK(r.numbering_hash(1) == r.numbering_hash(3));
    CHECK(r.numbering_hash(1) != r.numbering_hash(2));
    auto const dx12 = sgl::emit::target::hlsl_dx12;
    CHECK(numbers_after(emitted(r, 1, dx12).text, ": register(s") == "0");
    CHECK(numbers_after(emitted(r, 2, dx12).text, ": register(s") == "1");
    CHECK(emitted(r, 1, dx12).text == emitted(r, 3, dx12).text);
    CHECK(r.map().ray_set == "raysets.R");
}

TEST("sgl link - a ray-tracing group without a raygen appends what each entry reaches, from slot 0")
{
    sgl::link_entry const hit_only[] = {{.source_name = "hits.sgl", .entry_point = "water"}};
    auto const r = linked(hit_only);
    // hits.sgl numbers samplers.linear 0, samplers.clamp 1 and gWater 2; the hit reaches the first and the last
    CHECK(samplers_of(r.map()) == "samplers.linear@0, hits.gWater@1");
    CHECK(joined(r.map().renumbered) == "hits.sgl:water");
    CHECK(numbers_after(emitted(r, 0, sgl::emit::target::hlsl_dx12).text, ": register(s") == "0, 1");
    // the payload the hit takes is a ray of one set alone, which is the group's
    CHECK(r.map().ray_set == "raysets.R");
}

TEST("sgl link - of several ray sets every entry agrees with, the group's is the first by name, in any listing order")
{
    constexpr cc::string_view alt = "module alt\nrequire raytracing_pipeline\nuse raysets\n"
                                    "rays A:\n    surface: raysets.Payload\n";
    constexpr cc::string_view misses = "require raytracing_pipeline\nuse raysets\nuse alt\n"
                                       "@miss fun surface(p: mut raysets.Payload):\n    p.color = float4(1.0)\n"
                                       "@miss fun shadow(p: mut raysets.ShadowPayload):\n    p.lit = 1.0\n";
    sgl::library_file const library[] = {{.name = "raysets.sgl", .source = k_raysets},
                                         {.name = "alt.sgl", .source = alt},
                                         {.name = "misses.sgl", .source = misses}};
    // the surface miss alone agrees with both sets, and `alt.A` sorts first
    sgl::link_entry const surface[] = {{.source_name = "misses.sgl", .entry_point = "surface"}};
    auto const alone_surface = linked(surface, library);
    CHECK(alone_surface.map().ray_set == "alt.A");
    CHECK(alone_surface.map().multiplier == 1);
    // the shadow miss's payload is a ray of `raysets.R` alone, whichever miss is listed first
    sgl::link_entry const both[] = {{.source_name = "misses.sgl", .entry_point = "surface"},
                                    {.source_name = "misses.sgl", .entry_point = "shadow"}};
    sgl::link_entry const swapped[] = {both[1], both[0]};
    auto const forward = linked(both, library);
    auto const backward = linked(swapped, library);
    CHECK(forward.map().ray_set == "raysets.R");
    CHECK(forward.map().multiplier == 2);
    CHECK(sgl::to_json(forward.map()) == sgl::to_json(backward.map()));
}

TEST("sgl link - an emit the link cannot write is an error naming the group, the entry or what it reaches")
{
    sgl::emit::target const vulkan_only[] = {sgl::emit::target::hlsl_vulkan};
    auto const r = linked(k_deck, k_deck_library, vulkan_only);
    auto const dx12 = r.emit(0, sgl::emit::target::hlsl_dx12);
    REQUIRE(dx12.has_error());
    CHECK(dx12.error() == "deck: error: the group was linked for no target hlsl-dx12\n");
    auto const past = r.emit(r.entry_count(), sgl::emit::target::hlsl_vulkan);
    REQUIRE(past.has_error());
    CHECK(past.error() == "deck: error: no entry 9 (the group lists 9)\n");
    auto const before = r.emit(-1, sgl::emit::target::hlsl_vulkan);
    REQUIRE(before.has_error());

    // a map-only link emits nothing
    auto const mapped = sgl::link({.name = "deck", .entries = k_deck, .library = k_deck_library});
    REQUIRE(mapped.has_value());
    CHECK(mapped.value().emit(0, sgl::emit::target::hlsl_vulkan).has_error());

    // LINK-6: the group numbers from the check for no target, so a sampler only a target's branch reaches, which no
    // other entry numbers, is unnumbered when that target is written
    constexpr cc::string_view m = "module m\nsampler x:\n    filter = .linear\nsampler y:\n    filter = .nearest\n";
    constexpr cc::string_view bare = "require raytracing_pipeline\nuse bindings\n"
                                     "@raygen fun r(@launch_id id: int3){bindings.gTex}:\n"
                                     "    bindings.gTex.written[id.x] = float4(1.0)\n";
    constexpr cc::string_view by_target = "require raytracing_pipeline\nuse bindings\nuse raysets\nuse m\n"
                                          "@miss fun sky(p: mut raysets.Payload){bindings.gTex}:\n"
                                          "    const if target_api == .dx12:\n"
                                          "        p.color = bindings.gTex.map.sample(float2(0.5), m.y, level = 0.0)\n"
                                          "    else:\n"
                                          "        p.color = bindings.gTex.map.sample(float2(0.5), m.x, level = 0.0)\n";
    sgl::library_file const library[] = {{.name = "bindings.sgl", .source = k_bindings},
                                         {.name = "raysets.sgl", .source = k_raysets},
                                         {.name = "m.sgl", .source = m},
                                         {.name = "bare.sgl", .source = bare},
                                         {.name = "by_target.sgl", .source = by_target}};
    sgl::link_entry const entries[]
        = {{.source_name = "bare.sgl", .entry_point = "r"}, {.source_name = "by_target.sgl", .entry_point = "sky"}};
    sgl::emit::target const both[] = {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan};
    auto const split = linked(entries, library, both);
    CHECK(samplers_of(split.map()) == "m.x@0");
    CHECK(static_bindings(emitted(split, 1, sgl::emit::target::hlsl_vulkan).text) == "1");
    auto const unnumbered = split.emit(1, sgl::emit::target::hlsl_dx12);
    REQUIRE(unnumbered.has_error());
    CHECK(unnumbered.error().contains("error: unnumbered: 'sky' reaches sampler 'm.y', which the link numbers no "
                                      "slot"));
}

TEST("sgl link - a link outlives its request: every name and source it read is its own")
{
    // the request's strings are built here and freed before anything is emitted
    auto names = cc::vector<cc::string>();
    auto sources = cc::vector<cc::string>();
    for (auto const& f : k_deck_library)
    {
        names.push_back(cc::string(f.name));
        sources.push_back(cc::string(f.source));
    }
    auto result = cc::optional<sgl::link_result>();
    {
        auto library = cc::vector<sgl::library_file>();
        for (auto i = isize(0); i < names.size(); ++i)
            library.push_back({.name = names[i], .source = sources[i]});
        auto entry_files = cc::vector<cc::string>();
        auto entry_names = cc::vector<cc::string>();
        for (auto const& e : k_deck)
        {
            entry_files.push_back(cc::string(e.source_name));
            entry_names.push_back(cc::string(e.entry_point));
        }
        auto entries = cc::vector<sgl::link_entry>();
        for (auto i = isize(0); i < entry_files.size(); ++i)
            entries.push_back({.source_name = entry_files[i], .entry_point = entry_names[i]});
        auto const group = cc::string("deck");
        auto r = sgl::link({.name = group, .entries = entries, .library = library, .targets = k_all});
        REQUIRE(r.has_value());
        result = cc::move(r.value());
    }
    for (auto* const strings : {&names, &sources})
        for (auto& s : *strings)
            for (auto i = isize(0); i < s.size(); ++i)
                s.data()[i] = '#';
    // moved once more, as a host keeps it
    auto const kept = cc::move(result.value());
    auto const reference = linked(k_deck);
    CHECK(sgl::to_json(kept.map()) == sgl::to_json(reference.map()));
    for (auto i = isize(0); i < kept.entry_count(); ++i)
    {
        CHECK(kept.entry_name(i) == reference.entry_name(i));
        auto const text = emitted(kept, i, sgl::emit::target::hlsl_dx12);
        CHECK(text.text == emitted(reference, i, sgl::emit::target::hlsl_dx12).text);
        CHECK(joined(text.library_files) == joined(emitted(reference, i, sgl::emit::target::hlsl_dx12).library_files));
    }
}

TEST("sgl link - the map as JSON")
{
    auto const json = sgl::to_json(linked(k_deck).map());
    CHECK(json.starts_with("{\n  \"name\": \"deck\",\n  \"samplers\": [\n    {\"symbol\": \"border.gBorder\", "
                           "\"slot\": 0, "));
    CHECK(json.contains("{\"symbol\": \"light.picks\", \"first_record\": 2, \"callables\": [\"light.pickCall\"], "
                        "\"has_host\": false}"));
    CHECK(json.contains("\"ray_set\": \"raysets.R\",\n  \"multiplier\": 2,\n  \"rays\": [{\"name\": \"surface\", "
                        "\"contribution\": 0, \"miss\": 0}, {\"name\": \"shadow\", \"contribution\": 1, \"miss\": "
                        "1}],"));
    CHECK(json.ends_with("\"renumbered\": [\"hits.sgl:water\", \"light.sgl:pickCall\"]\n}\n"));
}


// LINK-7: a miss that reads m.y on dx12 and m.x elsewhere; the target-free check sees m.x alone
TEST("sgl link - the numbering hash covers what a target's branch reaches, not only what the check that numbers does")
{
    constexpr cc::string_view m = "module m\nsampler x:\n    filter = .linear\nsampler y:\n    filter = .nearest\n";
    constexpr cc::string_view n = "module n\nsampler z:\n    filter = .linear\n    address = .mirror_repeat\n";
    constexpr cc::string_view a
        = "require raytracing_pipeline\nuse bindings\nuse m\n"
          "@raygen fun ra(@launch_id id: int3){bindings.gTex}:\n"
          "    bindings.gTex.written[id.x] = bindings.gTex.map.sample(float2(0.5), m.x, level = 0.0)\n";
    constexpr cc::string_view bare = "require raytracing_pipeline\nuse bindings\n"
                                     "@raygen fun r(@launch_id id: int3){bindings.gTex}:\n"
                                     "    bindings.gTex.written[id.x] = float4(1.0)\n";
    constexpr cc::string_view others = "require raytracing_pipeline\nuse bindings\nuse raysets\nuse m\nuse n\n"
                                       "@miss fun mx(p: mut raysets.Payload){bindings.gTex}:\n"
                                       "    p.color = bindings.gTex.map.sample(float2(0.5), m.x, level = 0.0)\n"
                                       "@miss fun mz(p: mut raysets.Payload){bindings.gTex}:\n"
                                       "    p.color = bindings.gTex.map.sample(float2(0.5), n.z, level = 0.0)\n"
                                       "@miss fun my(p: mut raysets.Payload){bindings.gTex}:\n"
                                       "    p.color = bindings.gTex.map.sample(float2(0.5), m.y, level = 0.0)\n";
    constexpr cc::string_view by_target = "require raytracing_pipeline\nuse bindings\nuse raysets\nuse m\n"
                                          "@miss fun sky(p: mut raysets.Payload){bindings.gTex}:\n"
                                          "    const if target_api == .dx12:\n"
                                          "        p.color = bindings.gTex.map.sample(float2(0.5), m.y, level = 0.0)\n"
                                          "    else:\n"
                                          "        p.color = bindings.gTex.map.sample(float2(0.5), m.x, level = 0.0)\n";
    sgl::library_file const library[] = {{.name = "bindings.sgl", .source = k_bindings},
                                         {.name = "raysets.sgl", .source = k_raysets},
                                         {.name = "m.sgl", .source = m},
                                         {.name = "n.sgl", .source = n},
                                         {.name = "a.sgl", .source = a},
                                         {.name = "bare.sgl", .source = bare},
                                         {.name = "others.sgl", .source = others},
                                         {.name = "by_target.sgl", .source = by_target}};
    sgl::link_entry const ga[]
        = {{.source_name = "a.sgl", .entry_point = "ra"}, {.source_name = "by_target.sgl", .entry_point = "sky"}};
    sgl::link_entry const gb[] = {{.source_name = "bare.sgl", .entry_point = "r"},
                                  {.source_name = "others.sgl", .entry_point = "mx"},
                                  {.source_name = "others.sgl", .entry_point = "mz"},
                                  {.source_name = "others.sgl", .entry_point = "my"},
                                  {.source_name = "by_target.sgl", .entry_point = "sky"}};
    sgl::emit::target const dx[] = {sgl::emit::target::hlsl_dx12};
    auto const A = linked(ga, library, dx);
    auto const B = linked(gb, library, dx);
    CHECK(samplers_of(A.map()) == "m.x@0, m.y@1");
    CHECK(samplers_of(B.map()) == "m.x@0, n.z@1, m.y@2");
    // dx12 reads m.y at s1 in one group and s2 in the other: two binaries, so two hashes, and B moved it
    CHECK(numbers_after(emitted(A, 1, dx[0]).text, ": register(s") == "1");
    CHECK(numbers_after(emitted(B, 4, dx[0]).text, ": register(s") == "2");
    CHECK(A.numbering_hash(1) != B.numbering_hash(4));
    CHECK(joined(B.map().renumbered).contains("by_target.sgl:sky"));
}

namespace
{
/// Each file sampler `e`'s interface reflects, as `name@slot`, each checked against the map: one name, one slot.
cc::string reflected_samplers(sgl::link_map const& map, sgl::emitted_source const& e)
{
    auto result = cc::vector<cc::string>();
    for (auto const& b : e.bindings)
    {
        if (!b.is_file_sampler)
            continue;
        result.push_back(cc::format("{}@{}", b.name, b.slot));
        auto is_mapped = false;
        for (auto const& s : map.samplers)
            is_mapped = is_mapped || (s.symbol == b.name && s.slot == b.slot);
        CHECK(is_mapped);
    }
    return joined(result);
}
} // namespace

TEST("sgl link - a sampler of the entry's own file is reflected by its map name, on every route")
{
    // LINK-6: hits.sgl's own gWater is `hits.gWater` in the map, and so in the interface of the water hit linked
    auto const r = linked(k_deck);
    auto const water = deck_entry("water");
    for (auto const t : k_traced)
    {
        auto const text = emitted(r, water, t);
        CHECK(reflected_samplers(r.map(), text) == "samplers.linear@1, hits.gWater@3");
        // the text spells it as before, which is what a compiler of the text reflects
        for (auto const& b : text.bindings)
            if (b.name == "hits.gWater")
                CHECK(b.emitted == "gWater");
    }
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
        CHECK(reflected_samplers(r.map(), emitted(r, water, t, sgl::emit::native_route::require))
              == "samplers.linear@1, hits.gWater@3");

    // compiled alone, it keeps the name its file gives it
    auto names = cc::vector<cc::string>();
    for (auto const& b : alone("hits.sgl", "water", sgl::emit::target::hlsl_dx12).bindings)
        if (b.is_file_sampler)
            names.push_back(cc::format("{}@{}", b.name, b.slot));
    CHECK(joined(names) == "samplers.linear@0, gWater@2");
}

TEST("sgl link - a sampler of a file that names no module is reflected by the file's path and its name")
{
    // LINK-2, LINK-6
    constexpr cc::string_view own = "sampler gBorder:\n"
                                    "    filter = .linear\n"
                                    "    address = .clamp_edge\n"
                                    "binding work:\n"
                                    "    map: texture_2d[float4]\n"
                                    "    values: mut buffer[float4]\n"
                                    "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                    "    work.values[id.x] = work.map.sample(float2(0.5), gBorder, level = 0.0)\n";
    sgl::library_file const library[] = {{.name = "x/own.sgl", .source = own}};
    sgl::link_entry const group[] = {{.source_name = "x/own.sgl", .entry_point = "cs"}};
    auto const r = linked(group, library);
    CHECK(samplers_of(r.map()) == "x/own.sgl:gBorder@0");
    for (auto const t : k_all)
        CHECK(reflected_samplers(r.map(), emitted(r, 0, t)) == "x/own.sgl:gBorder@0");
    for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
        CHECK(reflected_samplers(r.map(), emitted(r, 0, t, sgl::emit::native_route::require)) == "x/own.sgl:gBorder@0");
    auto names = cc::vector<cc::string>();
    for (auto const& b : alone("x/own.sgl", "cs", sgl::emit::target::hlsl_dx12, sgl::emit::native_route::text, library)
                             .bindings)
        if (b.is_file_sampler)
            names.push_back(cc::format("{}@{}", b.name, b.slot));
    CHECK(joined(names) == "gBorder@0");
}

TEST("sgl link - an entry that expects a diagnostic is refused as never built, not as missing")
{
    // CHK-403
    constexpr cc::string_view source = "require early_depth_test\n"
                                       "struct varyings:\n"
                                       "    @position position: hpos4\n"
                                       "@vertex fun vs(@vertex_index i: int) -> varyings:\n"
                                       "    return {position = hpos4(0.0, 0.0, 0.0, 1.0)}\n"
                                       "@expect(warning = \"early-depth-keeps-discarded\")\n"
                                       "@early_depth_test\n"
                                       "@pixel fun bad(p: varyings) -> float4:\n"
                                       "    if p.position.x < 0.5 => discard\n"
                                       "    return float4(1.0)\n";
    sgl::library_file const library[] = {{.name = "e.sgl", .source = source}};
    sgl::link_entry const group[] = {{.source_name = "e.sgl", .entry_point = "bad"}};
    auto const r = sgl::link({.name = "deck", .entries = group, .library = library, .targets = k_all});
    REQUIRE(r.has_error());
    CHECK(r.error().starts_with(
        "deck: error: link-no-entry-point: 'bad' of e.sgl expects a diagnostic, so it is never built"));
}
