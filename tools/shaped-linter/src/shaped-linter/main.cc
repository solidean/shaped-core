#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <shaped-linter/cli/options.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/report/reporter.hh>
#include <shaped-linter/rules/engine.hh>

#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#endif

namespace
{
// 0 = ran clean (no findings), 1 = bad usage / IO error, 2 = findings reported.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_findings = 2;

/// Make the console interpret our output (already UTF-8 bytes from cc::print) as UTF-8, so the repo's
/// typography and any UTF-8 in echoed source lines render instead of codepage mojibake. A no-op when
/// output is redirected — the pipe carries the same UTF-8 bytes, which the reader decodes as UTF-8.
void enable_utf8_console()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
}

int run(scl::options const& opts)
{
    scl::source_manager sm;
    cc::vector<scl::finding> all;

    for (auto const& file : opts.files)
    {
        auto buffer = sm.add_from_file(file);
        if (buffer.has_error())
        {
            cc::eprintln("error: cannot read {}: {}", file, buffer.error().to_string());
            return exit_usage;
        }

        auto found = scl::run_rules(*buffer.value());
        for (auto& f : found)
            all.push_back(cc::move(f));
    }

    if (all.empty())
    {
        cc::println("shaped-linter: no findings in {} file(s)", opts.files.size());
        return exit_ok;
    }

    scl::report_findings(all, sm, {.color = !opts.no_color});

    if (opts.apply_fixes)
    {
        auto const changed = scl::apply_fixes(sm, all);
        if (changed.has_error())
        {
            cc::eprintln("error applying fixes: {}", changed.error().to_string());
            return exit_usage;
        }
        cc::println("shaped-linter: applied {} fix(es) across {} file(s)", all.size(), changed.value());
    }

    return exit_findings;
}
} // namespace

int main(int argc, char const* const* argv)
{
    enable_utf8_console();

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
