#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_span.hh>

namespace scl
{
/// One source file: owns its bytes and a line-start index, and maps a byte offset to a 1-based
/// line/column. The text is NOT null-terminated (clean-core string semantics); do not pass `.data()`
/// to C APIs. Construction scans the text once to build the line index.
struct source_buffer
{
    /// A buffer over in-memory text (tests, snippets) — no file IO. `path` is used only for reporting.
    static source_buffer from_text(cc::string text, cc::string_view path, u32 file_id);

    cc::string_view text() const { return _text; }
    cc::string_view path() const { return _path; }
    u32 file_id() const { return _file_id; }

    /// The bytes covered by `s`. `s` must refer to this buffer's file and be within bounds.
    cc::string_view span_text(source_span s) const;

    /// The 1-based line/column of a byte offset. `offset` is clamped to `[0, size]`.
    line_col line_col_at(u32 byte_offset) const;

    /// The text of the line containing `byte_offset`, without its trailing newline. For reporting.
    cc::string_view line_text(u32 byte_offset) const;

private:
    cc::string _text;
    cc::string _path;
    u32 _file_id = 0;
    cc::vector<u32> _line_starts; // byte offset of each line's first char; always begins with 0

    void build_line_index();
};
} // namespace scl
