#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/cli/changed_lines.hh>
#include <shaped-linter/cli/options.hh>
#include <shaped-linter/lex/source_manager.hh>
#include <shaped-linter/prose/apply.hh>
#include <shaped-linter/prose/plan.hh>
#include <shaped-linter/report/reporter.hh>
#include <shaped-linter/rules/engine.hh>
#include <shaped-linter/rules/registry.hh>

#if defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#endif

using namespace cc::primitive_defines;

namespace
{
// 0 = ran clean (no findings), 1 = bad usage / IO error, 2 = findings reported.
constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_findings = 2;

/// Make the console interpret our output (already UTF-8 bytes from cc::print) as UTF-8.
/// Without it the repo's typography and any UTF-8 in echoed source lines come out as codepage mojibake.
/// A no-op when output is redirected — the pipe carries the same UTF-8 bytes, which the reader decodes as UTF-8.
void enable_utf8_console()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
}

/// Whether a rule id belongs to a prose rule — the only findings `--changed-lines` may drop.
bool is_prose_rule(cc::string_view rule_id)
{
    for (auto const& r : scl::all_rules())
        if (r.id == rule_id)
            return r.layer == scl::rule_layer::prose;
    return false;
}

int run(scl::options const& opts)
{
    scl::changed_lines scope;
    if (!opts.changed_lines_path.empty())
    {
        scl::source_manager spec_sm;
        auto spec = spec_sm.add_from_file(opts.changed_lines_path);
        if (spec.has_error())
        {
            cc::eprintln("error: cannot read {}: {}", opts.changed_lines_path, spec.error().to_string());
            return exit_usage;
        }

        auto parsed = scl::parse_changed_lines(spec.value()->text());
        if (parsed.has_error())
        {
            cc::eprintln("error: {}", parsed.error().to_string());
            return exit_usage;
        }
        scope = cc::move(parsed.value());
    }

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
        {
            // Prose findings outside the changed lines are somebody else's older prose, not this edit's.
            if (!scope.empty() && is_prose_rule(f.rule_id)
                && !scope.covers(file, sm.buffer(f.span.file_id).line_col_at(f.span.byte_begin).line))
                continue;

            all.push_back(cc::move(f));
        }
    }

    if (all.empty())
    {
        cc::println("shaped-linter: no findings in {} file(s)", opts.files.size());
        return exit_ok;
    }

    scl::report_findings(all, sm, {.color = cc::console::color_enabled()});

    if (opts.apply_fixes)
    {
        auto fixes = 0;
        for (auto const& f : all)
            if (f.suggested_fix.has_value() && !f.suggested_fix.value().edits.empty())
                ++fixes;

        auto const changed = scl::apply_fixes(sm, all);
        if (changed.has_error())
        {
            cc::eprintln("error applying fixes: {}", changed.error().to_string());
            return exit_usage;
        }
        cc::println("shaped-linter: applied {} fix(es) across {} file(s)", fixes, changed.value());
    }

    return exit_findings;
}

/// Right-align `text` in `width` columns.
cc::string aligned(cc::string_view text, isize width)
{
    cc::string out;
    for (auto i = text.size(); i < width; ++i)
        out += ' ';
    out += text;
    return out;
}

/// A delta always carries its sign, which is what makes the column read as a change rather than a count.
cc::string signed_text(isize value)
{
    return value > 0 ? cc::format("+{}", value) : cc::to_string(value);
}

/// One row of the prose delta table, rendered to text up front so the columns can be sized before printing.
struct stats_row
{
    cc::string lines_before;
    cc::string lines_after;
    cc::string lines_delta;
    cc::string words_before;
    cc::string words_after;
    cc::string words_delta;
    cc::string label;
};

stats_row row_of(scl::prose_stats before, scl::prose_stats after, cc::string_view label)
{
    return {
        .lines_before = cc::to_string(before.lines),
        .lines_after = cc::to_string(after.lines),
        .lines_delta = signed_text(after.lines - before.lines),
        .words_before = cc::to_string(before.words),
        .words_after = cc::to_string(after.words),
        .words_delta = signed_text(after.words - before.words),
        .label = cc::string(label),
    };
}

/// Print `--stats`: one row per file in plan order, then the total.
/// Every column is sized from the widest entry, the total row included, so the numbers line up down the block.
void print_prose_stats(cc::span<scl::file_prose_delta const> files)
{
    if (files.empty())
        return;

    scl::prose_stats before;
    scl::prose_stats after;
    cc::vector<stats_row> rows;

    for (auto const& f : files)
    {
        before.lines += f.before.lines;
        before.words += f.before.words;
        after.lines += f.after.lines;
        after.words += f.after.words;
        rows.push_back(row_of(f.before, f.after, f.path));
    }
    rows.push_back(row_of(before, after, cc::format("total, {} file(s)", files.size())));

    auto w_lines = isize(0);
    auto w_words = isize(0);
    auto w_lines_delta = isize(0);
    auto w_words_delta = isize(0);
    for (auto const& r : rows)
    {
        w_lines = cc::max({w_lines, r.lines_before.size(), r.lines_after.size()});
        w_words = cc::max({w_words, r.words_before.size(), r.words_after.size()});
        w_lines_delta = cc::max(w_lines_delta, r.lines_delta.size());
        w_words_delta = cc::max(w_words_delta, r.words_delta.size());
    }

    cc::println("prose lines and words, before -> after:");
    for (auto const& r : rows)
        cc::println("  {} -> {} ({})   {} -> {} ({})   {}", aligned(r.lines_before, w_lines),
                    aligned(r.lines_after, w_lines), aligned(r.lines_delta, w_lines_delta),
                    aligned(r.words_before, w_words), aligned(r.words_after, w_words),
                    aligned(r.words_delta, w_words_delta), r.label);
}

int run_prose_apply(scl::prose_apply_options const& opts)
{
    scl::source_manager sm;
    auto plan_text = sm.add_from_file(opts.plan_path);
    if (plan_text.has_error())
    {
        cc::eprintln("error: cannot read {}: {}", opts.plan_path, plan_text.error().to_string());
        return exit_usage;
    }

    // to_string() already carries the `error:` prefix and the site — do not prefix it again.
    auto plan = scl::parse_prose_plan(plan_text.value()->text());
    if (plan.has_error())
    {
        cc::eprintln("{}", plan.error().to_string());
        return exit_usage;
    }

    // Paths in a plan are repo-relative and dev.py runs us from the repo root, so the cwd is the root.
    auto const report = scl::apply_prose_plan(plan.value(), "", {.dry_run = opts.dry_run, .stats = opts.stats});
    if (report.has_error())
    {
        cc::eprintln("{}", report.error().to_string());
        return exit_usage;
    }

    cc::println("shaped-linter: {} {} edit(s) across {} file(s)", opts.dry_run ? "validated" : "applied",
                report.value().edits_applied, report.value().files_changed);
    print_prose_stats(report.value().prose);
    return exit_ok;
}

/// Dispatch `shaped-linter prose apply …`. `args` is the full argv.
int run_prose_command(cc::span<char const* const> args)
{
    if (args.size() < 3 || cc::string_view(args[2]) != "apply")
    {
        cc::eprintln("error: the only prose subcommand is 'apply'");
        cc::eprint(scl::prose_apply_usage_text());
        return exit_usage;
    }

    auto opts = scl::parse_prose_apply_options(args.subspan(3));
    cc::console::configure(opts.has_value() ? opts.value().color : cc::console::color_mode::automatic);

    if (opts.has_error())
    {
        cc::eprintln("error: {}", opts.error().to_string());
        cc::eprint(scl::prose_apply_usage_text());
        return exit_usage;
    }

    if (opts.value().help)
    {
        cc::print(scl::prose_apply_usage_text());
        return exit_ok;
    }

    return run_prose_apply(opts.value());
}
} // namespace

int main(int argc, char const* const* argv)
{
    enable_utf8_console();

    auto const args = cc::span<char const* const>(argv, isize(argc));

    // The one verb the tool has.
    // Everything else is the flat lint command it has always been.
    if (args.size() >= 2 && cc::string_view(args[1]) == "prose")
        return run_prose_command(args);

    auto opts = scl::parse_options(args);

    // Resolve color before the first byte of output, including the usage error below.
    // A parse failure has no options to read, so that path auto-detects.
    cc::console::configure(opts.has_value() ? opts.value().color : cc::console::color_mode::automatic);

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
