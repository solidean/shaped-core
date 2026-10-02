#include "../check/check-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::emit::target;

// CHK-353 to CHK-356: an option is set per compile, and a branch on a constant leaves the text with the side it skips.

namespace
{
constexpr cc::string_view k_shifts = "@option const shift = 3\n"
                                     "@option const wide = false\n"
                                     "@option const unused = 1\n"
                                     "\n"
                                     "binding work:\n"
                                     "    values: mut buffer[int]\n"
                                     "    wide_values: buffer[int]\n"
                                     "\n"
                                     "fun widened(i: int){work} -> int:\n"
                                     "    if wide:\n"
                                     "        return work.wide_values[i]\n"
                                     "    return 1\n"
                                     "\n"
                                     "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                                     "    work.values[id.x] = widened(id.x) << shift\n"
                                     "\n"
                                     "test shift == 3\n";

cc::result<sgl::emitted_source, cc::string> compiled(cc::span<sgl::check::option_value const> options,
                                                     target t = target::hlsl_dx12)
{
    return sgl::compile_to_text({.source = k_shifts, .entry_point = "main_cs", .target = t, .options = options});
}

cc::string footprint_of(sgl::emitted_source const& e)
{
    return sgl::check::footprint_text(e.footprint);
}
} // namespace

TEST("sgl options - a compile without values takes every default, and names the options its entry point reaches")
{
    auto const text = compiled({});
    REQUIRE(text.has_value());
    // `unused` is named by nothing the entry point reaches, so it multiplies nothing (CHK-355)
    REQUIRE(text.value().options.size() == 2);
    CHECK(text.value().options[0] == "shift");
    CHECK(text.value().options[1] == "wide");
    CHECK(footprint_of(text.value()) == "work.values: write");
}

TEST("sgl options - each set of values is a text of its own, and a branch on one leaves what only it used")
{
    sgl::check::option_value const narrow[] = {{.name = "shift", .value = "5"}};
    sgl::check::option_value const wide[] = {{.name = "shift", .value = "5"}, {.name = "wide", .value = "true"}};
    for (auto const t : sgl::emit::all_targets())
    {
        auto const defaults = compiled({}, t);
        auto const a = compiled(narrow, t);
        auto const b = compiled(wide, t);
        REQUIRE(defaults.has_value());
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(defaults.value().text != a.value().text);
        CHECK(a.value().text != b.value().text);
        // the binding's slots are declared either way, and only a read of one is a use (CHK-356)
        CHECK(footprint_of(a.value()) == "work.values: write");
        CHECK(footprint_of(b.value()) == "work.values: write, work.wide_values: read");
    }
}

TEST("sgl options - every constant verdict is judged for the values the compile gave")
{
    // CHK-270 judges the shift by the option's value, which is fine at its default
    sgl::check::option_value const far[] = {{.name = "shift", .value = "40"}};
    auto const refused = compiled(far);
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("shift-out-of-range"));
    sgl::check::option_value const negative[] = {{.name = "shift", .value = "-1"}};
    CHECK(compiled(negative).has_error());
}

TEST("sgl options - a value for no option, or of another type than the option's, is invalid-option")
{
    sgl::check::option_value const unknown[] = {{.name = "shfit", .value = "4"}};
    auto const misspelled = compiled(unknown);
    REQUIRE(misspelled.has_error());
    CHECK(misspelled.error().contains("invalid-option"));
    CHECK(misspelled.error().contains("shfit"));

    for (auto const& [name, value] : {cc::pair<cc::string_view, cc::string_view>{"shift", "true"},
                                      {"shift", "4.0"},
                                      {"shift", ""},
                                      {"wide", "1"},
                                      {"wide", ".maybe"}})
    {
        sgl::check::option_value const given[] = {{.name = cc::string(name), .value = cc::string(value)}};
        auto const r = compiled(given);
        REQUIRE(r.has_error());
        CHECK(r.error().contains("invalid-option")).dump("name", name).dump("value", value);
    }

    // a bool and an enum case take their leading dot or none
    sgl::check::option_value const dotted[] = {{.name = "wide", .value = ".true"}};
    CHECK(compiled(dotted).has_value());
}

TEST("sgl options - the tests of a compile run with every option at its default")
{
    sgl::check::option_value const moved[] = {{.name = "shift", .value = "4"}};
    auto const r
        = sgl::compile_to_text({.source = k_shifts, .entry_point = "main_cs", .options = moved, .run_tests = true});
    CHECK(r.has_value());
}

TEST("sgl options - a workgroup size and an image's format may name an option, and describe says what reaches what")
{
    constexpr auto source = "@option const tile = 8\n"
                            "@option const output_format: pixel_format = .rgba16_float\n"
                            "\n"
                            "binding outputs:\n"
                            "    image: out image_2d[output_format]\n"
                            "\n"
                            "@compute(tile, tile) fun fill(@thread_id id: int3){outputs}:\n"
                            "    outputs.image.store(id.xy, float4(1.0, 0.0, 0.0, 1.0))\n";
    sgl::check::option_value const given[]
        = {{.name = "tile", .value = "16"}, {.name = "output_format", .value = ".rgba8_unorm"}};
    auto const r
        = sgl::compile_to_text({.source = source, .entry_point = "fill", .target = target::wgsl, .options = given});
    REQUIRE(r.has_value());
    CHECK(r.value().workgroup[0] == 16);
    CHECK(r.value().text.contains("@workgroup_size(16, 16, 1)"));
    CHECK(r.value().text.contains("texture_storage_2d<rgba8unorm, write>"));
    REQUIRE(r.value().bindings.size() == 1);
    CHECK(r.value().bindings[0].image_format == "rgba8_unorm");

    auto const described = sgl::describe({.source = source});
    REQUIRE(described.has_value());
    auto const& d = described.value();
    REQUIRE(d.options.size() == 2);
    CHECK(d.options[0].name == "tile");
    CHECK(d.options[0].type == "int");
    CHECK(d.options[0].value == "8");
    CHECK(d.options[1].type == "pixel_format");
    CHECK(d.options[1].value == ".rgba16_float");
    REQUIRE(d.entry_points.size() == 1);
    REQUIRE(d.entry_points[0].options.size() == 2);
    CHECK(d.entry_points[0].options[0] == "tile");
    CHECK(d.entry_points[0].options[1] == "output_format");
    REQUIRE(d.bindings.size() == 1);
    REQUIRE(d.bindings[0].options.size() == 1);
    CHECK(d.bindings[0].options[0] == "output_format");
    // the member says which option its format is, so a host takes that format at run time
    CHECK(d.bindings[0].members[0].format_option == "output_format");
    CHECK(d.bindings[0].members[0].count_option == "");
}

TEST("sgl options - a binding array's length that names an option is the member's count option")
{
    constexpr auto source = "require binding_arrays\n"
                            "\n"
                            "@option const layers = 3\n"
                            "\n"
                            "binding inputs:\n"
                            "    plain: texture_2d[float4]\n"
                            "    layered: texture_2d[float4][layers]\n"
                            "\n"
                            "@pixel struct target:\n"
                            "    color: float4\n"
                            "\n"
                            "struct pixel_input:\n"
                            "    @position position: hpos4\n"
                            "\n"
                            "@pixel fun main_ps(p: pixel_input){inputs} -> target:\n"
                            "    return {color = inputs.layered[1].load(int2(0, 0)) + inputs.plain.load(int2(0, 0))}\n";
    auto const described = sgl::describe({.source = source});
    REQUIRE(described.has_value());
    auto const& d = described.value();
    REQUIRE(d.bindings.size() == 1);
    CHECK(d.bindings[0].members[0].count_option == "");
    CHECK(d.bindings[0].members[1].count_option == "layers");
    CHECK(d.bindings[0].members[1].count == 3);
    CHECK(d.bindings[0].members[1].format_option == "");
}

TEST("sgl options - an option of a pipeline's stage is the pipeline's, and one only a dead branch names still counts")
{
    // CHK-355: what the entry point names is one set whatever the values, so `nested` counts while `outer` is false
    constexpr auto source = "@option const outer = false\n"
                            "@option const nested = false\n"
                            "\n"
                            "struct vertex_out:\n"
                            "    @position position: hpos4\n"
                            "\n"
                            "@pixel struct target:\n"
                            "    @format(.host) color: float4\n"
                            "\n"
                            "fun tone() -> float:\n"
                            "    if outer:\n"
                            "        if nested:\n"
                            "            return 1.0\n"
                            "    return 0.5\n"
                            "\n"
                            "@vertex struct corner:\n"
                            "    position: pos3\n"
                            "\n"
                            "@vertex fun main_vs(c: corner) -> vertex_out:\n"
                            "    return {position = hpos4(0.0, 0.0, 0.0, 1.0)}\n"
                            "\n"
                            "@pixel fun main_ps(v: vertex_out) -> target:\n"
                            "    let t = tone()\n"
                            "    return {color = float4(t, t, t, 1.0)}\n"
                            "\n"
                            "pipeline draw:\n"
                            "    vertex = main_vs\n"
                            "    pixel = main_ps\n";
    auto const described = sgl::describe({.source = source});
    REQUIRE(described.has_value());
    auto const& d = described.value();
    REQUIRE(d.entry_points.size() == 2);
    CHECK(d.entry_points[0].options.empty());
    REQUIRE(d.entry_points[1].options.size() == 2);
    REQUIRE(d.pipelines.size() == 1);
    REQUIRE(d.pipelines[0].options.size() == 2);
    CHECK(d.pipelines[0].options[0] == "outer");
    CHECK(d.pipelines[0].options[1] == "nested");
}

TEST("sgl options - an option is a bool, an int or an enum case, at file scope")
{
    CHECK(reports_for("@option const blend = 0.5\n").contains("unsupported-yet"));
    CHECK(reports_for("@option(1) const tile = 8\n").contains("invalid-attribute-arguments"));
    // a leading dot is resolved against the written type, and needs one
    CHECK(reports_for("@option const f = .rgba8_unorm\n").contains("unsupported-yet"));
    CHECK(reports_for("@option const f: pixel_format = .no_such_format\n").contains("unknown-member"));
    CHECK(reports_for("@option const f: pixel_format = .rgba8_unorm\n") == "");
    // an image takes a const whose value is an image format, and no other
    CHECK(reports_for("const n = 3\n\nbinding b:\n    image: out image_2d[n]\n").contains("wrong-kind-of-name"));
}
