#include <babel-data/shader/spirv.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/print.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/test.hh>
#include <shader-analysis/diff.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;

namespace
{
// 0 = printed; 1 = bad usage, or a file that would not open or read as a module
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;

/// The metrics of the SPIR-V module at `path`, or nothing, its failure printed.
[[nodiscard]] cc::optional<shan::module_metrics> metrics_of(cc::string_view path)
{
    auto file = cc::file_read_stream_adapter::open(path);
    if (file.has_error())
    {
        cc::eprintln("shan: cannot open {}: {}", path, file.error().to_string());
        return {};
    }
    cc::read_stream stream = file.value().stream();
    auto const module = babel::spirv::read(stream);
    if (module.has_error())
    {
        cc::eprintln("shan: {}: {}", path, module.error().to_string());
        return {};
    }
    auto metrics = shan::analyze(module.value());
    if (metrics.has_error())
    {
        cc::eprintln("shan: {}: {}", path, metrics.error().to_string());
        return {};
    }
    return metrics.value();
}
} // namespace

// `shan analyze`: what one module costs, its totals and each function.
COMMAND("analyze")
{
    auto path = cc::string();
    auto args = nx::args({.name = "shan analyze",
                          .description = "Prints a SPIR-V module's metrics: operations by kind, register pressure and "
                                         "the critical path, its totals first and each function after."});
    args.positional("FILE", path, {.desc = "the SPIR-V module"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    if (path.empty())
    {
        cc::eprintln("shan analyze: name a module");
        return exit_usage;
    }
    auto const metrics = metrics_of(path);
    if (!metrics.has_value())
        return exit_usage;
    cc::print("{}", shan::to_text(metrics.value()));
    return exit_ok;
}

// `shan diff`: what two modules of one entry point differ in, worst first.
COMMAND("diff")
{
    auto a = cc::string();
    auto b = cc::string();
    auto args = nx::args({.name = "shan diff",
                          .description = "Prints every total in which two SPIR-V modules differ, the largest relative "
                                         "difference first: each value, and the first over the second."});
    args.positional("A", a, {.desc = "the first module, a native one"});
    args.positional("B", b, {.desc = "the second module, DXC's of the same entry point"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    if (a.empty() || b.empty())
    {
        cc::eprintln("shan diff: name two modules");
        return exit_usage;
    }
    auto const x = metrics_of(a);
    auto const y = metrics_of(b);
    if (!x.has_value() || !y.has_value())
        return exit_usage;
    auto const d = shan::diff(x.value(), y.value());
    cc::print("{}", shan::to_text(d));
    return exit_ok;
}
