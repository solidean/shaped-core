#include <clean-core/container/span.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>

#include <shaped-linter/cli/options.hh>

namespace
{
// 0 = ran clean (no findings), 1 = bad usage, 2 = findings reported.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
[[maybe_unused]] constexpr int exit_findings = 2;

int run(scl::options const& opts)
{
    // M0 scaffold: the lexer / parser / rule engine land in later milestones.
    // For now, prove the wiring — echo what we would lint.
    cc::println("shaped-linter: {} file(s) to lint (engine not yet implemented)", opts.files.size());
    for (auto const& f : opts.files)
        cc::println("  {}", f);
    return exit_ok;
}
} // namespace

int main(int argc, char const* const* argv)
{
    auto opts = scl::parse_options(cc::span<char const* const>(argv, cc::isize(argc)));

    if (opts.has_error())
    {
        cc::eprintln("error: {}", opts.error().to_string());
        cc::eprint(scl::usage_text());
        return exit_usage;
    }

    if (opts.value().help)
    {
        cc::print(scl::usage_text());
        return exit_ok;
    }

    return run(opts.value());
}
