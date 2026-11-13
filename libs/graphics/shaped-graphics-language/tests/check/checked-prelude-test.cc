#include "check-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/time.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/driver/shared_library.hh>
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
/// `observed`, with each file named by `names`, parallel to `files`, rather than by its position.
/// A test of a file the program does not reach is left out, as it is from every driver.
cc::string observed_as(sgl::check::checked_module const& m,
                       cc::span<sgl::check::module_file const> files,
                       cc::span<cc::string const> names)
{
    auto out = cc::string();
    auto const name_of = [&](sgl::i32 file) { return names[file]; };
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
    // which module's tests come first follows the order the modules were checked in, which no reader depends on
    auto tests = cc::vector<cc::string>();
    for (auto const& r : sgl::test::run_tests(m, files))
    {
        auto const& t = m.tests[r.test];
        if (!m.is_reached_file(t.file))
            continue;
        tests.push_back(cc::format("test at {}:{} ({}): {}, {} checks\n", name_of(t.file), t.where.offset, t.scope_path,
                                   sgl::test::to_string(r.status), r.checks_run));
    }
    cc::sort(tests);
    for (auto const& t : tests)
        out += t;
    return out;
}

cc::string observed(sgl::check::checked_module const& m, cc::span<sgl::check::module_file const> files)
{
    auto names = cc::vector<cc::string>();
    for (auto i = isize(0); i < files.size(); ++i)
        names.push_back(cc::format("file{}", i));
    return observed_as(m, files, names);
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

TEST("sgl checked modules - a module file that does not parse makes no layer of the whole library, and a program "
     "using the others still continues from theirs")
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const clean_file = sgl::parse("module clean\nfun half(x: float) -> float => x * 0.5\n");
    auto const clean_ast = sgl::ast::build(clean_file);
    auto const broken_file = sgl::parse("module broken\nfun f(x: float -> float => x\n");
    auto const broken_ast = sgl::ast::build(broken_file);
    // a parse error is the parser's or the AST's to report, and the check of what it left reports nothing
    REQUIRE((!broken_file.diagnostics.empty() || !broken_ast.diagnostics.empty()));
    auto const library = cc::vector<sgl::check::module_file>{{.file = clean_file, .ast = clean_ast},
                                                             {.file = broken_file, .ast = broken_ast}};
    auto const program_file = sgl::parse("use clean\ntest clean.half(2.0) == 1.0\n");
    auto const program_ast = sgl::ast::build(program_file);
    auto const user = sgl::check::module_file{.file = program_file, .ast = program_ast};

    CHECK(!sgl::check::check_library(*checked, library).has_value());
    auto const layer = sgl::check::check_modules(*checked, library, user);
    REQUIRE(layer.has_value());
    auto const m = sgl::check::check(layer.value(), library, user);
    CHECK(m.diagnostics.empty());
    REQUIRE(m.library_files.size() == 1);
    CHECK(m.library_files[0] == 0);
}

namespace
{
/// The module a source's first `module` line names; empty for a source of no module.
cc::string module_name_of(cc::string_view source)
{
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
}

/// shaped-viewer's shader library as its packages hand it in, its and shaped-rendering's modules, and the programs of
/// its packages and its tests, parsed once.
struct viewer_library
{
    cc::vector<program> programs;
    cc::vector<cc::unique_ptr<sgl::parsed_file>> parsed;
    cc::vector<cc::unique_ptr<sgl::ast::file_ast>> built;
    cc::vector<sgl::check::module_file> files;
    /// Parallel to `files`.
    cc::vector<cc::string> names;
    cc::vector<cc::string> modules;
};

viewer_library const& viewer_modules()
{
    static auto const result = []
    {
        auto out = viewer_library();
        auto sources = cc::vector<program>();
        add_files_under(sources, "sv", nx::source_path(SGL_SV_SHADERS_DIR));
        add_files_under(sources, "sr", nx::source_path(SGL_SR_SHADERS_DIR) + "/modules");
        add_files_under(out.programs, "sv-tests", nx::source_path(SGL_SV_TEST_SHADERS_DIR));
        for (auto const& s : sources)
        {
            auto const name = module_name_of(s.source);
            if (name.empty())
            {
                // an entry of a package, which no module directory holds
                if (!s.name.subview(3).contains('/'))
                    out.programs.push_back(s);
                continue;
            }
            out.parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(s.source)));
            out.built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*out.parsed.back())));
            out.files.push_back({.file = *out.parsed.back(), .ast = *out.built.back()});
            out.names.push_back(s.name);
            auto is_named = false;
            for (auto const& n : out.modules)
                is_named = is_named || n == name;
            if (!is_named)
                out.modules.push_back(name);
        }
        return out;
    }();
    return result;
}

/// The files `m` was checked over, the prelude's, the library's it placed and the program, and each one's name.
struct placed_files
{
    cc::vector<sgl::check::module_file> files;
    cc::vector<cc::string> names;
};

placed_files placed(sgl::check::checked_module const& m,
                    cc::span<sgl::check::module_file const> library,
                    cc::span<cc::string const> library_names,
                    sgl::check::module_file program)
{
    auto out = placed_files();
    for (auto const& f : sgl::parsed_prelude())
    {
        out.names.push_back(cc::format("prelude{}", out.files.size()));
        out.files.push_back({.file = f.file, .ast = f.ast});
    }
    for (auto const i : m.library_files)
    {
        out.files.push_back(library[i]);
        out.names.push_back(library_names[i]);
    }
    out.files.push_back(program);
    out.names.push_back("program");
    return out;
}
} // namespace

TEST("sgl checked modules - a chain puts a module with options late, and its link checked with a compile's values "
     "serves that compile as a check beside the library does")
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const sources = cc::vector<cc::string>{
        "module tuned\n@option const width = 2\nfun scale(x: float) -> float => x * (width as float)\n",
        "module blur\nuse tuned\nfun wide() -> float => tuned.scale(2.0)\n",
        "module plain\nfun one() -> float => 1.0\n",
    };
    auto parsed = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
    auto built = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
    auto library = cc::vector<sgl::check::module_file>();
    for (auto const& s : sources)
    {
        parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(s)));
        built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*parsed.back())));
        library.push_back({.file = *parsed.back(), .ast = *built.back()});
    }
    auto const names = cc::vector<cc::string>{"tuned.sgl", "blur.sgl", "plain.sgl"};

    // `plain` has no option, so it comes before `tuned` though `blur` needs `tuned` first
    auto const order = sgl::check::library_chain(library);
    REQUIRE(order.size() == 3);
    CHECK(order[0].name == "plain");
    CHECK(!order[0].has_options);
    CHECK(order[1].name == "tuned");
    CHECK(order[1].has_options);
    CHECK(order[2].name == "blur");
    // a module being edited goes as late as the modules using it allow: `plain` after them all, `tuned` before `blur`
    auto const plain_late = cc::vector<sgl::i32>{2};
    auto const editing_plain = sgl::check::library_chain(library, plain_late);
    REQUIRE(editing_plain.size() == 3);
    CHECK(editing_plain[0].name == "tuned");
    CHECK(editing_plain[2].name == "plain");
    auto const tuned_late = cc::vector<sgl::i32>{0};
    auto const editing_tuned = sgl::check::library_chain(library, tuned_late);
    REQUIRE(editing_tuned.size() == 3);
    CHECK(editing_tuned[0].name == "plain");
    CHECK(editing_tuned[2].name == "blur");

    using options = cc::vector<sgl::check::option_value>;
    auto const set = options{{.name = "taps", .value = "3"}, {.name = "tuned.width", .value = "3"}};
    auto const of_tuned = sgl::check::options_of_module(set, "tuned");
    REQUIRE(of_tuned.size() == 1);
    CHECK(of_tuned[0].name == "tuned.width");
    CHECK(sgl::check::options_of_module(set, "tune").empty());
    CHECK(sgl::check::options_of_module(set, "blur").empty());

    auto const first = sgl::check::check_next_module(*checked, library, "plain");
    REQUIRE(first.has_value());
    auto const tuned
        = sgl::check::check_next_module(first.value(), library, "tuned", sgl::check::options_of_module(set, "tuned"));
    REQUIRE(tuned.has_value());
    auto const last = sgl::check::check_next_module(tuned.value(), library, "blur");
    REQUIRE(last.has_value());
    auto const defaults = sgl::check::check_library(*checked, library);
    REQUIRE(defaults.has_value());

    // a layer serves exactly the module values it was checked with, whatever the program's own are
    CHECK(sgl::check::layer_serves_options(last.value(), set));
    CHECK((sgl::check::layer_serves_options(last.value(), options{{.name = "tuned.width", .value = "3"}})));
    CHECK((!sgl::check::layer_serves_options(last.value(), options{{.name = "taps", .value = "3"}})));
    CHECK((!sgl::check::layer_serves_options(last.value(), options{{.name = "tuned.width", .value = "4"}})));
    CHECK(!sgl::check::layer_serves_options(defaults.value(), set));
    CHECK((sgl::check::layer_serves_options(defaults.value(), options{{.name = "taps", .value = "3"}})));
    // a layer holding no option the compile sets serves it
    CHECK(sgl::check::layer_serves_options(first.value(), set));

    auto const program_file = sgl::parse("use blur\nuse plain\n@option const taps = 1\n"
                                         "fun total() -> float => blur.wide() + plain.one() * (taps as float)\n"
                                         "test total() == 9.0\n");
    auto const program_ast = sgl::ast::build(program_file);
    auto const user = sgl::check::module_file{.file = program_file, .ast = program_ast};
    auto const beside = sgl::check::check(*checked, library, user, set);
    auto const behind = sgl::check::check(last.value(), library, user, set);
    CHECK(beside.diagnostics.empty());
    auto const at_beside = placed(beside, library, names, user);
    auto const at_behind = placed(behind, library, names, user);
    CHECK(observed_as(behind, at_behind.files, at_behind.names) == observed_as(beside, at_beside.files, at_beside.names));
    // the module's option has the value the compile gave, and so does what the program's test reads: 2 * 3 + 1 * 3
    auto width = 0;
    for (auto const& symbol : behind.symbols)
        if (symbol.name == "width" && symbol.kind == sgl::check::symbol_kind::constant)
            width = behind.constants[symbol.info].integer;
    CHECK(width == 3);
    for (auto const& r : sgl::test::run_tests(behind, at_behind.files))
        CHECK(r.status == sgl::test::test_status::passed);
}

TEST("sgl shared library - modules of dotted names whose last parts are alike make one chain, and an option of one is "
     "set by its whole name")
{
    auto const library = cc::vector<sgl::library_file>{
        {.name = "palette/warm.sgl",
         .source = "module palette.warm\n@option const gain = 2\nfun tone() -> float => "
                   "gain as float\n"},
        {.name = "heat/warm.sgl", .source = "module heat.warm\nfun tone() -> float => 3.0\n"},
    };
    auto const shared = sgl::shared_library_of(library, "program.sgl");
    REQUIRE(shared != nullptr);
    auto const program_file = sgl::parse("use palette.warm\nuse heat.warm as hot\n"
                                         "test warm.tone() + hot.tone() == 5.0\n");
    auto const program_ast = sgl::ast::build(program_file);
    auto const user = sgl::check::module_file{.file = program_file, .ast = program_ast};

    using options = cc::vector<sgl::check::option_value>;
    auto const defaults = sgl::layer_of(*shared, user);
    REQUIRE(defaults != nullptr);
    REQUIRE(defaults->before != nullptr);
    auto const behind = sgl::check::check(*defaults->layer, shared->files, user);
    CHECK(behind.diagnostics.empty());
    auto const set = options{{.name = "palette.warm.gain", .value = "4"}};
    auto const four = sgl::layer_of(*shared, user, set);
    REQUIRE(four != nullptr);
    CHECK(four != defaults);
    CHECK(sgl::check::layer_serves_options(*four->layer, set));
    auto const with_four = sgl::check::check(*four->layer, shared->files, user, set);
    CHECK(with_four.diagnostics.empty());
}

TEST("sgl shared library - a compile that sets a module's option shares every link before that module's, and a "
     "permutation compiled again finds its own")
{
    auto const library = cc::vector<sgl::library_file>{
        {.name = "tuned.sgl",
         .source = "module tuned\n@option const width = 2\nfun scale(x: float) -> float => x * (width as float)\n"},
        {.name = "plain.sgl", .source = "module plain\nfun one() -> float => 1.0\n"},
    };
    auto const shared = sgl::shared_library_of(library, "program.sgl");
    REQUIRE(shared != nullptr);
    auto const program_file = sgl::parse("use tuned\nuse plain\nfun f() -> float => tuned.scale(plain.one())\n");
    auto const program_ast = sgl::ast::build(program_file);
    auto const user = sgl::check::module_file{.file = program_file, .ast = program_ast};

    using options = cc::vector<sgl::check::option_value>;
    auto const defaults = sgl::layer_of(*shared, user);
    auto const three = sgl::layer_of(*shared, user, options{{.name = "tuned.width", .value = "3"}});
    auto const four = sgl::layer_of(*shared, user, options{{.name = "tuned.width", .value = "4"}});
    auto const three_again = sgl::layer_of(*shared, user, options{{.name = "tuned.width", .value = "3"}});
    // the program's own options are no module's, so they continue from the defaults' chain
    auto const own = sgl::layer_of(*shared, user, options{{.name = "taps", .value = "3"}});
    REQUIRE(defaults != nullptr);
    REQUIRE(three != nullptr);
    REQUIRE(four != nullptr);
    CHECK(three != defaults);
    CHECK(four != three);
    CHECK(three_again == three);
    CHECK(own == defaults);
    // `plain` has no option, so it is the first link, and every chain holds the same one
    REQUIRE(defaults->before != nullptr);
    CHECK(three->before == defaults->before);
    CHECK(four->before == defaults->before);
    CHECK((sgl::check::layer_serves_options(*three->layer, options{{.name = "tuned.width", .value = "3"}})));
    // a value the option does not take reports, so that compile has no link and checks beside the library
    CHECK((sgl::layer_of(*shared, user, options{{.name = "tuned.width", .value = "wide"}}) == nullptr));
}

TEST("sgl shared library - a host keeps each module's link as an artifact, and a process that never checked a module "
     "loads its link from it")
{
    // sources no other test holds, so no other test's compile makes or shares these links
    auto const library = cc::vector<sgl::library_file>{
        {.name = "kept_base.sgl",
         .source = "module kept_base\nfun kept_twice(x: float) -> float => x + x\nfun kept_scale(x: float) -> float "
                   "=> x * 3.0\n"},
        {.name = "kept_top.sgl",
         .source = "module kept_top\nuse kept_base\nfun kept_both(x: float) -> float => kept_base.kept_twice(x) + "
                   "kept_base.kept_scale(x)\n"},
    };
    auto const program_file = sgl::parse("use kept_top\nfun f() -> float => kept_top.kept_both(1.0)\n");
    auto const program_ast = sgl::ast::build(program_file);
    auto const user = sgl::check::module_file{.file = program_file, .ast = program_ast};
    auto const keys = sgl::module_artifact_keys(library, "program.sgl");
    REQUIRE(keys.size() == 2);

    sgl::keep_module_artifacts(true);
    auto const shared = sgl::shared_library_of(library, "program.sgl");
    REQUIRE(shared != nullptr);
    auto const checked = sgl::layer_of(*shared, user);
    REQUIRE(checked != nullptr);
    // other tests may compile meanwhile, and what they make is theirs
    auto made = cc::vector<sgl::module_artifact>();
    for (auto& a : sgl::take_module_artifacts())
        if (a.key == keys[0] || a.key == keys[1])
            made.push_back(cc::move(a));
    CHECK(made.size() == 2);
    CHECK(!checked->layer->is_relocated());

    // Another process, as far as these links go: the same sources under other names are another library, which shares
    // no link with this one, and reads each module's link from the artifact a host handed in.
    auto renamed = library;
    renamed[0].name = "elsewhere/kept_base.sgl";
    renamed[1].name = "elsewhere/kept_top.sgl";
    sgl::offer_module_artifacts(made);
    auto const other = sgl::shared_library_of(renamed, "program.sgl");
    REQUIRE(other != nullptr);
    auto const loaded = sgl::layer_of(*other, user);
    REQUIRE(loaded != nullptr);
    CHECK(loaded != checked);
    CHECK(loaded->layer->is_relocated());
    CHECK(loaded->before->layer->is_relocated());
    for (auto const& a : sgl::take_module_artifacts())
        CHECK((a.key != keys[0] && a.key != keys[1]));
    CHECK(sgl::check::layer_difference(*checked->layer, *loaded->layer).empty());
    auto const behind = sgl::check::check(*loaded->layer, other->files, user);
    CHECK(behind.diagnostics.empty());
    sgl::keep_module_artifacts(false);
}

TEST("sgl checked modules - behind a layer of the whole library a program checks as beside the modules it reaches")
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const& library = viewer_modules();
    auto const whole = sgl::check::check_library(*checked, library.files);
    REQUIRE(whole.has_value());

    auto continued = isize(0);
    for (auto const& p : library.programs)
    {
        auto const file = sgl::parse(p.source);
        auto const ast = sgl::ast::build(file);
        auto const user = sgl::check::module_file{.file = file, .ast = ast};
        if (!sgl::check::may_continue_from(whole.value(), library.files, user))
            continue;
        ++continued;
        // every program under --thorough; by default one of each shape: a trace with callables, the hybrid, a raster
        // pass, and a probe whose local names the library's other modules declare too
        auto const is_default = p.name == "sv/tracer_pipeline.sgl" || p.name == "sv/hybrid_pass.sgl"
                             || p.name == "sv/raster_pass.sgl" || p.name == "sv-tests/bsdf_probe.sgl";
        if (!nx::is_thorough() && !is_default)
            continue;
        auto const beside = sgl::check::check(*checked, library.files, user);
        auto const behind = sgl::check::check(whole.value(), library.files, user);
        REQUIRE(behind.reached_library_files.size() == beside.library_files.size());
        for (auto i = isize(0); i < beside.library_files.size(); ++i)
            CHECK(behind.reached_library_files[i] == beside.library_files[i]);
        auto const at_beside = placed(beside, library.files, library.names, user);
        auto const at_behind = placed(behind, library.files, library.names, user);
        auto const said_beside = observed_as(beside, at_beside.files, at_beside.names);
        auto const said_behind = observed_as(behind, at_behind.files, at_behind.names);
        if (said_behind != said_beside)
        {
            // the first line that differs, and the line on each side
            auto line_start = isize(0);
            auto at = isize(0);
            while (at < said_beside.size() && at < said_behind.size() && said_beside[at] == said_behind[at])
            {
                if (said_beside[at] == '\n')
                    line_start = at + 1;
                ++at;
            }
            auto const line_of = [&](cc::string_view text)
            {
                auto end = line_start;
                while (end < text.size() && text[end] != '\n')
                    ++end;
                return cc::string(text.subview({.start = cc::min(line_start, text.size()), .end = end}));
            };
            FAIL(cc::format("{} reads otherwise behind the whole library:\n  beside: {}\n  behind: {}", p.name,
                            line_of(said_beside), line_of(said_behind)));
        }
    }
    CHECK(continued >= 8);
}

// What checking every module once saves a process against checking each set of modules a program reaches.
// Manual, since it measures rather than asserts: `uv run dev.py test "sgl checked modules - shaped-viewer" --manual`.
namespace
{
/// `source` with a comment line behind its `module` line: every place in the file moves, and nothing it declares.
cc::string with_comment_line(cc::string_view source)
{
    auto const at = source.find("module ");
    auto const end = at < 0 ? isize(0) : source.subview(at).find('\n');
    auto const split = at < 0 || end < 0 ? isize(0) : at + end + 1;
    return cc::format("{}// moved by an edit\n{}", source.subview({.start = 0, .end = split}), source.subview(split));
}

/// `source` with the first float literal of an arrow body that states its result type made another value of the type:
/// the body changes, and nothing a user of the module sees; `source` itself where it has no such literal.
cc::string with_body_literal_changed(cc::string_view source)
{
    for (auto line_start = isize(0); line_start < source.size();)
    {
        auto line_end = source.subview(line_start).find('\n');
        line_end = line_end < 0 ? source.size() : line_start + line_end;
        auto const line = source.subview({.start = line_start, .end = line_end});
        auto const arrow = line.find("=> ");
        if (line.find(" -> ") >= 0 && arrow >= 0 && line.find("//") < 0)
            for (auto i = arrow; i + 2 < line.size(); ++i)
                if (line[i] >= '0' && line[i] <= '9' && line[i + 1] == '.' && line[i + 2] >= '0' && line[i + 2] <= '9'
                    && (i == 0 || !(line[i - 1] >= 'a' && line[i - 1] <= 'z')))
                {
                    auto end = i + 2;
                    while (end < line.size() && line[end] >= '0' && line[end] <= '9')
                        ++end;
                    auto const at = line_start + end;
                    return cc::format("{}1{}", source.subview({.start = 0, .end = at}), source.subview(at));
                }
        line_start = line_end + 1;
    }
    return cc::string(source);
}

/// `source` with a function more at its end: what the module declares changes, so no user of it moves past the edit.
cc::string with_function_added(cc::string_view source)
{
    return cc::format("{}\nfun added_by_an_edit() -> int => 1\n", source);
}
} // namespace

namespace
{
/// Every module of `library` edited three ways, and every link after it relocated past the edit where it may be: each
/// one must be the very link a fresh check makes over the edited one, field for field.
/// So must the link's artifact, saved before the edit and loaded after it, which loads exactly where it relocates.
/// The chain is the links that build; a module that reports ends it, since nothing is relocated past what failed.
void check_relocations(cc::span<sgl::check::module_file const> library, bool expects_relocation)
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto order = sgl::check::library_chain(library);
    REQUIRE(!order.empty());

    // the chain before any edit, of the modules that check: one that reports is left out, and so is one using it
    auto before = cc::vector<sgl::check::checked_prelude>();
    auto kept = cc::vector<sgl::check::chained_module>();
    for (auto const& module : order)
    {
        auto next = sgl::check::check_next_module(before.empty() ? *checked : before.back(), library, module.name);
        if (!next.has_value())
            continue;
        before.push_back(cc::move(next.value()));
        kept.push_back(module);
    }
    order = cc::move(kept);
    cc::println("{} of the library's modules make a chain", order.size());

    using edit_fn = cc::string (*)(cc::string_view);
    struct edit
    {
        char const* name;
        edit_fn apply;
        bool keeps_interface;
    };
    auto const edits = cc::vector<edit>{
        {.name = "a comment line", .apply = &with_comment_line, .keeps_interface = true},
        {.name = "a body's literal", .apply = &with_body_literal_changed, .keeps_interface = true},
        {.name = "a function added", .apply = &with_function_added, .keeps_interface = false},
    };
    for (auto const& e : edits)
    {
        auto relocated = 0;
        auto refused = 0;
        for (auto k = isize(0); k < order.size(); ++k)
        {
            // the edited library: module k's files with a comment line each
            auto edited_parsed = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
            auto edited_built = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
            auto files = cc::vector<sgl::check::module_file>();
            for (auto i = isize(0); i < library.size(); ++i)
            {
                auto is_edited = false;
                for (auto const e : order[k].files)
                    is_edited = is_edited || e == i;
                if (!is_edited)
                {
                    files.push_back(library[i]);
                    continue;
                }
                edited_parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(e.apply(library[i].file.source))));
                edited_built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*edited_parsed.back())));
                files.push_back({.file = *edited_parsed.back(), .ast = *edited_built.back()});
            }

            auto after = cc::vector<sgl::check::checked_prelude>();
            auto const base_of = [&](isize j) -> sgl::check::checked_prelude const&
            {
                if (j == 0)
                    return *checked;
                return j - 1 < k ? before[j - 1] : after[j - 1 - k];
            };
            auto edited = sgl::check::check_next_module(base_of(k), files, order[k].name);
            // a literal changed may break what the module checks, which leaves nothing to relocate onto
            if (!edited.has_value())
                continue;
            after.push_back(cc::move(edited.value()));
            for (auto j = k + 1; j < order.size(); ++j)
            {
                auto fresh = sgl::check::check_next_module(base_of(j), files, order[j].name);
                // an interface changed may break a module using it, after which nothing is relocated
                if (!fresh.has_value())
                    break;
                auto const* const old_base = &before[j - 1];
                auto moved = sgl::check::relocate_link(before[j], *old_base, base_of(j));
                // the same link kept as an artifact before the edit, and loaded after it
                auto const kept_bytes = sgl::check::save_link_artifact(before[j], *old_base, "oracle");
                REQUIRE(kept_bytes.has_value());
                auto loaded
                    = sgl::check::load_link_artifact(kept_bytes.value(), "oracle", base_of(j), files, order[j].name);
                CHECK(loaded.has_value() == moved.has_value());
                if (!moved.has_value())
                {
                    ++refused;
                    after.push_back(cc::move(fresh.value()));
                    continue;
                }
                ++relocated;
                auto const difference = sgl::check::layer_difference(fresh.value(), moved.value());
                if (!difference.empty())
                    FAIL(cc::format("{} relocated past {} in {} differs from its fresh check: {}", order[j].name,
                                    e.name, order[k].name, difference));
                if (loaded.has_value())
                {
                    auto const loaded_difference = sgl::check::layer_difference(fresh.value(), loaded.value());
                    if (!loaded_difference.empty())
                        FAIL(cc::format("{} loaded past {} in {} differs from its fresh check: {}", order[j].name,
                                        e.name, order[k].name, loaded_difference));
                }
                after.push_back(cc::move(moved.value()));
            }
        }
        cc::println("{}: relocated {}, checked again {}", e.name, relocated, refused);
        CHECK((e.keeps_interface && expects_relocation ? relocated > 0 : true));
    }
}
} // namespace

TEST("sgl checked modules - a link relocated past an edit of a module is the link a fresh check makes there")
{
    check_relocations(viewer_modules().files, true);
}

TEST("sgl checked modules - a link that instantiates, evaluates or inlines an edited module's function is checked "
     "again, and every other one relocated is a fresh check's")
{
    // a generic and a template a later module instantiates, a function a const evaluates, and one a test inlines
    auto const sources = cc::vector<cc::string>{
        "module base\n\nstruct box[T = float]:\n    value: T\n\nfun twice[T](x: T) -> T => x + x\n"
        "fun scale(x: float) -> float => x * 2.0\nfun sum_to(n: int) -> int => n * (n + 1) / 2\n\n"
        "test scale(1.0) == 2.0\n",
        "module mid\n\nuse base\n\nfun boxed(x: float) -> float => base.box(value = x).value\n"
        "fun doubled(x: float) -> float => base.twice(x)\nconst triangle = base.sum_to(4)\n\n"
        "test base.scale(2.0) == 4.0\n",
        "module other\n\nfun half(x: float) -> float => x * 0.5\n",
        "module top\n\nuse mid\nuse other\n\nfun both(x: float) -> float => mid.doubled(x) + other.half(x)\n",
    };
    auto parsed = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
    auto built = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
    auto library = cc::vector<sgl::check::module_file>();
    for (auto const& source : sources)
    {
        parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(source)));
        built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*parsed.back())));
        library.push_back({.file = *parsed.back(), .ast = *built.back()});
    }
    REQUIRE(sgl::check::check_library(*sgl::checked_prelude(), library).has_value());
    check_relocations(library, true);
}

TEST("sgl checked modules - shaped-viewer's sets of modules, checked each, against every module checked once",
     nx::config::manual)
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const& library = viewer_modules();

    // each distinct set once, as the driver kept them before the library was one layer
    auto layers = cc::vector<sgl::check::checked_prelude>();
    auto sets_secs = 0.0;
    for (auto const& p : library.programs)
    {
        auto const file = sgl::parse(p.source);
        auto const ast = sgl::ast::build(file);
        auto const user = sgl::check::module_file{.file = file, .ast = ast};
        if (!sgl::check::reaches_modules_to_layer(library.files, user))
            continue;
        auto is_known = false;
        for (auto const& l : layers)
            is_known = is_known || sgl::check::reaches_the_same_modules(l, library.files, user);
        if (is_known)
            continue;
        auto const started = cc::current_time_steady_secs();
        auto layer = sgl::check::check_modules(*checked, library.files, user);
        sets_secs += cc::current_time_steady_secs() - started;
        REQUIRE(layer.has_value());
        layers.push_back(cc::move(layer.value()));
    }

    auto const started = cc::current_time_steady_secs();
    auto const whole = sgl::check::check_library(*checked, library.files);
    auto const once_secs = cc::current_time_steady_secs() - started;
    REQUIRE(whole.has_value());

    cc::println("{} programs, {} modules in {} files, {} distinct sets", library.programs.size(),
                library.modules.size(), library.files.size(), layers.size());
    cc::println("every set checked once: {:.1f} ms; the whole library checked once: {:.1f} ms", sets_secs * 1000,
                once_secs * 1000);
}

TEST("sgl checked modules - shaped-viewer's chain of module layers, and what an edit of each module checks again",
     nx::config::manual)
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const& library = viewer_modules();
    auto const order = sgl::check::library_chain(library.files);
    REQUIRE(!order.empty());

    auto step_secs = cc::vector<double>();
    auto layer = cc::optional<sgl::check::checked_prelude>();
    for (auto const& module : order)
    {
        auto const started = cc::current_time_steady_secs();
        auto next
            = sgl::check::check_next_module(layer.has_value() ? layer.value() : *checked, library.files, module.name);
        step_secs.push_back(cc::current_time_steady_secs() - started);
        REQUIRE(next.has_value());
        layer = cc::move(next);
    }
    // an edit of a module checks its layer and every one after it again
    auto total = 0.0;
    for (auto const s : step_secs)
        total += s;
    auto after = total;
    auto edits = 0.0;
    for (auto i = isize(0); i < order.size(); ++i)
    {
        cc::println("{:>2} {:<24} {:6.1f} ms, an edit of it {:6.1f} ms", i, order[i].name, step_secs[i] * 1000,
                    after * 1000);
        edits += after;
        after -= step_secs[i];
    }
    cc::println("the chain whole: {:.1f} ms; an edit of a module, on average: {:.1f} ms", total * 1000,
                edits / double(order.size()) * 1000);

    // While a module is edited the chain puts it as late as the modules using it allow, so every keystroke after the
    // first checks it and those after it again.
    auto edited = 0.0;
    for (auto const& module : order)
    {
        auto const late = sgl::check::library_chain(library.files, module.files);
        auto base = cc::optional<sgl::check::checked_prelude>();
        auto suffix = 0.0;
        auto is_after = false;
        for (auto const& m : late)
        {
            is_after = is_after || m.name == module.name;
            auto const started = cc::current_time_steady_secs();
            auto next = sgl::check::check_next_module(base.has_value() ? base.value() : *checked, library.files, m.name);
            if (is_after)
                suffix += cc::current_time_steady_secs() - started;
            REQUIRE(next.has_value());
            base = cc::move(next);
        }
        cc::println("{:<24} late: an edit of it {:6.1f} ms", module.name, suffix * 1000);
        edited += suffix;
    }
    cc::println("an edit of a module placed late, on average: {:.1f} ms", edited / double(order.size()) * 1000);

    // An edit of the first module that changes none of what it declares: it is checked again, and the links after it
    // relocated rather than checked.
    auto chain = cc::vector<sgl::check::checked_prelude>();
    for (auto const& module : order)
    {
        auto next = sgl::check::check_next_module(chain.empty() ? *checked : chain.back(), library.files, module.name);
        REQUIRE(next.has_value());
        chain.push_back(cc::move(next.value()));
    }
    auto edited_parsed = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
    auto edited_built = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < library.files.size(); ++i)
    {
        auto is_edited = false;
        for (auto const e : order[0].files)
            is_edited = is_edited || e == i;
        if (!is_edited)
        {
            files.push_back(library.files[i]);
            continue;
        }
        edited_parsed.push_back(
            cc::make_unique<sgl::parsed_file>(sgl::parse(with_comment_line(library.files[i].file.source))));
        edited_built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*edited_parsed.back())));
        files.push_back({.file = *edited_parsed.back(), .ast = *edited_built.back()});
    }
    auto moved_chain = cc::vector<sgl::check::checked_prelude>();
    auto started = cc::current_time_steady_secs();
    auto first = sgl::check::check_next_module(*checked, files, order[0].name);
    REQUIRE(first.has_value());
    moved_chain.push_back(cc::move(first.value()));
    auto const checked_secs = cc::current_time_steady_secs() - started;
    auto relocated_secs = 0.0;
    for (auto j = isize(1); j < order.size(); ++j)
    {
        started = cc::current_time_steady_secs();
        auto next = sgl::check::relocate_link(chain[j], chain[j - 1], moved_chain[j - 1]);
        relocated_secs += cc::current_time_steady_secs() - started;
        REQUIRE(next.has_value());
        moved_chain.push_back(cc::move(next.value()));
    }
    cc::println("an edit of {} that keeps its interface: {:.1f} ms checking it, {:.1f} ms relocating the {} after it",
                order[0].name, checked_secs * 1000, relocated_secs * 1000, order.size() - 1);
}

TEST("sgl checked modules - shaped-viewer's chain loaded from its links' artifacts, against checking it",
     nx::config::manual)
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const& library = viewer_modules();
    auto const order = sgl::check::library_chain(library.files);
    auto chain = cc::vector<sgl::check::checked_prelude>();
    auto const check_started = cc::current_time_steady_secs();
    for (auto const& module : order)
    {
        auto next = sgl::check::check_next_module(chain.empty() ? *checked : chain.back(), library.files, module.name);
        REQUIRE(next.has_value());
        chain.push_back(cc::move(next.value()));
    }
    auto const checked_secs = cc::current_time_steady_secs() - check_started;
    auto artifacts = cc::vector<cc::vector<sgl::byte>>();
    auto bytes = isize(0);
    for (auto i = isize(0); i < chain.size(); ++i)
    {
        auto saved = sgl::check::save_link_artifact(chain[i], i == 0 ? *checked : chain[i - 1], order[i].name);
        REQUIRE(saved.has_value());
        bytes += saved.value().size();
        artifacts.push_back(cc::move(saved.value()));
    }
    auto loaded = cc::vector<sgl::check::checked_prelude>();
    auto const load_started = cc::current_time_steady_secs();
    for (auto i = isize(0); i < chain.size(); ++i)
    {
        auto next = sgl::check::load_link_artifact(
            artifacts[i], order[i].name, loaded.empty() ? *checked : loaded.back(), library.files, order[i].name);
        REQUIRE(next.has_value());
        loaded.push_back(cc::move(next.value()));
    }
    auto const loaded_secs = cc::current_time_steady_secs() - load_started;
    CHECK(sgl::check::layer_difference(chain.back(), loaded.back()).empty());
    cc::println("{} links: {:.1f} ms checking them, {:.1f} ms loading them from {:.0f} kB of artifacts", chain.size(),
                checked_secs * 1000, loaded_secs * 1000, double(bytes) / 1024);
}

TEST("sgl checked modules - relocating shaped-viewer's chain past an edit of its first module, many times over",
     nx::config::manual)
{
    auto const* const checked = sgl::checked_prelude();
    REQUIRE(checked != nullptr);
    auto const& library = viewer_modules();
    auto const order = sgl::check::library_chain(library.files);
    auto chain = cc::vector<sgl::check::checked_prelude>();
    for (auto const& module : order)
    {
        auto next = sgl::check::check_next_module(chain.empty() ? *checked : chain.back(), library.files, module.name);
        REQUIRE(next.has_value());
        chain.push_back(cc::move(next.value()));
    }
    auto edited = sgl::check::check_next_module(*checked, library.files, order[0].name);
    REQUIRE(edited.has_value());
    auto const started = cc::current_time_steady_secs();
    for (auto round = 0; round < 40; ++round)
    {
        auto moved_chain = cc::vector<sgl::check::checked_prelude const*>();
        auto kept = cc::vector<sgl::check::checked_prelude>();
        kept.reserve(order.size());
        moved_chain.push_back(&edited.value());
        for (auto j = isize(1); j < order.size(); ++j)
        {
            auto next = sgl::check::relocate_link(chain[j], chain[j - 1], *moved_chain[j - 1]);
            REQUIRE(next.has_value());
            kept.push_back(cc::move(next.value()));
            moved_chain.push_back(&kept.back());
        }
    }
    cc::println("relocating {} links: {:.2f} ms a round", order.size() - 1,
                (cc::current_time_steady_secs() - started) * 1000 / 40);
}
