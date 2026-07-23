#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
/// A half-open byte range `[byte_begin, byte_end)` within one source file.
/// Small value type: line/column are NEVER stored here — they are resolved lazily from the buffer
/// only when a finding is reported. This is the backbone of macro-placement rules and future fix-its:
/// every token and every syntax node carries a source_span, and a later macro-expansion table keys off
/// the same spans (the span always stays the *spelling* location).
struct source_span
{
    u32 file_id = 0;
    u32 byte_begin = 0;
    u32 byte_end = 0;

    bool empty() const { return byte_end <= byte_begin; }
    u32 size() const { return byte_end > byte_begin ? byte_end - byte_begin : 0; }

    /// The smallest span covering both `a` and `b`. They must share a file_id.
    static source_span join(source_span a, source_span b)
    {
        return {
            .file_id = a.file_id,
            .byte_begin = a.byte_begin < b.byte_begin ? a.byte_begin : b.byte_begin,
            .byte_end = a.byte_end > b.byte_end ? a.byte_end : b.byte_end,
        };
    }

    bool operator==(source_span const&) const = default;
};

/// A 1-based line and column. Column is byte-based (not UTF-8 codepoint-based) — fine for diagnostics.
struct line_col
{
    u32 line = 1;
    u32 column = 1;

    bool operator==(line_col const&) const = default;
};

/// A fully resolved location for reporting: the file path plus 1-based line/column.
struct resolved_location
{
    cc::string_view path;
    u32 line = 1;
    u32 column = 1;
};
} // namespace scl
