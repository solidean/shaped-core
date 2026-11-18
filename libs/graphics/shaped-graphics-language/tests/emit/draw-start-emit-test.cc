#include "emit-test-support.hh"

#include <babel-data/shader/dxil_container.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::check::dxil_shader_model;
using sgl::check::fill;
using sgl::emit::mesh_form;
using sgl::emit::native_route;
using sgl::emit::target;

// The draw's start under the `root_constant` fill of `draw_start` (EMIT-128), and the slot a host writes it into
// (EMIT-271): a dx12 vertex entry that takes `@vertex_index`, `@instance_index` or `@first_instance` reads where the draw
// started from `sgl_draw_start` at `b1, space9`, at Shader Model 6.6 on both routes.

namespace
{
constexpr native_route k_routes[] = {native_route::text, native_route::require};
constexpr auto k_six_six = dxil_shader_model{.major = 6, .minor = 6};
constexpr auto k_six_eight = dxil_shader_model{.major = 6, .minor = 8};
/// The core fills a host compiling dx12 for Shader Model 6.6 asks for.
constexpr auto k_six_six_fills = sgl::check::core_fills{.draw_start = fill::root_constant, .wave_size = fill::none};

/// A vertex entry taking each input `inputs` names, as SGL spells its parameters, whose position reads `reads`.
[[nodiscard]] cc::string vertex_of(cc::string_view inputs, cc::string_view reads)
{
    return cc::format("require first_instance\n"
                      "struct varyings:\n"
                      "    @position position: hpos4\n"
                      "\n"
                      "@vertex fun vs({}) -> varyings:\n"
                      "    return {{ position = hpos4({}, 0.0, 0.0, 1.0) }}\n"
                      "\n"
                      "@pixel fun ps(p: varyings) -> float4:\n"
                      "    return float4(p.position.x, 0.0, 0.0, 1.0)\n",
                      inputs, reads);
}

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> dx12(cc::string_view source,
                                                               cc::string_view entry,
                                                               native_route route,
                                                               sgl::check::core_fills core = k_six_six_fills)
{
    return sgl::compile_to_text(
        {.source = source, .entry_point = entry, .target = target::hlsl_dx12, .core = core, .native = route});
}

/// The shader model SGL's own DXIL container states in its program's header.
[[nodiscard]] dxil_shader_model container_model(cc::span<byte const> bytes)
{
    auto const c = babel::dxil::read(bytes);
    REQUIRE(c.has_value());
    auto const* const part = c.value().part_named("DXIL");
    REQUIRE(part != nullptr);
    auto const p = babel::dxil::program_of(*part);
    REQUIRE(p.has_value());
    return {.major = i32(p.value().model_major), .minor = i32(p.value().model_minor)};
}

/// The features SGL's own DXIL container states in its `SFI0` part.
[[nodiscard]] u64 container_features(cc::span<byte const> bytes)
{
    auto const c = babel::dxil::read(bytes);
    REQUIRE(c.has_value());
    auto const* const part = c.value().part_named("SFI0");
    REQUIRE(part != nullptr);
    REQUIRE(part->data.size() >= 8);
    auto flags = u64(0);
    for (auto i = 0; i < 8; ++i)
        flags |= u64(part->data[i]) << (8 * i);
    return flags;
}

/// `D3D_SHADER_REQUIRES_*`'s bit of `SV_StartVertexLocation` and `SV_StartInstanceLocation`, Shader Model 6.8's.
constexpr auto k_feature_start_location = u64(1) << 32;

/// The reserved slots `e` lists.
[[nodiscard]] cc::vector<sgl::interface_binding> reserved_of(sgl::emitted_source const& e)
{
    auto found = cc::vector<sgl::interface_binding>();
    for (auto const& b : e.bindings)
        if (b.is_reserved)
            found.push_back(b);
    return found;
}

/// That `e` lists the draw's start slot once, as EMIT-271 states it, used as `is_used` says, and lays its block out.
void check_draw_start_slot(sgl::emitted_source const& e, bool is_used)
{
    auto const reserved = reserved_of(e);
    REQUIRE(reserved.size() == 1);
    auto const& slot = reserved[0];
    CHECK(slot.reserved == sgl::reserved_slot::draw_start);
    CHECK(slot.name == "draw_start");
    CHECK(slot.emitted == "sgl_draw_start");
    CHECK(slot.kind == sgl::described_member_kind::constant);
    CHECK(!slot.is_inline);
    CHECK(slot.group == 9);
    CHECK(slot.slot == 1);
    CHECK(slot.count == 1);
    CHECK(slot.block_size == 8);
    CHECK(slot.is_used == is_used);
    auto layouts = 0;
    for (auto const& l : e.layouts)
        if (l.global == "sgl_draw_start")
        {
            ++layouts;
            CHECK(l.stride == 0);
            REQUIRE(l.fields.size() == 2);
            CHECK(l.fields[0].name == "first_vertex");
            CHECK(l.fields[0].offset == 0);
            CHECK(l.fields[1].name == "first_instance");
            CHECK(l.fields[1].offset == 4);
        }
    CHECK(layouts == 1);
}

/// That `e` lists no slot of the draw's start, and no layout of it.
void check_no_draw_start_slot(sgl::emitted_source const& e)
{
    for (auto const& b : e.bindings)
        CHECK(b.reserved != sgl::reserved_slot::draw_start);
    for (auto const& l : e.layouts)
        CHECK(l.global != "sgl_draw_start");
}
} // namespace

TEST("sgl emit - the root-constant fill reads @vertex_index's draw start from sgl_draw_start at 6.6 (EMIT-128, "
     "EMIT-271)")
{
    auto const source = vertex_of("@vertex_index v: int", "v as float");
    for (auto const route : k_routes)
    {
        auto const e = dx12(source, "vs", route);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().dxil_shader_model == k_six_six);
        check_draw_start_slot(e.value(), true);
        if (route == native_route::require)
        {
            REQUIRE(!e.value().dxil.empty());
            CHECK(container_model(e.value().dxil) == k_six_six);
            CHECK((container_features(e.value().dxil) & k_feature_start_location) == 0);
            continue;
        }
        auto const& text = e.value().text;
        CHECK(text.contains("struct sgl_draw_start_block\n{\n    uint first_vertex;\n    uint first_instance;\n};\n"
                            "ConstantBuffer<sgl_draw_start_block> sgl_draw_start : register(b1, space9);\n"))
            .dump("text", text);
        CHECK(text.contains(" : SV_VertexID")).dump("text", text);
        CHECK(text.contains(" + sgl_draw_start.first_vertex)")).dump("text", text);
        CHECK(!text.contains("SV_Start")).dump("text", text);
    }
}

TEST("sgl emit - the root-constant fill reads @instance_index and @first_instance from sgl_draw_start (EMIT-128)")
{
    struct input_case
    {
        cc::string_view inputs;
        cc::string_view reads;
        /// What the text reads the draw's start into, each; empty past the last.
        cc::string_view expected[3];
    };
    input_case const cases[] = {
        {"@instance_index i: int", "i as float", {" : SV_InstanceID", " + sgl_draw_start.first_instance)"}},
        {"@first_instance f: int", "f as float", {"int(sgl_draw_start.first_instance)"}},
        // both read the block's first instance, where the native fill shares `@first_instance`'s one semantic
        {"@instance_index i: int, @first_instance f: int",
         "(i as float) + (f as float)",
         {" : SV_InstanceID", " + sgl_draw_start.first_instance)", "int(sgl_draw_start.first_instance)"}},
        {"@vertex_index v: int, @instance_index i: int, @first_instance f: int",
         "(v as float) + (i as float) + (f as float)",
         {" + sgl_draw_start.first_vertex)", " + sgl_draw_start.first_instance)", "int(sgl_draw_start.first_instance)"}},
    };
    for (auto const& c : cases)
        for (auto const route : k_routes)
        {
            auto const source = vertex_of(c.inputs, c.reads);
            auto const e = dx12(source, "vs", route);
            REQUIRE(e.has_value()).dump("inputs", c.inputs).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK(e.value().dxil_shader_model == k_six_six).dump("inputs", c.inputs);
            check_draw_start_slot(e.value(), true);
            if (route == native_route::require)
            {
                CHECK(container_model(e.value().dxil) == k_six_six).dump("inputs", c.inputs);
                CHECK((container_features(e.value().dxil) & k_feature_start_location) == 0).dump("inputs", c.inputs);
                continue;
            }
            auto const& text = e.value().text;
            for (auto const expected : c.expected)
                if (!expected.empty())
                    CHECK(text.contains(expected)).dump("inputs", c.inputs).dump("expected", expected).dump("text", text);
            CHECK(!text.contains("SV_Start")).dump("inputs", c.inputs).dump("text", text);
            CHECK(text.contains("register(b1, space9)")).dump("inputs", c.inputs);
        }
}

TEST("sgl emit - an entry that takes the draw's start and reads none lists its slot, unused (EMIT-271)")
{
    auto const source = vertex_of("@vertex_index v: int, @first_instance f: int", "0.0");
    for (auto const route : k_routes)
    {
        auto const e = dx12(source, "vs", route);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().dxil_shader_model == k_six_six);
        check_draw_start_slot(e.value(), false);
        if (route == native_route::text)
        {
            // the text declares the block whatever the body reads, as it keeps an unread input as a parameter
            CHECK(e.value().text.contains("register(b1, space9)"));
            CHECK(!e.value().text.contains("SV_Start"));
        }
        else
            CHECK(container_model(e.value().dxil) == k_six_six);
    }
}

TEST("sgl emit - an entry that takes no draw start, and every other stage, lists no slot of it (EMIT-271)")
{
    auto const source = vertex_of("", "0.0");
    for (auto const route : k_routes)
        for (auto const entry : {cc::string_view("vs"), cc::string_view("ps")})
        {
            auto const e = dx12(source, entry, route);
            REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK(reserved_of(e.value()).empty()).dump("entry", entry);
            check_no_draw_start_slot(e.value());
            if (route == native_route::text)
                CHECK(!e.value().text.contains("sgl_draw_start")).dump("entry", entry);
        }
    // the pixel stage of a pipeline whose vertex stage takes it lists none either
    auto const pixel = dx12(vertex_of("@vertex_index v: int", "v as float"), "ps", native_route::text);
    REQUIRE(pixel.has_value());
    CHECK(reserved_of(pixel.value()).empty());
}

TEST("sgl emit - the native fill and vulkan read the draw's start as before, and list no slot of it (EMIT-128)")
{
    auto const source = vertex_of("@vertex_index v: int, @instance_index i: int", "(v as float) + (i as float)");
    for (auto const route : k_routes)
    {
        auto const e = dx12(source, "vs", route, {});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().dxil_shader_model == k_six_eight);
        CHECK(reserved_of(e.value()).empty());
        check_no_draw_start_slot(e.value());
        if (route == native_route::text)
        {
            CHECK(e.value().text.contains("SV_StartVertexLocation"));
            CHECK(!e.value().text.contains("sgl_draw_start"));
        }
        else
            CHECK((container_features(e.value().dxil) & k_feature_start_location) != 0);
    }
    auto const vulkan = sgl::compile_to_text({.source = source, .entry_point = "vs", .target = target::hlsl_vulkan});
    REQUIRE(vulkan.has_value());
    CHECK(vulkan.value().text.contains("[[vk::builtin(\"BaseVertex\")]]"));
    CHECK(!vulkan.value().text.contains("sgl_draw_start"));
    check_no_draw_start_slot(vulkan.value());
}

TEST("sgl emit - every target's compile under the root-constant fill lists the draw's start slot on dx12 alone "
     "(EMIT-271)")
{
    auto const source = vertex_of("@vertex_index v: int", "v as float");
    target const both[] = {target::hlsl_dx12, target::hlsl_vulkan};
    auto const all = sgl::compile_all_to_text({.source = source, .targets = both, .core = k_six_six_fills});
    REQUIRE(all.has_value()).dump("error", all.has_error() ? all.error() : cc::string());
    auto seen = 0;
    for (auto const& entry : all.value())
    {
        if (entry.entry_point != "vs")
            continue;
        REQUIRE(entry.text.has_value()).dump("error", entry.text.has_error() ? entry.text.error() : cc::string());
        ++seen;
        if (entry.target == target::hlsl_dx12)
            check_draw_start_slot(entry.text.value(), true);
        else
        {
            CHECK(reserved_of(entry.text.value()).empty());
            CHECK(entry.text.value().text.contains("BaseVertex"));
        }
    }
    CHECK(seen == 2);
}

TEST("sgl emit - a polyfill form under the root-constant fill of the draw's start is unsupported (EMIT-128)")
{
    constexpr auto source = cc::string_view("require geometry_shader\n"
                                            "\n"
                                            "struct staged:\n"
                                            "    @position position: hpos4\n"
                                            "\n"
                                            "@vertex fun corner_vs(@vertex_index i: int) -> staged:\n"
                                            "    return {position = hpos4(i as float, 0.0, 0.0, 1.0)}\n"
                                            "\n"
                                            "@geometry(max_vertices = 3)\n"
                                            "fun tri_gs(tri: staged[3], stream: mut triangle_stream[staged]):\n"
                                            "    for i in 0 ..< 3:\n"
                                            "        stream.emit(tri[i])\n");
    for (auto const form : {mesh_form::vertex_fetch, mesh_form::compute})
    {
        auto const refused = sgl::compile_to_text({.source = source,
                                                   .entry_point = "tri_gs",
                                                   .target = target::hlsl_dx12,
                                                   .core = k_six_six_fills,
                                                   .mesh = form,
                                                   .polyfill_vertex = "corner_vs"});
        REQUIRE(refused.has_error());
        auto const expected = cc::format("<sgl>: error: unsupported: the {} polyfill form of geometry entry point "
                                         "'tri_gs' under the root_constant fill of draw_start, which no polyfill form "
                                         "takes: sg draws them, indirectly too\n",
                                         form == mesh_form::vertex_fetch ? "vertex_fetch" : "compute");
        CHECK(refused.error() == expected);
        // the native fill writes either form, and vulkan has the one fill
        auto const native = sgl::compile_to_text({.source = source,
                                                  .entry_point = "tri_gs",
                                                  .target = target::hlsl_dx12,
                                                  .mesh = form,
                                                  .polyfill_vertex = "corner_vs"});
        CHECK(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    }
    // the pipeline's vertex stage drawn as itself takes the fill
    auto const vertex = dx12(source, "corner_vs", native_route::text);
    REQUIRE(vertex.has_value()).dump("error", vertex.has_error() ? vertex.error() : cc::string());
    check_draw_start_slot(vertex.value(), true);
}

TEST("sgl emit - an entry with an @inline block and the draw's start declares and lists both blocks of space 9 "
     "(EMIT-271)")
{
    constexpr auto source = cc::string_view("@inline binding tuning:\n"
                                            "    offset: int\n"
                                            "    scale: float\n"
                                            "\n"
                                            "struct varyings:\n"
                                            "    @position position: hpos4\n"
                                            "\n"
                                            "@vertex fun vs(@vertex_index v: int){tuning} -> varyings:\n"
                                            "    let x = ((v + tuning.offset) as float) * tuning.scale\n"
                                            "    return { position = hpos4(x, 0.0, 0.0, 1.0) }\n");
    for (auto const route : k_routes)
    {
        auto const e = dx12(source, "vs", route);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().dxil_shader_model == k_six_six);
        check_draw_start_slot(e.value(), true);
        // the program's block keeps its own entry, at its own place, and its own layout beside the draw start's
        auto inline_blocks = 0;
        for (auto const& b : e.value().bindings)
            if (b.is_inline)
            {
                ++inline_blocks;
                CHECK(!b.is_reserved);
                CHECK(b.reserved == sgl::reserved_slot::none);
                CHECK(b.group == -1);
                CHECK(b.slot == 0);
            }
        CHECK(inline_blocks == 1);
        CHECK(e.value().layouts.size() == 2);
        if (route == native_route::text)
        {
            auto const& text = e.value().text;
            CHECK(text.contains("register(b0, space9)")).dump("text", text);
            CHECK(text.contains("ConstantBuffer<sgl_draw_start_block> sgl_draw_start : register(b1, space9);"))
                .dump("text", text);
        }
    }
}

TEST("sgl emit - @vertex_index adds the first vertex as a uint and converts the sum, so a negative base vertex wraps "
     "(EMIT-128, EMIT-271)")
{
    // an indexed draw's base vertex is a signed INT the host writes as its bits; uint addition wraps, so
    // `int(SV_VertexID + first_vertex)` is the fetched index plus the base, negative bases included
    auto const e = dx12(vertex_of("@vertex_index v: int", "v as float"), "vs", native_route::text);
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& text = e.value().text;
    CHECK(text.contains("uint first_vertex;")).dump("text", text);
    auto const at = text.find(" + sgl_draw_start.first_vertex)");
    REQUIRE(at >= 0);
    // the addition is the argument of `int(...)`, of the uint semantic and the uint member
    auto const view = cc::string_view(text);
    auto const open = view.subview(cc::start_end{.start = 0, .end = at}).rfind("int(");
    REQUIRE(open >= 0);
    auto const name = view.subview(cc::offset_size{.offset = open + 4, .size = at - open - 4});
    CHECK(text.contains(cc::format("uint {} : SV_VertexID", name))).dump("name", name).dump("text", text);
}
