#include "format_diagnostic.hh"

#include <clean-core/string/format.hh>

sgl::line_column sgl::line_column_of(cc::string_view source, u32 offset)
{
    auto const end = isize(offset) < source.size() ? isize(offset) : source.size();

    auto result = line_column();
    for (auto i = isize(0); i < end; ++i)
    {
        if (source[i] == '\n')
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
    if (!detail.empty())
        out.appendf(": {}", detail);
    return out;
}
