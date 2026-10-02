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

TEST("sgl options - an option of the program reaches a function of a module it uses, at the value the compile gave")
{
    constexpr auto module_source = "module scaling\n"
                                   "\n"
                                   "fun shifted(x: int, by: int) -> int => x << by\n";
    constexpr auto source = "use scaling\n"
                            "\n"
                            "@option const shift = 3\n"
                            "\n"
                            "binding work:\n"
                            "    values: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun main_cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = scaling.shifted(id.x, shift)\n";
    sgl::library_file const library[] = {{.name = "modules/scaling.sgl", .source = module_source}};
    sgl::check::option_value const moved[] = {{.name = "shift", .value = "5"}};
    for (auto const t : sgl::emit::all_targets())
    {
        auto const compile = [&](cc::span<sgl::check::option_value const> options)
        {
            return sgl::compile_to_text(
                {.source = source, .library = library, .entry_point = "main_cs", .target = t, .options = options});
        };
        auto const defaults = compile({});
        auto const given = compile(moved);
        REQUIRE(defaults.has_value());
        REQUIRE(given.has_value());
        CHECK(defaults.value().text != given.value().text);
        REQUIRE(given.value().options.size() == 1);
        CHECK(given.value().options[0] == "shift");
        REQUIRE(given.value().library_files.size() == 1);
        CHECK(given.value().library_files[0] == "modules/scaling.sgl");
    }

    // a name the program has no option of is refused with the library in place as without it
    sgl::check::option_value const unknown[] = {{.name = "by", .value = "5"}};
    auto const refused
        = sgl::compile_to_text({.source = source, .library = library, .entry_point = "main_cs", .options = unknown});
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("invalid-option"));

    auto const described = sgl::describe({.source = source, .options = moved, .library = library});
    REQUIRE(described.has_value());
    REQUIRE(described.value().entry_points.size() == 1);
    REQUIRE(described.value().entry_points[0].options.size() == 1);
    CHECK(described.value().entry_points[0].options[0] == "shift");
}

TEST("sgl options - a module's option is set by its qualified name, apart from a program option of its own name")
{
    constexpr auto module_source = "module common\n"
                                   "\n"
                                   "@option const taps = 4\n"
                                   "\n"
                                   "fun weight() -> float => 1.0 / (taps as float)\n";
    constexpr auto source = "use common\n"
                            "\n"
                            "@option const taps = 2\n"
                            "\n"
                            "binding res:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){res}:\n"
                            "    res.values[id.x] = common.weight() * (taps as float)\n";
    sgl::library_file const library[] = {{.name = "modules/common.sgl", .source = module_source}};
    auto const compile = [&](cc::span<sgl::check::option_value const> options)
    {
        return sgl::compile_to_text(
            {.source = source, .library = library, .entry_point = "cs", .target = target::wgsl, .options = options});
    };
    sgl::check::option_value const own[] = {{.name = "taps", .value = "8"}};
    sgl::check::option_value const module[] = {{.name = "common.taps", .value = "8"}};
    auto const defaults = compile({});
    auto const by_own = compile(own);
    auto const by_module = compile(module);
    REQUIRE(defaults.has_value());
    REQUIRE(by_own.has_value());
    REQUIRE(by_module.has_value());
    CHECK(defaults.value().text != by_own.value().text);
    CHECK(defaults.value().text != by_module.value().text);
    CHECK(by_own.value().text != by_module.value().text);
    // the module's file is checked ahead of the program, so its option comes first
    REQUIRE(by_module.value().options.size() == 2);
    CHECK(by_module.value().options[0] == "common.taps");
    CHECK(by_module.value().options[1] == "taps");

    auto const described = sgl::describe({.source = source, .options = module, .library = library});
    REQUIRE(described.has_value());
    auto const& d = described.value();
    REQUIRE(d.options.size() == 2);
    CHECK(d.options[0].name == "common.taps");
    CHECK(d.options[0].value == "8");
    CHECK(d.options[1].name == "taps");
    CHECK(d.options[1].value == "2");
    REQUIRE(d.entry_points.size() == 1);
    REQUIRE(d.entry_points[0].options.size() == 2);
    CHECK(d.entry_points[0].options[0] == "common.taps");
}

TEST("sgl options - a module's option given by its bare name is refused, naming the name that sets it")
{
    constexpr auto module_source = "module common\n"
                                   "\n"
                                   "@option const taps = 4\n"
                                   "\n"
                                   "fun weight() -> float => 1.0 / (taps as float)\n";
    constexpr auto source = "use common\n"
                            "\n"
                            "binding res:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){res}:\n"
                            "    res.values[id.x] = common.weight()\n";
    sgl::library_file const library[] = {{.name = "modules/common.sgl", .source = module_source}};
    sgl::check::option_value const bare[] = {{.name = "taps", .value = "8"}};
    auto const refused
        = sgl::compile_to_text({.source = source, .library = library, .entry_point = "cs", .options = bare});
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("invalid-option"));
    CHECK(refused.error().contains("common.taps"));
}

TEST("sgl options - a name given twice is invalid-option, and the most negative int is a value")
{
    sgl::check::option_value const twice[] = {{.name = "wide", .value = "true"}, {.name = "wide", .value = "false"}};
    auto const repeated = compiled(twice);
    REQUIRE(repeated.has_error());
    CHECK(repeated.error().contains("invalid-option"));
    CHECK(repeated.error().contains("more than once"));

    constexpr auto source = "@option const bias = 0\n";
    sgl::check::option_value const lowest[] = {{.name = "bias", .value = "-2147483648"}};
    auto const described = sgl::describe({.source = source, .options = lowest});
    REQUIRE(described.has_value());
    REQUIRE(described.value().options.size() == 1);
    CHECK(described.value().options[0].value == "-2147483648");
    for (auto const value : {cc::string_view("-2147483649"), cc::string_view("2147483648"), cc::string_view("--1")})
    {
        sgl::check::option_value const given[] = {{.name = "bias", .value = cc::string(value)}};
        auto const r = sgl::describe({.source = source, .options = given});
        REQUIRE(r.has_error());
        CHECK(r.error().contains("invalid-option")).dump("value", value);
    }
}

TEST("sgl options - a struct an entry point names reaches the options its fields name")
{
    // CHK-355: a struct sizes its array by an option, so the text follows that option wherever an entry point names
    // the struct: through a binding's member, a local or a parameter alike
    constexpr auto source = "@option const tile = 8\n"
                            "@option const width = 2\n"
                            "\n"
                            "struct scratch_rows:\n"
                            "    rows: float[tile]\n"
                            "\n"
                            "struct pair:\n"
                            "    values: float[width]\n"
                            "\n"
                            "@workgroup binding scratch:\n"
                            "    data: scratch_rows\n"
                            "\n"
                            "binding res:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "fun first(p: pair) -> float => p.values[0]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){res, scratch}:\n"
                            "    scratch.data.rows[0] = res.values[id.x]\n"
                            "    workgroup_barrier()\n"
                            "    res.values[id.x] = scratch.data.rows[0]\n"
                            "\n"
                            "@compute(64) fun by_parameter(@thread_id id: int3){res}:\n"
                            "    let p: pair = {values = [1.0, 2.0]}\n"
                            "    res.values[id.x] = first(p)\n";
    auto const described = sgl::describe({.source = source});
    REQUIRE(described.has_value());
    auto const& d = described.value();
    REQUIRE(d.entry_points.size() == 2);
    REQUIRE(d.entry_points[0].options.size() == 1);
    CHECK(d.entry_points[0].options[0] == "tile");
    REQUIRE(d.entry_points[1].options.size() == 1);
    CHECK(d.entry_points[1].options[0] == "width");
}

TEST("sgl options - an option whose value names another option is unsupported-yet")
{
    CHECK(reports_for("@option const base = 8\n@option const tile = base\n").contains("unsupported-yet"));
    // a plain const naming an option follows it, which is how a helper reads one
    CHECK(reports_for("@option const base = 8\nconst tile = base\n") == "");
}

TEST("sgl options - a binding's shape follows its layout rule, which moves every offset the host writes")
{
    auto const shape_of = [](cc::string_view layout) -> cc::string
    {
        auto const source = cc::string(layout)
                          + "binding params:\n"
                            "    a: float\n"
                            "    d: float4\n"
                            "\n"
                            "@compute(1) fun cs(){params}:\n"
                            "    let x = params.a + params.d.x\n";
        auto const described = sgl::describe({.source = source});
        if (!described.has_value() || described.value().bindings.size() != 1)
            return "";
        return described.value().bindings[0].shape;
    };
    auto const cpp = shape_of("@layout(.cpp)\n");
    auto const hlsl = shape_of("@layout(.hlsl)\n");
    auto const none = shape_of("");
    REQUIRE(!cpp.empty());
    REQUIRE(!hlsl.empty());
    REQUIRE(!none.empty());
    CHECK(cpp != hlsl);
    CHECK(cpp != none);
    CHECK(hlsl != none);
}
