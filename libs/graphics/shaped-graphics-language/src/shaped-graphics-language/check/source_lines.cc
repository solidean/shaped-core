#include "source_lines.hh"

#include <shaped-graphics-language/check/check.hh>

using namespace sgl;
using namespace sgl::check;

source_line source_lines::line_of(i32 file, ast::expr_id expr, ast::stmt_id stmt)
{
    if (file < 0 || file >= files.size())
        return {};
    auto const& f = files[file];
    auto const form = ast::is_valid(expr) ? f.ast.at(expr).form
                    : ast::is_valid(stmt) ? f.ast.at(stmt).form
                                          : form_id::none;
    if (!is_valid(form))
        return {};
    return line_at(file, f.file.at(form).where.offset);
}

source_line source_lines::line_at(i32 file, u32 offset)
{
    if (file < 0 || file >= files.size())
        return {};
    auto const& f = files[file];
    if (line_starts.size() < files.size())
        line_starts.resize_to_defaulted(files.size());
    auto& starts = line_starts[file];
    if (starts.empty())
    {
        // a line ends at `\n`, `\r\n` or a bare `\r`
        auto const text = cc::string_view(f.file.source);
        starts.push_back(0);
        for (auto i = isize(0); i < text.size(); ++i)
            if (text[i] == '\n' || (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')))
                starts.push_back(u32(i + 1));
    }
    auto line = isize(1);
    while (line < starts.size() && starts[line] <= offset)
        ++line;
    return {.file = file, .line = i32(line)};
}

line_origin source_lines::origin_of(flat_entry_point const& e, flat_stmt const& s)
{
    // innermost first: the statement, then its calls from the inside out
    // a call stands in the function the call before it called, the outermost in the entry point's
    auto const calls = e.at(s.inlined_through);
    auto const callee_of = [&](isize i) -> symbol_id
    {
        if (i < 0)
            return e.function;
        auto const& site = calls[i];
        if (site.file < 0 || site.file >= m.files.size() || !ast::is_valid(site.call))
            return symbol_id::none;
        // a call in a generic function's instance stands past the file's own tables, and names no function here
        auto const& calls_of = m.files[site.file].call_of;
        if (ast::index_of(site.call) >= calls_of.size())
            return symbol_id::none;
        auto const record = calls_of[ast::index_of(site.call)];
        return record >= 0 && record < m.call_records.size() ? m.call_records[record].callee : symbol_id::none;
    };
    auto frames = cc::vector<source_line>();
    frames.push_back(line_of(s.from.file, s.from.expr, s.from.stmt));
    frames.back().function = callee_of(calls.size() - 1);
    for (auto i = calls.size(); i > 0; --i)
    {
        frames.push_back(line_of(calls[i - 1].file, calls[i - 1].call, ast::stmt_id::none));
        frames.back().function = callee_of(i - 2);
    }

    auto const is_prelude = [&](source_line const& l) { return l.file >= 0 && l.file < m.prelude_files; };
    auto any_outside = false;
    for (auto const& f : frames)
        any_outside = any_outside || (f.file >= 0 && !is_prelude(f));
    auto result = line_origin();
    for (auto const& f : frames)
    {
        if (f.file < 0 || (any_outside && is_prelude(f)))
            continue;
        if (result.at.file < 0)
            result.at = f;
        else
            result.inlined_from.push_back(f);
    }
    return result;
}

cc::string_view source_lines::name_of(i32 file) const
{
    return file >= 0 && file < names.size() ? names[file] : cc::string_view("<sgl>");
}
