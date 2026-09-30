#include "check-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <shaped-graphics-language/check/footprint.hh>
#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics-language/legalize/legalize.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>
#include <shaped-graphics-language/test/run_tests.hh>

// clean-core has no directory listing yet.
#include <filesystem>

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
    for (auto const& e : fs::recursive_directory_iterator(base, ec))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back({.name = cc::format("{}/{}", label, fs::relative(e.path(), base, ec).generic_string().c_str()),
                             .source = read_text(cc::string(e.path().string().c_str()))});
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
        out.appendf("footprint {}\n",
                    sgl::check::footprint_text(sgl::check::footprint_of(m, sgl::check::legalize(m, e))));
        for (auto const target : sgl::emit::all_targets())
        {
            auto const legal = sgl::check::legalize(m, e, {.is_emulated = target == sgl::emit::target::wgsl});
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
    files.push_back(user);
    CHECK(observed(behind, files) == observed(together, files));
    if (p.name.starts_with("reporting"))
        CHECK(!together.diagnostics.empty());
    CHECK(behind.symbols.size() == together.symbols.size());
    CHECK(behind.types.size() == together.types.size());
    CHECK(behind.tests.size() == together.tests.size());
}

// Three drivers rather than one, so the larger sets check in parallel.
TEST("sgl checked prelude - every corpus file")
{
    auto files = cc::vector<program>();
    add_files_under(files, "corpus", SGL_CORPUS_DIR);
    REQUIRE(files.size() > 0);
    for (auto const& p : files)
        nx::invoke_tests(p.name, p);
}

TEST("sgl checked prelude - every sample and sg shader")
{
    auto files = cc::vector<program>();
    add_files_under(files, "samples", SGL_SAMPLES_DIR);
    add_files_under(files, "sg", SGL_SG_SHADERS_DIR);
    REQUIRE(files.size() > 0);
    for (auto const& p : files)
        nx::invoke_tests(p.name, p);
}

TEST("sgl checked prelude - programs written to report")
{
    for (auto const& p : written_programs())
        nx::invoke_tests(p.name, p);
}
