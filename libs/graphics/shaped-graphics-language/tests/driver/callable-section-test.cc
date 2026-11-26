#include "../check/check-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;

// CHK-562: a call of a callable is compiled at its table's place in the program's callable section.
// The section holds the tables of the modules the program reaches and of no other module, in an order the files'
// `module` and `use` lines and the modules' names fix.
// Every way a compile checks the program, and every module of the library it does not reach, must leave each place
// where it is, since the host builds its shader table from `describe`'s places.

namespace
{
// `seen` is used by `light`; `glow` has an option, which puts it late in the library's chain; `aside` is used by
// nothing.
// The program uses `light` and `glow`, so its section is glow's, seen's, light's and then its own: `glows` at 0,
// `lookups` at 1, `picks` at 2 and `own` at 4.
constexpr cc::string_view k_seen = R"(module seen
require raytracing_pipeline
struct seen_call:
    x: float
@callable fun lookup(c: mut seen_call):
    c.x = 1.0
callables lookups = (lookup)
fun via_seen() -> float:
    let mut c = seen_call(0.0)
    lookups[0](mut c)
    return c.x
)";

constexpr cc::string_view k_light = R"(module light
require raytracing_pipeline
use seen
struct light_call:
    x: float
@callable fun pick(c: mut light_call):
    c.x = 2.0
@callable fun restir(c: mut light_call):
    c.x = 3.0
callables picks = (pick, restir)
fun via_light() -> float:
    let mut c = light_call(0.0)
    picks[1](mut c)
    return c.x + seen.via_seen()
)";

constexpr cc::string_view k_glow = R"(module glow
require raytracing_pipeline
@option const boost = 1
struct glow_call:
    x: float
@callable fun glowing(c: mut glow_call):
    c.x = boost as float
callables glows = (glowing)
fun via_glow() -> float:
    let mut c = glow_call(0.0)
    glows[0](mut c)
    return c.x
)";

constexpr cc::string_view k_aside = R"(module aside
require raytracing_pipeline
struct aside_call:
    x: float
@callable fun aside_one(c: mut aside_call):
    c.x = 4.0
@callable fun aside_two(c: mut aside_call):
    c.x = 5.0
callables asides = (aside_one, aside_two)
)";

constexpr cc::string_view k_program = R"(require raytracing_pipeline
use light
use glow
struct own_call:
    x: float
struct probe:
    is_missed: bool
rays probe_rays:
    probe: probe
binding result:
    v: mut buffer[float]
@callable fun local(c: mut own_call):
    c.x = 6.0
callables own = (local)
@raygen fun gen(@launch_id id: int3){result}:
    let mut c = own_call(0.0)
    let x = light.via_light() + glow.via_glow()
    own[0](mut c)
    result.v[id.x] = x + c.x
@miss fun probe_miss(p: mut probe):
    p.is_missed = true
@raytracing pipeline path:
    rays = probe_rays
    raygen = gen
    miss.probe = probe_miss
)";

/// `names`, comma-separated.
cc::string joined(cc::span<cc::string const> names)
{
    auto result = cc::string();
    for (auto const& n : names)
        result.appendf("{}{}", result.empty() ? "" : ", ", n);
    return result;
}

/// The indices of `CallShader` in `text`, comma-separated in the order the text calls them; -1 for one that is not a
/// constant.
cc::string call_indices(cc::string_view text)
{
    auto result = cc::vector<cc::string>();
    constexpr auto head = cc::string_view("CallShader(uint(");
    for (auto at = text.find(head); at >= 0; at = text.find(head, at + 1))
    {
        auto value = 0;
        auto i = at + head.size();
        auto const start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
            value = value * 10 + (text[i++] - '0');
        result.push_back(cc::format("{}", i > start && i < text.size() && text[i] == ')' ? value : -1));
    }
    return joined(result);
}

/// `gen` compiled for dx12 behind `library`, as its `CallShader` indices.
cc::string indices_of(cc::span<sgl::library_file const> library,
                      cc::string_view program = k_program,
                      cc::span<sgl::check::option_value const> options = {})
{
    auto const r = sgl::compile_to_text({.source = program,
                                         .source_name = "program.sgl",
                                         .library = library,
                                         .entry_point = "gen",
                                         .target = sgl::emit::target::hlsl_dx12,
                                         .options = options});
    if (r.has_error())
        FAIL(r.error());
    return call_indices(r.value().text);
}

/// The section of `m` as each table's name, `module.table` for a module's, and its place.
cc::string section_of(sgl::check::checked_module const& m)
{
    auto result = cc::vector<cc::string>();
    for (auto const t : m.callable_tables)
        result.push_back(cc::format("{}@{}", m.qualified_name_of(t), m.callable_offset_of(t)));
    return joined(result);
}

/// Parsed library files, which a check takes by reference.
struct parsed_library
{
    cc::vector<cc::unique_ptr<sgl::parsed_file>> parsed;
    cc::vector<cc::unique_ptr<sgl::ast::file_ast>> built;
    cc::vector<sgl::check::module_file> files;

    explicit parsed_library(cc::span<cc::string_view const> sources)
    {
        for (auto const s : sources)
            add(s);
    }
    sgl::check::module_file add(cc::string_view source)
    {
        parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(source)));
        built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*parsed.back())));
        files.push_back({.file = *parsed.back(), .ast = *built.back()});
        return files.back();
    }
};

/// The section the program wants: glow's, seen's, light's, then its own.
constexpr auto k_section = cc::string_view("glow.glows@0, seen.lookups@1, light.picks@2, own@4");
/// What `gen` calls: light's restir (2 + 1), seen's lookup, glow's glowing, its own local.
constexpr auto k_calls = cc::string_view("3, 1, 0, 4");
} // namespace

TEST("sgl callable section - SGL-H1's two modules: a table of a module the program does not use takes no place")
{
    // `a` comes first in the library's chain by name, and the program reaches only `b`, so `tb[0]` is callable 0
    constexpr auto a = cc::string_view("module a\nrequire raytracing_pipeline\nstruct pa:\n    x: float\n"
                                       "@callable fun call_a(c: mut pa):\n    c.x = 1.0\ncallables ta = (call_a)\n");
    constexpr auto b = cc::string_view(R"(module b
require raytracing_pipeline
struct pb:
    x: float
@callable fun call_b(c: mut pb):
    c.x = 2.0
callables tb = (call_b)
fun via_b() -> float:
    let mut c = pb(0.0)
    tb[0](mut c)
    return c.x
)");
    constexpr auto program = cc::string_view(R"(require raytracing_pipeline
use b
binding result:
    v: mut buffer[float]
@raygen fun gen(@launch_id id: int3){result}:
    result.v[id.x] = b.via_b()
)");
    sgl::library_file const alone[] = {{.name = "b.sgl", .source = b}};
    sgl::library_file const beside[] = {{.name = "a.sgl", .source = a}, {.name = "b.sgl", .source = b}};
    CHECK(indices_of(alone, program) == "0");
    CHECK(indices_of(beside, program) == "0");

    auto const r = sgl::describe({.source = program, .source_name = "program.sgl", .library = beside});
    if (r.has_error())
        FAIL(r.error());
    REQUIRE(r.value().callables.size() == 1);
    CHECK(r.value().callables[0].name == "b.tb");
    CHECK(r.value().callables[0].offset == 0);
}

TEST("sgl callable section - a module's tables follow those of the modules it uses, and the program's come last")
{
    sgl::library_file const library[] = {{.name = "light.sgl", .source = k_light},
                                         {.name = "seen.sgl", .source = k_seen},
                                         {.name = "glow.sgl", .source = k_glow}};
    CHECK(indices_of(library) == k_calls);

    // the files' order in the library, and `aside` anywhere in it, change nothing
    sgl::library_file const shuffled[] = {{.name = "aside.sgl", .source = k_aside},
                                          {.name = "glow.sgl", .source = k_glow},
                                          {.name = "seen.sgl", .source = k_seen},
                                          {.name = "light.sgl", .source = k_light}};
    CHECK(indices_of(shuffled) == k_calls);
    sgl::library_file const aside_last[] = {{.name = "seen.sgl", .source = k_seen},
                                            {.name = "light.sgl", .source = k_light},
                                            {.name = "glow.sgl", .source = k_glow},
                                            {.name = "zz_aside.sgl", .source = k_aside}};
    CHECK(indices_of(aside_last) == k_calls);

    // nor does the order of the program's `use` lines
    auto swapped = cc::string(k_program);
    CHECK(swapped.replace_all("use light\nuse glow\n", "use glow\nuse light\n") == 1);
    CHECK(indices_of(shuffled, swapped) == k_calls);
}

TEST("sgl callable section - a compile that sets a module's option, re-making the chain from it, keeps every place")
{
    sgl::library_file const library[] = {{.name = "aside.sgl", .source = k_aside},
                                         {.name = "glow.sgl", .source = k_glow},
                                         {.name = "light.sgl", .source = k_light},
                                         {.name = "seen.sgl", .source = k_seen}};
    sgl::check::option_value const boosted[] = {{.name = "glow.boost", .value = "2"}};
    CHECK(indices_of(library, k_program, boosted) == k_calls);
    // and so does a library whose chain cannot be made, a file of it not parsing, which checks the reached modules alone
    sgl::library_file const broken[] = {{.name = "aside.sgl", .source = k_aside},
                                        {.name = "broken.sgl", .source = "module broken\nfun (\n"},
                                        {.name = "glow.sgl", .source = k_glow},
                                        {.name = "light.sgl", .source = k_light},
                                        {.name = "seen.sgl", .source = k_seen}};
    CHECK(indices_of(broken) == k_calls);
    CHECK(indices_of(broken, k_program, boosted) == k_calls);
}

TEST("sgl callable section - every way of checking the program makes the same section")
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    cc::string_view const sources[] = {k_seen, k_aside, k_light, k_glow};
    auto library = parsed_library(sources);
    auto const program = library.add(k_program);
    auto const files = cc::span<sgl::check::module_file const>(library.files).subspan({.offset = 0, .size = 4});

    // beside the modules it reaches, in the order their `use` lines reach them
    auto const beside = sgl::check::check(*checked, files, program);
    CHECK(beside.diagnostics.empty());
    CHECK(section_of(beside) == k_section);

    // behind a layer of the modules it reaches
    auto const reached = sgl::check::check_modules(*checked, files, program);
    REQUIRE(reached.has_value());
    auto const behind_reached = sgl::check::check(reached.value(), files, program);
    CHECK(behind_reached.diagnostics.empty());
    CHECK(section_of(behind_reached) == k_section);

    // behind the whole library's chain, which holds `aside` too and places `glow`, with its option, late
    auto const whole = sgl::check::check_library(*checked, files);
    REQUIRE(whole.has_value());
    auto const behind_whole = sgl::check::check(whole.value(), files, program);
    CHECK(behind_whole.diagnostics.empty());
    CHECK(section_of(behind_whole) == k_section);

    // the whole library's own layer reaches no module, so it has no section
    CHECK(whole.value().module().callable_tables.empty());
}

TEST("sgl callable section - a program of a library module joins its tables, after the module's library files")
{
    // CHK-346: the program's file is the last of its module's files, so its own table follows the library file's
    constexpr auto program = cc::string_view(R"(module light
require raytracing_pipeline
struct extra_call:
    x: float
binding result:
    v: mut buffer[float]
@callable fun more(c: mut extra_call):
    c.x = 7.0
callables extras = (more)
@raygen fun gen(@launch_id id: int3){result}:
    let mut c = extra_call(0.0)
    extras[0](mut c)
    result.v[id.x] = via_light() + c.x
)");
    sgl::library_file const library[] = {{.name = "aside.sgl", .source = k_aside},
                                         {.name = "light.sgl", .source = k_light},
                                         {.name = "seen.sgl", .source = k_seen}};
    // `extras` at 3, past seen's one and light's two; `via_light` calls restir at 2 and seen's lookup at 0
    CHECK(indices_of(library, program) == "3, 2, 0");

    // named apart from the module's library file, which a source of its own name would stand in for
    auto const r = sgl::describe({.source = program, .source_name = "light_extra.sgl", .library = library});
    if (r.has_error())
        FAIL(r.error());
    auto const& tables = r.value().callables;
    REQUIRE(tables.size() == 3);
    CHECK(tables[0].name == "seen.lookups");
    CHECK(tables[1].name == "picks");
    CHECK(tables[1].offset == 1);
    CHECK(tables[2].name == "extras");
    CHECK(tables[2].offset == 3);
}

TEST("sgl callable section - describe lists every table of the section with its place and its callables")
{
    sgl::library_file const library[] = {{.name = "aside.sgl", .source = k_aside},
                                         {.name = "glow.sgl", .source = k_glow},
                                         {.name = "light.sgl", .source = k_light},
                                         {.name = "seen.sgl", .source = k_seen}};
    auto const r = sgl::describe({.source = k_program, .source_name = "program.sgl", .library = library});
    if (r.has_error())
        FAIL(r.error());
    auto const& d = r.value();

    // a module's table and callables by the names the program gives them
    REQUIRE(d.callables.size() == 4);
    CHECK(d.callables[0].name == "glow.glows");
    CHECK(d.callables[0].offset == 0);
    CHECK(joined(d.callables[0].entries) == "glow.glowing");
    CHECK(d.callables[0].parameter == "glow.glow_call");
    CHECK(d.callables[1].name == "seen.lookups");
    CHECK(d.callables[1].offset == 1);
    CHECK(joined(d.callables[1].entries) == "seen.lookup");
    CHECK(d.callables[2].name == "light.picks");
    CHECK(d.callables[2].offset == 2);
    CHECK(joined(d.callables[2].entries) == "light.pick, light.restir");
    CHECK(d.callables[3].name == "own");
    CHECK(d.callables[3].offset == 4);
    CHECK(joined(d.callables[3].entries) == "local");

    // the pipeline holds the section in the same order, and freezes it
    REQUIRE(d.raytracing_pipelines.size() == 1);
    auto const& p = d.raytracing_pipelines[0];
    CHECK(joined(p.callables) == "glow.glowing, seen.lookup, light.pick, light.restir, local");
    CHECK(p.frozen.values().any(
        [](cc::string const& line)
        { return line == "callables = glow.glowing, seen.lookup, light.pick, light.restir, local"; }));
}

TEST("sgl callable section - the host's callables follow the section's last table, whatever the library holds")
{
    auto const host_light = [&]
    {
        auto source = cc::string(k_light);
        CHECK(source.replace_all("callables picks = (pick, restir)", "callables picks = (pick, restir, .host)") == 1);
        return source;
    }();
    // the program reaches `light` alone, so its table is the section's last, whatever `aside` declares
    constexpr auto program = cc::string_view(R"(require raytracing_pipeline
use light
binding result:
    v: mut buffer[float]
@raygen fun gen(@launch_id id: int3){result}:
    result.v[id.x] = light.via_light()
)");
    sgl::library_file const library[] = {{.name = "light.sgl", .source = host_light},
                                         {.name = "seen.sgl", .source = k_seen},
                                         {.name = "zz_aside.sgl", .source = k_aside}};
    CHECK(indices_of(library, program) == "2, 0");
    cc::string_view const sources[] = {host_light, k_seen, k_aside};
    CHECK(reports_with(sources, program) == "");

    // a table of the program's own follows it, so the host's would land past it
    auto const more = cc::string(program)
                    + "struct own_call:\n    x: float\n@callable fun local(c: mut own_call):\n"
                      "    c.x = 6.0\ncallables own = (local)\n";
    auto const r
        = sgl::compile_to_text({.source = more, .source_name = "program.sgl", .library = library, .entry_point = "gen"});
    REQUIRE(r.has_error());
    CHECK(r.error().contains("picks takes the host's callables, so it is the last table of the program's callable "
                             "section"));
}

TEST("sgl callable section - a module of two files, and a program whose module sorts first, keep every place")
{
    // `light` is two files, given in an order no name gives: its tables follow the library's order of its files
    constexpr auto light_more = cc::string_view(R"(module light
require raytracing_pipeline
struct more_call:
    x: float
@callable fun more(c: mut more_call):
    c.x = 8.0
callables mores = (more)
)");
    // the program names a module of its own whose name sorts before every other, and still comes last
    auto const program_source = cc::string("module aaa\n") + cc::string(k_program);
    cc::string_view const sources[] = {light_more, k_seen, k_aside, k_light, k_glow};
    auto library = parsed_library(sources);
    auto const program = library.add(program_source);
    auto const files = cc::span<sgl::check::module_file const>(library.files).subspan({.offset = 0, .size = 5});
    constexpr auto section = cc::string_view("glow.glows@0, seen.lookups@1, light.mores@2, light.picks@3, own@5");

    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const beside = sgl::check::check(*checked, files, program);
    CHECK(beside.diagnostics.empty());
    CHECK(section_of(beside) == section);

    auto const reached = sgl::check::check_modules(*checked, files, program);
    REQUIRE(reached.has_value());
    auto const behind_reached = sgl::check::check(reached.value(), files, program);
    CHECK(behind_reached.diagnostics.empty());
    CHECK(section_of(behind_reached) == section);

    auto const whole = sgl::check::check_library(*checked, files);
    REQUIRE(whole.has_value());
    auto const behind_whole = sgl::check::check(whole.value(), files, program);
    CHECK(behind_whole.diagnostics.empty());
    CHECK(section_of(behind_whole) == section);
}
