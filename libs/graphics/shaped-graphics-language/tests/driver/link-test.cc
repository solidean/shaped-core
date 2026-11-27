#include "../check/check-test-support.hh"

#include <clean-core/algorithm/sort.hh>
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
