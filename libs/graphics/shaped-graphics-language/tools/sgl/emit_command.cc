#include "files.hh"

#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = the text was printed, 1 = bad usage or an IO error, 2 = the source has errors, which were printed.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_errors = 2;
} // namespace

// `sgl emit`: one entry point of one source file as the text a graphics API compiles.
COMMAND("emit")
{
    auto path = cc::string();
    auto entry = cc::string();
    auto target_name = cc::string("hlsl-dx12");
    auto run_tests = false;
    auto args = nx::args({.name = "sgl emit",
                          .description = "Compiles one entry point of an SGL file and prints the target text, "
                                         "or the diagnostics that kept it from being written."});
    args.positional("FILE", path, {.desc = "the SGL source"});
    args.arg({"entry"}, entry, {.desc = "the entry point, by name", .metavar = "NAME", .required = true});
    args.arg({"target"}, target_name, {.desc = "hlsl-dx12, hlsl-vulkan, wgsl or msl", .metavar = "TARGET"});
    args.arg({"run-tests"}, run_tests, {.desc = "run the file's tests first, and write nothing where one fails"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();

    auto target = sgl::emit::target::hlsl_dx12;
    auto is_known = false;
    for (auto const t : sgl::emit::all_targets())
        if (sgl::emit::to_string(t) == target_name)
        {
            target = t;
            is_known = true;
        }
    if (!is_known)
    {
        cc::eprintln("sgl emit: unknown target '{}' (one of hlsl-dx12, hlsl-vulkan, wgsl, msl)", target_name);
        return exit_usage;
    }

    auto const source = sgl_tool::read_file(path);
    if (source.has_error())
    {
        cc::eprintln("sgl emit: cannot read {}: {}", path, source.error());
        return exit_usage;
    }

    auto const text = sgl::compile_to_text(
        {.source = source.value(), .source_name = path, .entry_point = entry, .target = target, .run_tests = run_tests});
    if (text.has_error())
    {
        cc::eprint(text.error());
        cc::flush();
        return exit_errors;
    }
    cc::print(text.value().text);
    cc::flush();
    return exit_ok;
}
