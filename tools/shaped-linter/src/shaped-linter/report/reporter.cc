#include "reporter.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/rules/registry.hh>

namespace scl
{
namespace
{
cc::string_view rationale_for(cc::string_view rule_id)
{
    for (auto const& r : all_rules())
        if (r.id == rule_id)
            return r.rationale;
    return {};
}

/// The findings for `rule_id`, in their original order.
cc::vector<finding const*> group_for(cc::span<finding const> findings, cc::string_view rule_id)
{
    cc::vector<finding const*> out;
    for (auto const& f : findings)
        if (f.rule_id == rule_id)
            out.push_back(&f);
    return out;
}

cc::string_view trim_left(cc::string_view s)
{
    isize a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    return s.subview({.start = a, .end = s.size()});
}

struct palette
{
    cc::string_view bold;
    cc::string_view dim;
    cc::string_view reset;
};

palette make_palette(bool color)
{
    if (color)
        return {.bold = "\x1b[1m", .dim = "\x1b[2m", .reset = "\x1b[0m"};
    return {.bold = "", .dim = "", .reset = ""};
}
} // namespace

void report_findings(cc::span<finding const> findings, source_manager const& sm, report_options opts)
{
    if (findings.empty())
        return;

    auto const p = make_palette(opts.color);

    // Group by rule in registry order, so output is stable and every rule's rationale leads its group.
    for (auto const& rule : all_rules())
    {
        auto const group = group_for(findings, rule.id);
        if (group.empty())
            continue;

        cc::println("{}{} ({} finding{}){}", p.bold, rule.id, group.size(), group.size() == 1 ? "" : "s", p.reset);
        cc::println("{}  why: {}{}", p.dim, rationale_for(rule.id), p.reset);

        for (auto const* f : group)
        {
            auto const loc = sm.resolve(f->span);
            cc::println("  {}:{}:{}: {} [{}]", loc.path, loc.line, loc.column, f->message, f->rule_id);
            cc::println("{}    {}{}", p.dim, sm.buffer(f->span.file_id).line_text(f->span.byte_begin), p.reset);

            if (f->suggested_fix.has_value() && !f->suggested_fix.value().edits.empty())
            {
                auto const& e = f->suggested_fix.value().edits[0];
                cc::println("{}    fix: replace `{}` with `{}`{}", p.dim, sm.span_text(f->span),
                            trim_left(e.replacement), p.reset);
            }
        }
        cc::println();
    }
}
} // namespace scl
