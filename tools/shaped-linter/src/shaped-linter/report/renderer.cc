#include "renderer.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/console.hh>
#include <clean-core/string/format.hh>
#include <shaped-linter/report/impl/display_width.hh>
#include <shaped-linter/report/snippet.hh>
#include <shaped-linter/rules/registry.hh>

#include <algorithm> // std::sort: findings arrive per file in rule order, the report reads top to bottom

namespace scl
{
namespace
{
using cc::console::color;

color color_of(severity sev)
{
    switch (sev)
    {
    case severity::note:
        return color::cyan;
    case severity::warning:
        return color::yellow;
    case severity::error:
        return color::red;
    }
    return color::yellow;
}

cc::string_view rationale_for(cc::string_view rule_id)
{
    for (auto const& r : all_rules())
        if (r.id == rule_id)
            return r.rationale;
    return {};
}

/// A replacement is spelled as it will sit in the file (` = {0}`), which reads badly inside back-ticks.
cc::string_view trim_left(cc::string_view s)
{
    isize a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    return s.subview({.start = a, .end = s.size()});
}

/// Greedy word wrap at `columns` display columns, every line prefixed by `indent`.
/// An explicit newline in `text` stays a break.
cc::string wrap_text(cc::string_view text, i32 columns, cc::string_view indent)
{
    auto out = cc::string();
    auto line = cc::string(indent);
    auto has_word = false;

    auto const flush = [&]
    {
        out += line;
        out += '\n';
        line = cc::string(indent);
        has_word = false;
    };

    isize i = 0;
    while (i < text.size())
    {
        if (text[i] == '\n')
        {
            flush();
            ++i;
            continue;
        }
        if (text[i] == ' ' || text[i] == '\t')
        {
            ++i;
            continue;
        }

        auto const start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n')
            ++i;
        auto const word = text.subview({.start = start, .end = i});

        auto const width = impl::display_column(line, line.size(), 4) + 1 + impl::display_column(word, word.size(), 4);
        if (has_word && width > columns)
            flush();

        if (has_word)
            line += ' ';
        line += word;
        has_word = true;
    }

    if (has_word)
        flush();

    return out;
}

void append_fix(cc::string& out, fix const& f, source_manager const& sm, report_style style)
{
    if (f.edits.empty())
        return;

    auto const label = cc::console::colorize(color::green, "fix:", style.color);

    if (f.edits.size() == 1)
    {
        out += cc::format("  {} replace `{}` with `{}` (applied by --fix)\n", label, sm.span_text(f.edits[0].span),
                          trim_left(f.edits[0].replacement));
        return;
    }

    out += cc::format("  {} {} edits (applied by --fix)\n", label, f.edits.size());
    for (auto const& e : f.edits)
        out += cc::format("       replace `{}` with `{}`\n", sm.span_text(e.span), trim_left(e.replacement));
}

/// A hint is advice: it says what to weigh and never claims to have changed anything.
/// Its replacements always take their own line, because a hint message is a sentence, not a label.
void append_hint(cc::string& out, hint const& h, report_style style)
{
    out += cc::format("  {} {}\n", cc::console::colorize(color::cyan, "help:", style.color), h.message);
    for (auto const& e : h.edits)
        out += cc::format("        consider `{}` (not applied)\n", trim_left(e.replacement));
}

/// A section heading, underlined to its own length.
/// ASCII on purpose: this output is piped into logs whose encoding we do not control.
cc::string section_heading(cc::string_view title, bool colored)
{
    return cc::format("{}\n{}\n", cc::console::colorize(color::bold, title, colored),
                      cc::console::colorize(color::dim, cc::string::create_filled(title.size(), '-'), colored));
}
} // namespace

cc::string render_finding(finding const& f, source_manager const& sm, report_style style)
{
    auto labels = cc::vector<label>();
    labels.push_back({.span = f.span, .text = f.primary_label});
    for (auto const& l : f.secondary)
        labels.push_back(l);

    auto out = cc::format("{} {}\n", cc::console::colorize(color_of(f.sev), cc::format("[{}]", f.rule_id), style.color),
                          cc::console::colorize(color::bold, f.message, style.color));

    out += render_snippet(labels, sm, style);

    if (f.suggested_fix.has_value())
        append_fix(out, f.suggested_fix.value(), sm, style);

    if (f.suggested_hint.has_value())
        append_hint(out, f.suggested_hint.value(), style);

    return out;
}

cc::string render_report(cc::span<finding const> findings, source_manager const& sm, report_style style)
{
    if (findings.empty())
        return {};

    auto order = cc::vector<finding const*>();
    for (auto const& f : findings)
        order.push_back(&f);

    std::sort(order.begin(), order.end(),
              [&](finding const* a, finding const* b)
              {
                  auto const la = sm.resolve(a->span);
                  auto const lb = sm.resolve(b->span);
                  if (la.path != lb.path)
                      return la.path < lb.path;
                  if (la.line != lb.line)
                      return la.line < lb.line;
                  if (la.column != lb.column)
                      return la.column < lb.column;
                  return a->rule_id < b->rule_id;
              });

    auto out = cc::string();
    for (auto const* f : order)
    {
        out += render_finding(*f, sm, style);
        out += '\n';
    }

    // Every rule carries a mandatory rationale; it prints once, however often the rule fired.
    out += section_heading("rule rationale", style.color);
    for (auto const& rule : all_rules())
    {
        auto fired = false;
        for (auto const& f : findings)
            fired = fired || f.rule_id == rule.id;
        if (!fired)
            continue;

        out += cc::format("\n{}\n", cc::console::colorize(color::bold, cc::format("[{}]", rule.id), style.color));
        out += wrap_text(rationale_for(rule.id), style.wrap_columns, "  ");
    }

    auto files = cc::vector<u32>();
    auto fixable = 0;
    for (auto const& f : findings)
    {
        auto seen = false;
        for (auto const id : files)
            seen = seen || id == f.span.file_id;
        if (!seen)
            files.push_back(f.span.file_id);

        if (f.suggested_fix.has_value() && !f.suggested_fix.value().edits.empty())
            ++fixable;
    }

    out += cc::format("\n{} finding{} in {} file{}", findings.size(), findings.size() == 1 ? "" : "s", files.size(),
                      files.size() == 1 ? "" : "s");
    out += fixable > 0 ? cc::format(" ({} fixable with --fix)\n", fixable) : cc::string("\n");

    return out;
}
} // namespace scl
