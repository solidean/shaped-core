#include "files.hh"
#include "module_dirs.hh"

#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/emit/emit.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = the text was printed, its warnings on stderr; 1 = bad usage or an IO error; 2 = the source has errors, which were printed.
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
    auto line_directives = false;
    auto diagnostics = false;
    auto software_trace = false;
    auto undefined = cc::string("none");
    auto weaken_float = false;
    auto device_feature_names = cc::vector<cc::string>();
    auto option_arguments = cc::vector<cc::string>();
    auto module_dirs = cc::vector<cc::string>();
    auto args = nx::args({.name = "sgl emit",
                          .description = "Compiles one entry point of an SGL file and prints the target text, with its "
                                         "warnings on stderr, or the diagnostics that kept it from being written."});
    args.positional("FILE", path, {.desc = "the SGL source"});
    args.arg({"entry"}, entry, {.desc = "the entry point, by name", .metavar = "NAME", .required = true});
    args.arg({"target"}, target_name, {.desc = "hlsl-dx12, hlsl-vulkan, wgsl or msl", .metavar = "TARGET"});
    args.arg({"run-tests"}, run_tests, {.desc = "run the file's tests first, and write nothing where one fails"});
    args.arg({"line-directives"}, line_directives,
             {.desc = "write `#line` into HLSL and MSL, as a host's compile does, naming the SGL line of each "
                      "statement"});
    args.arg({"diagnostics"}, diagnostics,
             {.desc = "write print and assert into sg's debug buffer, as a dev build does"});
    args.arg({"software-trace"}, software_trace,
             {.desc = "write an inline trace as its emulated walk on HLSL too, as a package's SOFTWARE_TRACE does"});
    args.arg({"undefined"}, undefined,
             {.desc = "what an integer division by zero or an array index outside the array does: none, abort, or "
                      "report, which prints it with --diagnostics",
              .metavar = "GUARD"});
    args.arg({"weaken-float"}, weaken_float,
             {.desc = "compile an entry point whose @float WGSL cannot keep at .fast, with a warning, as a host that "
                      "allows it on WebGPU does"});
    args.arg({"device-feature"}, device_feature_names,
             {.desc = "a feature the device has, which `has` answers and slib passes for every one a context has, "
                      "`--device-feature float_atomic_add`; repeat for each",
              .metavar = "FEATURE"});
    args.arg(
        {"option"}, option_arguments,
        {.desc = "give an option of the source a value, `--option tile=16`; repeat for each", .metavar = "NAME=VALUE"});
    args.arg({"module-dir"}, module_dirs,
             {.desc = "a directory whose .sgl files are modules the file may use", .metavar = "DIR"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    auto const library = sgl_tool::read_module_dirs(module_dirs);
    if (library.has_error())
    {
        cc::eprintln("sgl emit: {}", library.error());
        return exit_usage;
    }

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

    auto guards_undefined = sgl::check::undefined_guard::none;
    if (undefined == "abort")
        guards_undefined = sgl::check::undefined_guard::abort;
    else if (undefined == "report")
        guards_undefined = sgl::check::undefined_guard::report;
    else if (undefined == "define")
        guards_undefined = sgl::check::undefined_guard::define;
    else if (undefined != "none")
    {
        cc::eprintln("sgl emit: unknown --undefined '{}' (one of none, abort, report, define)", undefined);
        return exit_usage;
    }

    auto device_features = sgl::check::feature_set();
    for (auto const& name : device_feature_names)
    {
        auto const index = sgl::check::find_feature(name);
        if (index < 0)
        {
            cc::eprintln("sgl emit: unknown feature '{}'", name);
            return exit_usage;
        }
        device_features.set(sgl::check::feature(index));
    }

    auto const options = sgl_tool::parse_options(option_arguments);
    if (options.has_error())
    {
        cc::eprintln("sgl emit: an option is given as `name=value`, and '{}' is not", options.error());
        return exit_usage;
    }

    auto const source = sgl_tool::read_file(path);
    if (source.has_error())
    {
        cc::eprintln("sgl emit: cannot read {}: {}", path, source.error());
        return exit_usage;
    }

    auto const text = sgl::compile_to_text({.source = source.value(),
                                            .source_name = path,
                                            .library = sgl_tool::files_but(library.value(), path),
                                            .entry_point = entry,
                                            .target = target,
                                            .options = options.value(),
                                            .run_tests = run_tests,
                                            .line_directives = line_directives,
                                            .diagnostics = diagnostics,
                                            .guards_undefined = guards_undefined,
                                            .software_trace = software_trace,
                                            .weakens_float = weaken_float,
                                            .device_features = device_features});
    if (text.has_error())
    {
        cc::eprint(text.error());
        cc::flush();
        return exit_errors;
    }
    for (auto const& w : text.value().warnings)
        cc::eprintln("{}", w);
    cc::print(text.value().text);
    cc::flush();
    return exit_ok;
}
