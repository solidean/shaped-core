#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

/// Mapping source bytes to terminal columns, so an underline lands under what it underlines.
///
/// Temporary and deliberately local: this is codepoint counting, not display width.
/// A codepoint is assumed to be one column — wrong for CJK and emoji, right for the identifiers and punctuation a C++ linter underlines.
/// Real display width needs grapheme clusters and East-Asian-width tables, which clean-core lists as an explicit non-goal.
/// If a second consumer shows up, that is the moment to grow a `cc::` API rather than to widen this.
namespace scl::impl
{
/// The line with each tab replaced by spaces up to the next multiple of `tab_width`.
/// Returns the line unchanged when it holds no tab, which is the common case.
cc::string expand_tabs(cc::string_view line, i32 tab_width);

/// The 0-based column at which the byte at `byte_offset` within `line` is displayed, after the same tab expansion `expand_tabs` performs.
/// `byte_offset` is clamped to `[0, line.size()]`.
i32 display_column(cc::string_view line, isize byte_offset, i32 tab_width);
} // namespace scl::impl
