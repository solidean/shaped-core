#include "format_diagnostic.hh"

#include <clean-core/string/format.hh>

sgl::line_column sgl::line_column_of(cc::string_view source, u32 offset)
{
    auto const end = isize(offset) < source.size() ? isize(offset) : source.size();

    auto result = line_column();
    for (auto i = isize(0); i < end; ++i)
    {
        auto const is_bare_cr = source[i] == '\r' && (i + 1 >= source.size() || source[i + 1] != '\n');
        if (source[i] == '\n' || is_bare_cr)
        {
            ++result.line;
            result.column = 1;
        }
        else
            ++result.column;
    }
    return result;
}

cc::string sgl::format_diagnostic(cc::string_view file_name,
                                  cc::string_view source,
                                  diagnostic const& d,
                                  cc::string_view detail)
{
    auto const at = line_column_of(source, d.where.offset);
    auto out = cc::format("{}:{}:{}: {}: {}", file_name, at.line, at.column,
                          d.level == severity::warning ? "warning" : "error", to_string(d.kind));
    out.appendf(": {}", detail.empty() ? summary_of(d.kind) : detail);
    return out;
}

cc::string sgl::format_note(cc::string_view file_name, cc::string_view source, source_span where, cc::string_view message)
{
    auto const at = line_column_of(source, where.offset);
    return cc::format("{}:{}:{}: note: {}", file_name, at.line, at.column, message);
}
