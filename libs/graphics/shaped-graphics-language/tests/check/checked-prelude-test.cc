#include "check-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/time.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics-language/legalize/legalize.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>
#include <shaped-graphics-language/test/run_tests.hh>

// clean-core has no directory listing yet.
#include <nexus/source_root.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// A program checked behind the checked prelude is the program checked together with the prelude.
// Everything a caller observes of the two modules is compared, which is all of it but the ids.

using namespace sgl_test;

namespace
{
struct program
{
    cc::string name;
    cc::string source;
};

/// Every `*.sgl` under `root`, in a stable order, each named `label/` and its path below `root`.
void add_files_under(cc::vector<program>& into, cc::string_view label, cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<program>();
    auto ec = std::error_code();
    auto const base = fs::path(root.data(), root.data() + root.size());
    // the name below `root` by its text: emscripten's fs::relative gives nothing for a root with a drive letter
    auto const prefix = base.generic_string() + "/";
    for (auto const& e : fs::recursive_directory_iterator(base, ec))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
        {
            auto const full = e.path().generic_string();
            auto const below = full.starts_with(prefix) ? full.substr(prefix.size()) : full;
            found.push_back({.name = cc::format("{}/{}", label, below.c_str()),
                             .source = read_text(cc::string(e.path().string().c_str()))});
        }
    cc::sort(found, [](program const& a, program const& b) { return a.name < b.name; });
    for (auto& p : found)
        into.push_back(cc::move(p));
}

/// Programs that report, each touching a step that joins the program to the prelude.
constexpr cc::string_view reporting_programs[] = {
    // a duplicate, and a redeclaration by parameters
    "fun f(x: float) -> float => x\nfun f(x: float) -> float => x\nstruct f:\n    a: float\n",
    // hiding a prelude name the prelude seals
    "fun dot(a: int) -> int => a\nstruct int3:\n    x: int\n",
    // an extension of a prelude type, one of them clashing with a member it has
    "fun float3.twice(self) -> float3 => self * 2.0\nfun float3.twice(self) -> float3 => self\nfun float3.x(self) -> "
    "float => 1.0\n",
    // recursion, a cycle and a wide literal
    "fun a(x: int) -> int => b(x)\nfun b(x: int) -> int => a(x)\nfun c() => c()\nfun d() -> int => 4000000000\n",
    // an unknown name, a mismatch, and a test that fails to check
    "fun h() -> float => nothing_here + 1\nfun k() -> float => true\ntest undefined_name\n",
    // an entry point that needs a feature it does not declare, and an unused require
    "binding b:\n    t: acceleration_structure\n@compute fun run(){b}:\n    require geometry_shader\n    let x = 1\n",
};

cc::vector<program> written_programs()
{
    auto out = cc::vector<program>();
    for (auto i = isize(0); i < isize(sizeof(reporting_programs) / sizeof(reporting_programs[0])); ++i)
        out.push_back({.name = cc::format("reporting program {}", i), .source = cc::string(reporting_programs[i])});
    return out;
}

/// What a caller can observe of `m` that no id shows through: diagnostics, entry points, their text, and the tests.
cc::string observed(sgl::check::checked_module const& m, cc::span<sgl::check::module_file const> files)
{
    auto out = cc::string();
    auto const name_of = [&](sgl::i32 file) { return cc::format("file{}", file); };
    for (auto const& d : m.diagnostics)
    {
        out += sgl::format_diagnostic(name_of(d.file), files[d.file].file.source, d.what, d.detail);
        out += "\n";
        for (auto const& n : d.notes)
            out += sgl::format_note(name_of(n.file), files[n.file].file.source, n.where, n.message) + "\n";
    }
    for (auto const& e : m.entry_points)
    {
        out.appendf("entry point {}: {}, features {}, workgroup {} {} {}\n", e.name,
                    sgl::check::stage_name(e.entry_stage), e.features.bits, e.workgroup[0], e.workgroup[1],
                    e.workgroup[2]);
        // legalize is a function of its arguments, and every target but wgsl takes the default options
        auto const native = sgl::check::legalize(m, e);
        auto const emulated = sgl::check::legalize(m, e, {.is_emulated = true});
        out.appendf("footprint {}\n", sgl::check::footprint_text(sgl::check::footprint_of(m, native)));
        for (auto const target : sgl::emit::all_targets())
        {
            auto const& legal = target == sgl::emit::target::wgsl ? emulated : native;
            auto const emitted = sgl::emit::emit_entry_point(m, legal, target);
            out.appendf("{} as {}:\n{}\n", sgl::emit::to_string(target), emitted.entry_point, emitted.text);
            for (auto const& error : emitted.errors)
                out.appendf("error {}: {}\n", sgl::emit::to_string(error.kind), error.detail);
        }
    }
    for (auto const& r : sgl::test::run_tests(m, files))
    {
        auto const& t = m.tests[r.test];
        out.appendf("test at {}:{} ({}): {}, {} checks\n", t.file, t.where.offset, t.scope_path,
                    sgl::test::to_string(r.status), r.checks_run);
    }
    return out;
}

/// Every place a program's check changed the prelude's part of a table: `functions[12]`, or `symbols: shorter` where it
/// dropped entries.
/// Empty is what sharing the prelude needs (the compilation model's checked layers): today a compile copies it instead.
cc::vector<cc::string> prelude_writes(sgl::check::checked_module const& prelude, sgl::check::checked_module const& m)
{
    auto out = cc::vector<cc::string>();
    auto const compare = [&](cc::string_view name, auto const& before, auto const& after)
    {
        if (after.size() < before.size())
        {
            out.push_back(cc::format("{}: shorter", name));
            return;
        }
        for (auto i = isize(0); i < before.size(); ++i)
            if (!(before[i] == after[i]))
                out.push_back(cc::format("{}[{}]", name, i));
    };
    compare("symbols", prelude.symbols, m.symbols);
    compare("types", prelude.types, m.types);
    compare("members", prelude.members, m.members);
    compare("enum_cases", prelude.enum_cases, m.enum_cases);
    compare("functions", prelude.functions, m.functions);
    compare("parameters", prelude.parameters, m.parameters);
    compare("bindings", prelude.bindings, m.bindings);
    compare("binding_lists", prelude.binding_lists, m.binding_lists);
    compare("type_lists", prelude.type_lists, m.type_lists);
    compare("samplers", prelude.samplers, m.samplers);
    compare("pipelines", prelude.pipelines, m.pipelines);
    compare("pipeline_settings", prelude.pipeline_settings, m.pipeline_settings);
    compare("constants", prelude.constants, m.constants);
    compare("option_uses", prelude.option_uses, m.option_uses);
    compare("call_records", prelude.call_records, m.call_records);
    compare("written_arguments", prelude.written_arguments, m.written_arguments);
    compare("call_slots", prelude.call_slots, m.call_slots);
    compare("near_misses", prelude.near_misses, m.near_misses);
    compare("ray_traces", prelude.ray_traces, m.ray_traces);
    compare("callable_calls", prelude.callable_calls, m.callable_calls);
    compare("texel_stores", prelude.texel_stores, m.texel_stores);
    compare("files", prelude.files, m.files);
    compare("tests", prelude.tests, m.tests);
    compare("test_units", prelude.test_units, m.test_units);
    return out;
}

/// The library's prelude files, as `check_prelude` takes them.
cc::vector<sgl::check::module_file> prelude_modules()
{
    auto files = cc::vector<sgl::check::module_file>();
    for (auto const& f : sgl::parsed_prelude())
        files.push_back({.file = f.file, .ast = f.ast});
    return files;
}

constexpr cc::string_view artifact_key = "the key of this test's artifacts";

/// The checked prelude written as an artifact, once.
cc::vector<byte> const& prelude_artifact()
{
    static auto const bytes = []
    {
        auto saved = sgl::check::save_artifact(*sgl::checked_prelude(), artifact_key);
        CC_ASSERT(saved.has_value(), "the library's prelude is a layer an artifact holds");
        return cc::move(saved.value());
    }();
    return bytes;
}

/// The checked prelude read back from its artifact, once, as a process that never checked it would have it.
sgl::check::checked_prelude const& loaded_prelude()
{
    static auto const loaded = []
    {
        auto layer = sgl::check::load_artifact(prelude_artifact(), artifact_key, prelude_modules(),
                                               sgl::builtins::default_registry());
        CC_ASSERT(layer.has_value(), "an artifact reads back under its own key");
        return cc::move(layer.value());
    }();
    return loaded;
}
} // namespace

TEST("sgl checked prelude - the library's prelude checks clean, so every compile continues from it")
{
    CHECK(sgl::checked_prelude() != nullptr);
}

INVOCABLE_TEST("sgl checked prelude - a program behind it checks as it does with the prelude checked beside it",
               (program const& p))
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const file = sgl::parse(p.source);
    auto const ast = sgl::ast::build(file);
    auto files = cc::vector<sgl::check::module_file>();
    for (auto const& f : sgl::parsed_prelude())
        files.push_back({.file = f.file, .ast = f.ast});
    auto const user = sgl::check::module_file{.file = file, .ast = ast};

    auto const together = sgl::check::check(files, user);
    auto const behind = sgl::check::check(*checked, user);
    // the compilation model's artifacts: a layer read back is the layer, so a program behind it checks to the same ids
    auto const behind_loaded = sgl::check::check(loaded_prelude(), user);
    CHECK(behind_loaded == behind);
    files.push_back(user);
    CHECK(observed(behind, files) == observed(together, files));
    CHECK(observed(behind_loaded, files) == observed(behind, files));
    if (p.name.starts_with("reporting"))
        CHECK(!together.diagnostics.empty());
    CHECK(behind.symbols.size() == together.symbols.size());
    CHECK(behind.types.size() == together.types.size());
    CHECK(behind.tests.size() == together.tests.size());

    // the compilation model's checked layers share the prelude, which needs a program's check to leave its part alone
    CHECK(checked->module().functions.size() > 0);
    auto written = cc::string();
    for (auto const& w : prelude_writes(checked->module(), behind))
        written.appendf("{} ", w);
    CHECK(written == "");
}

// Three drivers rather than one, so the larger sets check in parallel.
TEST("sgl checked prelude - the corpus files, every fourth by default")
{
    auto files = cc::vector<program>();
    add_files_under(files, "corpus", nx::source_path(SGL_CORPUS_DIR));
    REQUIRE(files.size() > 0);
    // a default run takes every fourth file
    auto const stride = nx::is_thorough() ? isize(1) : isize(4);
    for (auto i = isize(0); i < files.size(); i += stride)
        nx::invoke_tests(files[i].name, files[i]);
}

TEST("sgl checked prelude - the samples and sg shaders, every third by default")
{
    auto files = cc::vector<program>();
    add_files_under(files, "samples", nx::source_path(SGL_SAMPLES_DIR));
    REQUIRE(files.size() > 0);
    auto const samples = files.size();
    add_files_under(files, "sg", nx::source_path(SGL_SG_SHADERS_DIR));
    REQUIRE(files.size() > samples);
    // A default run takes every third file.
    auto const stride = nx::is_thorough() ? isize(1) : isize(3);
    auto visited_sg = isize(0);
    for (auto i = isize(0); i < files.size(); i += stride)
    {
        nx::invoke_tests(files[i].name, files[i]);
        if (i >= samples)
            ++visited_sg;
    }
    REQUIRE(visited_sg > 0);
}

TEST("sgl checked prelude - programs written to report")
{
    for (auto const& p : written_programs())
        nx::invoke_tests(p.name, p);
}

namespace
{
/// A program that reaches modules, its own type so the prelude's drivers do not hand it to this test, or theirs to it.
struct module_program
{
    program p;
};

/// The files of tests/corpus/modules/, parsed once: the library every corpus file may `use`.
struct parsed_modules
{
    cc::vector<cc::unique_ptr<sgl::parsed_file>> parsed;
    cc::vector<cc::unique_ptr<sgl::ast::file_ast>> built;
    cc::vector<sgl::check::module_file> files;
};

parsed_modules const& corpus_modules()
{
    static auto const result = []
    {
        auto out = parsed_modules();
        auto sources = cc::vector<program>();
        add_files_under(sources, "corpus", nx::source_path(SGL_CORPUS_DIR));
        for (auto const& p : sources)
            if (p.name.starts_with("corpus/modules/"))
            {
                out.parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(p.source)));
                out.built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*out.parsed.back())));
                out.files.push_back({.file = *out.parsed.back(), .ast = *out.built.back()});
            }
        return out;
    }();
    return result;
}
} // namespace

INVOCABLE_TEST("sgl checked modules - a program behind its modules' layer checks as it does with them beside it",
               (module_program const& m))
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const& library = corpus_modules().files;
    auto const file = sgl::parse(m.p.source);
    auto const ast = sgl::ast::build(file);
    auto const user = sgl::check::module_file{.file = file, .ast = ast};

    auto const layer = sgl::check::check_modules(*checked, library, user);
    REQUIRE(layer.has_value());
    REQUIRE(sgl::check::reaches_the_same_modules(layer.value(), library, user));
    auto const beside = sgl::check::check(*checked, library, user);
    auto const behind = sgl::check::check(layer.value(), library, user);

    REQUIRE(behind.library_files.size() == beside.library_files.size());
    auto files = cc::vector<sgl::check::module_file>();
    for (auto const& f : sgl::parsed_prelude())
        files.push_back({.file = f.file, .ast = f.ast});
    for (auto i = isize(0); i < beside.library_files.size(); ++i)
    {
        CHECK(behind.library_files[i] == beside.library_files[i]);
        files.push_back(library[beside.library_files[i]]);
    }
    files.push_back(user);
    CHECK(observed(behind, files) == observed(beside, files));
    CHECK(behind.symbols.size() == beside.symbols.size());
    CHECK(behind.types.size() == beside.types.size());
    CHECK(behind.tests.size() == beside.tests.size());

    // the compilation model's artifacts: the layer read back is the layer, the modules' tests' trees among it
    auto const saved = sgl::check::save_artifact(layer.value(), artifact_key);
    REQUIRE(saved.has_value());
    auto const loaded = sgl::check::load_modules_artifact(saved.value(), artifact_key, *checked, library, user,
                                                          sgl::builtins::default_registry());
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().module() == layer.value().module());
    auto const behind_loaded = sgl::check::check(loaded.value(), library, user);
    CHECK(behind_loaded == behind);
    CHECK(observed(behind_loaded, files) == observed(behind, files));

    // the layer is shared by every program that reaches its modules, so a program's check leaves its part alone
    auto written = cc::string();
    for (auto const& w : prelude_writes(layer.value().module(), behind))
        written.appendf("{} ", w);
    CHECK(written == "");
}

TEST("sgl checked modules - every corpus file that uses a module")
{
    auto files = cc::vector<program>();
    add_files_under(files, "corpus", nx::source_path(SGL_CORPUS_DIR));
    auto const& library = corpus_modules().files;
    auto layered = isize(0);
    for (auto const& f : files)
    {
        auto const parsed = sgl::parse(f.source);
        auto const built = sgl::ast::build(parsed);
        if (!sgl::check::reaches_modules_to_layer(library, {.file = parsed, .ast = built}))
            continue;
        nx::invoke_tests(f.name, module_program{.p = f});
        ++layered;
    }
    REQUIRE(layered > 0);
}

TEST("sgl prelude artifact - reads back as the layer it was written from")
{
    CHECK(loaded_prelude().module() == sgl::checked_prelude()->module());
    // what is read back writes the same bytes, so nothing a walk reaches is lost on the way
    auto const again = sgl::check::save_artifact(loaded_prelude(), artifact_key);
    REQUIRE(again.has_value());
    REQUIRE(again.value().size() == prelude_artifact().size());
    CHECK(cc::memcmp(again.value().data(), prelude_artifact().data(), prelude_artifact().size()) == 0);
}

TEST("sgl prelude artifact - anything but its own bytes under its own key is a miss")
{
    auto const& bytes = prelude_artifact();
    auto const files = prelude_modules();
    auto const& registry = sgl::builtins::default_registry();
    auto const loads = [&](cc::span<byte const> b, cc::string_view key, cc::span<sgl::check::module_file const> f)
    { return sgl::check::load_artifact(b, key, f, registry).has_value(); };

    CHECK(loads(bytes, artifact_key, files));
    // another compiler's build
    CHECK(!loads(bytes, "another key", files));
    // cut short anywhere: in the header, in the payload, by one byte
    for (auto const size : {isize(0), isize(7), isize(40), bytes.size() / 2, bytes.size() - 1})
        CHECK(!loads(cc::span<byte const>(bytes).subspan({.offset = 0, .size = size}), artifact_key, files));
    // one byte changed, in the header and in the payload
    for (auto const at : {isize(0), isize(9), bytes.size() / 3, bytes.size() - 1})
    {
        auto changed = bytes;
        changed[at] = byte(u8(changed[at]) ^ 0x5a);
        CHECK(!loads(changed, artifact_key, files));
    }
    // other prelude sources, here one file fewer
    CHECK(!loads(bytes, artifact_key,
                 cc::span<sgl::check::module_file const>(files).subspan({.offset = 0, .size = files.size() - 1})));
}

TEST("sgl modules artifact - one module's source changed is a miss, and so is a program that reaches other modules")
{
    auto const& library = corpus_modules().files;
    auto files = cc::vector<program>();
    add_files_under(files, "corpus", nx::source_path(SGL_CORPUS_DIR));
    for (auto const& f : files)
    {
        auto const parsed = sgl::parse(f.source);
        auto const built = sgl::ast::build(parsed);
        auto const user = sgl::check::module_file{.file = parsed, .ast = built};
        if (!sgl::check::reaches_modules_to_layer(library, user))
            continue;
        auto const& registry = sgl::builtins::default_registry();
        auto const layer = sgl::check::check_modules(*sgl::checked_prelude(), library, user);
        REQUIRE(layer.has_value());
        auto const saved = sgl::check::save_artifact(layer.value(), artifact_key);
        REQUIRE(saved.has_value());
        CHECK(sgl::check::load_modules_artifact(saved.value(), artifact_key, *sgl::checked_prelude(), library, user,
                                                registry)
                  .has_value());
        CHECK(!sgl::check::load_modules_artifact(saved.value(), "another key", *sgl::checked_prelude(), library, user,
                                                 registry)
                   .has_value());
        // the prelude's artifact is no layer of modules, and a layer of modules is no prelude's
        CHECK(!sgl::check::load_modules_artifact(prelude_artifact(), artifact_key, *sgl::checked_prelude(), library,
                                                 user, registry)
                   .has_value());
        CHECK(!sgl::check::load_artifact(saved.value(), artifact_key, prelude_modules(), registry).has_value());

        // every module file the layer holds, edited by one trailing line
        auto edited = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
        auto edited_asts = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
        auto changed = cc::vector<sgl::check::module_file>();
        for (auto const& m : library)
        {
            edited.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(cc::string(m.file.source) + "\n// edited\n")));
            edited_asts.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*edited.back())));
            changed.push_back({.file = *edited.back(), .ast = *edited_asts.back()});
        }
        CHECK(!sgl::check::load_modules_artifact(saved.value(), artifact_key, *sgl::checked_prelude(), changed, user,
                                                 registry)
                   .has_value());
        // a program of no module reaches no layer
        auto const none = sgl::parse("fun f() -> int => 1\n");
        auto const none_ast = sgl::ast::build(none);
        CHECK(!sgl::check::load_modules_artifact(saved.value(), artifact_key, *sgl::checked_prelude(), library,
                                                 {.file = none, .ast = none_ast}, registry)
                   .has_value());
        return;
    }
    FAIL("no corpus file reaches a module");
}

TEST("sgl checked modules - a compile that sets its own options continues from its modules' layer, one that sets "
     "theirs does not")
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const module_file = sgl::parse("module tuned\n"
                                        "@option const width = 2\n"
                                        "fun scale(x: float) -> float => x * (width as float)\n");
    auto const module_ast = sgl::ast::build(module_file);
    auto const library = cc::vector<sgl::check::module_file>{{.file = module_file, .ast = module_ast}};
    auto const program_file = sgl::parse("use tuned\n"
                                         "@option const taps = 4\n"
                                         "fun spread() -> float[taps] => float[taps].filled(tuned.scale(1.0))\n"
                                         "test spread().length == taps and spread()[0] == 2.0\n");
    auto const program_ast = sgl::ast::build(program_file);
    auto const user = sgl::check::module_file{.file = program_file, .ast = program_ast};

    auto const layer = sgl::check::check_modules(*checked, library, user);
    REQUIRE(layer.has_value());
    auto files = cc::vector<sgl::check::module_file>();
    for (auto const& f : sgl::parsed_prelude())
        files.push_back({.file = f.file, .ast = f.ast});
    files.push_back(library[0]);
    files.push_back(user);

    // the program's own options, a name no option has, and one given twice: behind the layer as beside it
    using options = cc::vector<sgl::check::option_value>;
    for (auto const& set : {options{{.name = "taps", .value = "3"}}, options{{.name = "nope", .value = "1"}},
                            options{{.name = "taps", .value = "3"}, {.name = "taps", .value = "5"}}})
    {
        CHECK(sgl::check::layer_serves_options(layer.value(), set));
        auto const beside = sgl::check::check(*checked, library, user, set);
        auto const behind = sgl::check::check(layer.value(), library, user, set);
        CHECK(observed(behind, files) == observed(beside, files));
        CHECK(behind.diagnostics.size() == beside.diagnostics.size());
    }
    // a module's option was checked at its default in the layer, so setting it takes the whole check
    CHECK(!sgl::check::layer_serves_options(layer.value(), options{{.name = "tuned.width", .value = "3"}}));
    CHECK(!sgl::check::layer_serves_options(
        layer.value(), options{{.name = "taps", .value = "3"}, {.name = "tuned.width", .value = "3"}}));
}

// What a module checked once for every set that reaches it would save a process: shaped-viewer's entries each reach a
// set of modules, and today each distinct set is a layer of its own, checked whole.
// Manual, since it measures rather than asserts: `uv run dev.py test "sgl checked modules - shaped-viewer" --manual`.
TEST("sgl checked modules - shaped-viewer's sets of modules, checked each, against every module checked once",
     nx::config::manual)
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const module_name = [](cc::string_view source) -> cc::string
    {
        // the first line that is a `module` declaration, up to its end
        for (auto at = isize(0); at < source.size();)
        {
            auto end = at;
            while (end < source.size() && source[end] != '\n' && source[end] != '\r')
                ++end;
            auto const line = source.subview({.start = at, .end = end});
            if (line.starts_with("module "))
                return cc::string(line.subview(7));
            at = end + 1;
        }
        return {};
    };

    auto sources = cc::vector<program>();
    add_files_under(sources, "sv", nx::source_path(SGL_SV_SHADERS_DIR));
    add_files_under(sources, "sr", nx::source_path(SGL_SR_SHADERS_DIR) + "/modules");
    auto programs = cc::vector<program>();
    add_files_under(programs, "sv-tests", nx::source_path(SGL_SV_TEST_SHADERS_DIR));
    auto parsed = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
    auto built = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
    auto library = cc::vector<sgl::check::module_file>();
    auto names = cc::vector<cc::string>();
    for (auto const& s : sources)
    {
        auto const name = module_name(s.source);
        if (name.empty())
        {
            // an entry of the package, which no module dir holds
            if (!s.name.subview(3).contains('/'))
                programs.push_back(s);
            continue;
        }
        parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(s.source)));
        built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*parsed.back())));
        library.push_back({.file = *parsed.back(), .ast = *built.back()});
        auto is_named = false;
        for (auto const& n : names)
            is_named = is_named || n == name;
        if (!is_named)
            names.push_back(name);
    }

    // each distinct set once, as the driver keeps them
    auto layers = cc::vector<sgl::check::checked_prelude>();
    auto sets_secs = 0.0;
    for (auto const& p : programs)
    {
        auto const file = sgl::parse(p.source);
        auto const ast = sgl::ast::build(file);
        auto const user = sgl::check::module_file{.file = file, .ast = ast};
        if (!sgl::check::reaches_modules_to_layer(library, user))
            continue;
        auto is_known = false;
        for (auto const& l : layers)
            is_known = is_known || sgl::check::reaches_the_same_modules(l, library, user);
        if (is_known)
            continue;
        auto const started = cc::current_time_steady_secs();
        auto layer = sgl::check::check_modules(*checked, library, user);
        sets_secs += cc::current_time_steady_secs() - started;
        REQUIRE(layer.has_value());
        layers.push_back(cc::move(layer.value()));
    }

    // every module once: a program that uses them all
    auto everything = cc::string();
    for (auto const& n : names)
        everything.appendf("use {}\n", n);
    auto const all_file = sgl::parse(everything);
    auto const all_ast = sgl::ast::build(all_file);
    auto const started = cc::current_time_steady_secs();
    auto const all = sgl::check::check_modules(*checked, library, {.file = all_file, .ast = all_ast});
    auto const once_secs = cc::current_time_steady_secs() - started;
    REQUIRE(all.has_value());

    cc::println("{} programs, {} modules in {} files, {} distinct sets", programs.size(), names.size(), library.size(),
                layers.size());
    cc::println("every set checked once: {:.1f} ms; every module checked once: {:.1f} ms", sets_secs * 1000,
                once_secs * 1000);
}
