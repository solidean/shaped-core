#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// Reading and picking apart the small text files the platform layer queries.
///
/// Internal to clean-core's platform code, and shaped by what /proc, /sys and /etc actually look like: a handful of
/// short files, each a line or a `key: value` list, read once and thrown away.
///
/// **cc has no whitespace-trimming or line-splitting helper of its own yet**, which is the only reason `trimmed` and
/// `next_line` live here rather than on cc::string_view.
/// They belong there; move them when that lands, and delete this half of the header.

namespace cc::impl
{
/// The whole file as text, or nothing when it cannot be read.
///
/// Reads until the stream ends rather than trusting a length, because /proc and /sys report a size of zero for files
/// that are not empty.
[[nodiscard]] cc::optional<cc::string> read_text_file(cc::string_view path);

/// The same, with surrounding whitespace removed and an empty result reported as absent.
/// Almost every sysfs file is one value and a trailing newline, so this is the common reader.
[[nodiscard]] cc::optional<cc::string> read_trimmed_file(cc::string_view path);

/// The file's contents parsed as one integer, or nothing when it is missing or is not a number.
[[nodiscard]] cc::optional<i64> read_int_file(cc::string_view path);

[[nodiscard]] cc::string_view trimmed(cc::string_view s);

/// The next line, and advances `rest` past it.
/// Returns false once nothing is left.
bool next_line(cc::string_view& rest, cc::string_view& out);

/// The value of one `key: value` or `key=value` line out of a whole file's text.
/// Surrounding double quotes are stripped, which is what /etc/os-release needs.
[[nodiscard]] cc::optional<cc::string> field_from(cc::string_view text, cc::string_view key, char separator);
} // namespace cc::impl
