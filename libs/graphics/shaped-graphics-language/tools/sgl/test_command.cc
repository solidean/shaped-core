#include "files.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/test_source.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = every file checked clean and every test passed, 1 = bad usage or an IO error, 2 = something to report, which was.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_failed = 2;
} // namespace

// `sgl test`: the tests of SGL files, run on the interpreter.
COMMAND("test")
{
    auto paths = cc::vector<cc::string>();
    auto args = nx::args({.name = "sgl test",
                          .description = "Checks SGL files and runs their tests; prints what failed, and one line per "
                                         "file."});
    args.positional("FILES", paths, {.desc = "the SGL sources", .min_count = 1});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    auto code = exit_ok;
    for (auto const& path : paths)
    {
        auto const source = sgl_tool::read_file(path);
        if (source.has_error())
        {
            cc::eprintln("sgl test: cannot read {}: {}", path, source.error());
            code = exit_usage;
            continue;
        }
        auto const tested = sgl::test_source(source.value(), path);
        cc::eprint(tested.errors);
        cc::eprint(tested.warnings);
        auto line = cc::format("{}: {} of {} tests passed", path, tested.tests_passed, tested.tests_run);
        if (tested.tests_expecting_diagnostics > 0)
            line.appendf(", {} judged by the diagnostics they expect", tested.tests_expecting_diagnostics);
        if (auto const unchecked = tested.test_count - tested.tests_run - tested.tests_expecting_diagnostics;
            unchecked > 0)
            line.appendf(", {} did not check", unchecked);
        cc::println("{}", line);
        if (!tested.is_clean() && code == exit_ok)
            code = exit_failed;
    }
    cc::flush();
    return code;
}
