#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/source/diagnostic.hh>

/// A place in a source text as an editor names it: both are 1-based, and the column counts bytes.
struct sgl::line_column
{
    i32 line = 1;
    i32 column = 1;

    constexpr bool operator==(line_column const&) const = default;
};

namespace sgl
{
/// `offset` may be `source.size()`, which is where a diagnostic at end of file points; a larger one is clamped to it.
/// A `\r\n` is one line end, and the `\n` of it is the last column of its line.
[[nodiscard]] line_column line_column_of(cc::string_view source, u32 offset);

/// One diagnostic as one line a terminal and an editor both read: `cube.sgl:12:5: error: unknown-name: foo`.
///
/// `source` must be the text `d.where` points into.
/// `detail` is what the kind alone cannot say, and is left out when empty.
[[nodiscard]] cc::string format_diagnostic(cc::string_view file_name,
                                           cc::string_view source,
                                           diagnostic const& d,
                                           cc::string_view detail = {});

/// A second place a diagnostic points at, as the line after it: `cube.sgl:3:1: note: declared here`.
/// `source` must be the text `where` points into.
[[nodiscard]] cc::string format_note(cc::string_view file_name,
                                     cc::string_view source,
                                     source_span where,
                                     cc::string_view message);
} // namespace sgl
