#pragma once

#include "fwd.hh"
#include "types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>

/// Where every line of a text starts, and the conversion between byte offsets and LSP positions.
///
/// Lines end at `\n`, `\r\n` and a bare `\r`, which is how both LSP and SGL's line tree end them.
/// Within a line a character is counted in the negotiated encoding: bytes for UTF-8, code units for UTF-16, where a
/// character outside the basic plane is two.
/// The text is not held: every call takes the same text the index was built from.
class lsp::text_index
{
public:
    /// The index of the empty text.
    text_index() : text_index(cc::string_view()) {}
    explicit text_index(cc::string_view text);

    [[nodiscard]] isize line_count() const { return _line_starts.size(); }

    /// The position of byte `offset`; one past the end is the end of the text, and larger offsets clamp to it.
    [[nodiscard]] position position_of(cc::string_view text, isize offset, position_encoding e) const;

    /// The byte offset of `p`; a line past the end is the end of the text, and a character past its line's end is the
    /// line's end, as LSP asks a server to treat them.
    [[nodiscard]] isize offset_of(cc::string_view text, position p, position_encoding e) const;

private:
    /// The byte offset each line starts at; never empty, since even an empty text has one line.
    cc::vector<isize> _line_starts;

    /// The byte offset the content of `line` ends at, its terminator excluded.
    [[nodiscard]] isize line_end(cc::string_view text, isize line) const;
};
