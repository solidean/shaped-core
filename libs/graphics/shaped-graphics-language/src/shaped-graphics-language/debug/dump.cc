#include "dump.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>

namespace
{
using namespace sgl;

cc::string_view name_of(line_kind kind)
{
    switch (kind)
    {
    case line_kind::blank:
        return "blank";
    case line_kind::code:
        return "code";
    case line_kind::comment:
        return "comment";
    case line_kind::string_content:
        return "string_content";
    }
    CC_UNREACHABLE("unknown line_kind");
}

template <class F>
void walk(parsed_file const& file, line_id first, int depth, F&& visit)
{
    for (auto id = first; is_valid(id); id = file.at(id).next_sibling)
    {
        visit(file.at(id), depth);
        walk(file, file.at(id).first_child, depth + 1, visit);
    }
}

void begin_line(cc::string& out, line const& l, int depth)
{
    for (auto i = 0; i < depth; ++i)
        out += "  ";
    out += name_of(l.kind);
}
} // namespace

cc::string sgl::dump_lines(parsed_file const& file)
{
    auto out = cc::string();
    walk(file, file.first_line, 0,
         [&](line const& l, int depth)
         {
             begin_line(out, l, depth);
             if (l.kind != line_kind::blank)
             {
                 out += ": ";
                 out += file.text_of({.offset = l.text.offset + l.indent_bytes, .length = l.text.length - l.indent_bytes});
             }
             out += "\n";
         });
    return out;
}

cc::string sgl::dump_tokens(parsed_file const& file)
{
    auto out = cc::string();
    walk(file, file.first_line, 0,
         [&](line const& l, int depth)
         {
             begin_line(out, l, depth);
             if (l.kind != line_kind::blank)
                 out += ":";
             for (auto t = l.tokens_begin(); t < l.tokens_end(); t = next(t))
             {
                 out += t > l.tokens_begin() && file.is_fused_left(t) ? "~" : " ";
                 out.appendf("{}({})", to_string(file.at(t).kind), file.text_of(file.at(t).where));
             }
             out += "\n";
         });
    return out;
}

cc::string sgl::dump_diagnostics(parsed_file const& file)
{
    auto out = cc::string();
    for (auto const& d : file.diagnostics)
        out.appendf("{} @{}+{}\n", to_string(d.kind), d.where.offset, d.where.length);
    return out;
}

cc::string sgl::print_source(parsed_file const& file)
{
    auto out = cc::string();
    for (auto const& l : file.lines)
        out += file.text_of({.offset = l.text.offset, .length = l.text.length + l.terminator_length});
    return out;
}
