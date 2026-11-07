#include "debug_sites.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::emit;
using namespace sgl::emit::impl;

namespace
{
/// FNV-1a over `text`, continuing from `hash`.
[[nodiscard]] u32 hashed(u32 hash, cc::string_view text)
{
    for (auto const c : text)
        hash = (hash ^ u32(u8(c))) * 16777619u;
    return (hash ^ 0xFFu) * 16777619u;
}

/// The family and lanes of a printable type (CHK-424): `float3` is `float` in 3, and an enum one `int`.
[[nodiscard]] debug_value debug_value_of(plan const& p, type_id type)
{
    auto result = debug_value{.family = "int", .type = cc::string(p.m.name_of(type))};
    // the prelude declares `bool` an enum, which is no enum a message writes as its int
    if (p.m.at(type).kind == check::type_kind::enumeration && result.type != "bool")
        return result;
    auto name = cc::string_view(result.type);
    if (!name.empty() && name[name.size() - 1] >= '2' && name[name.size() - 1] <= '4')
    {
        result.lanes = name[name.size() - 1] - '0';
        name = name.subview({.offset = 0, .size = name.size() - 1});
    }
    result.family = cc::string(name);
    return result;
}

[[nodiscard]] debug_site site_of(plan const& p, source_lines& lines, flat_stmt const& s, flat_print const& n)
{
    auto site = debug_site();
    if (n.message >= 0 && n.message < p.e.messages.size())
    {
        site.pieces = p.e.messages[n.message].pieces;
        site.is_assert = p.e.messages[n.message].is_assert;
    }
    if (!lines.sources.files.empty())
        site.origin = lines.origin_of(p.e, s);
    for (auto const id : p.e.at(n.values))
        site.values.push_back(debug_value_of(p, p.e.at(id).type));

    auto id = hashed(2166136261u, p.entry_name);
    id = hashed(id, cc::format("{}", n.message));
    for (auto const& piece : site.pieces)
        id = hashed(id, piece);
    if (site.origin.at.file >= 0)
        id = hashed(id, cc::format("{}:{}", lines.name_of(site.origin.at.file), site.origin.at.line));
    // one line inlined along two call chains is two sites, which another permutation of this entry point may hold
    for (auto const& from : site.origin.inlined_from)
        if (from.file >= 0)
            id = hashed(id, cc::format("<{}:{}", lines.name_of(from.file), from.line));
    site.id = id;
    return site;
}
} // namespace

source_line source_lines::line_of(i32 file, ast::expr_id expr, ast::stmt_id stmt)
{
    if (file < 0 || file >= sources.files.size())
        return {};
    auto const& f = sources.files[file];
    auto const form = ast::is_valid(expr) ? f.ast.at(expr).form
                    : ast::is_valid(stmt) ? f.ast.at(stmt).form
                                          : form_id::none;
    if (!is_valid(form))
        return {};

    if (line_starts.size() < sources.files.size())
        line_starts.resize_to_defaulted(sources.files.size());
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
    auto const offset = f.file.at(form).where.offset;
    auto line = isize(1);
    while (line < starts.size() && starts[line] <= offset)
        ++line;
    return {.file = file, .line = i32(line)};
}

line_origin source_lines::origin_of(flat_entry_point const& e, flat_stmt const& s)
{
    // innermost first: the statement, then its calls from the inside out
    auto frames = cc::vector<source_line>();
    frames.push_back(line_of(s.from.file, s.from.expr, s.from.stmt));
    auto const calls = e.at(s.inlined_through);
    for (auto i = calls.size(); i > 0; --i)
        frames.push_back(line_of(calls[i - 1].file, calls[i - 1].call, ast::stmt_id::none));

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
    return file >= 0 && file < sources.names.size() ? sources.names[file] : cc::string_view("<sgl>");
}

void sgl::emit::impl::plan_debug_sites(plan& p, bool writes, options const& sources)
{
    p.debug_site_of.resize_to_filled(p.e.stmts.size(), -1);
    if (!writes)
        return;
    auto lines = source_lines{.m = p.m, .sources = sources};
    for (auto i = isize(0); i < p.e.stmts.size(); ++i)
        if (auto const* const n = p.e.stmts[i].node.try_as<flat_print>())
        {
            p.debug_site_of[i] = i32(p.debug_sites.size());
            p.debug_sites.push_back(site_of(p, lines, p.e.stmts[i], *n));
        }
    p.writes_debug = !p.debug_sites.empty();
}
