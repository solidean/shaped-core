#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::temp_directory_path / cc::temp_file_path / cc::remove_file — where the OS puts scratch files, and how to unmake one
// =========================================================================================================
//
// This is not a filesystem layer: there is no directory creation, no iteration, no metadata and no path arithmetic.
// It answers two questions the OS alone can answer — where scratch files go, and whether one is gone — and leaves reading and writing to the file streams.
//
// It lives under platform/ rather than streams/ because that is what it is: the OS is the whole implementation, and no stream is involved.

namespace cc
{
/// The OS temp directory, without a trailing separator.
/// Falls back to "." where the environment names none, so the result is always usable as a prefix.
[[nodiscard]] cc::string temp_directory_path();

/// A unique path under the temp directory, of the shape "<temp>/<prefix>-<pid>-<counter><suffix>".
///
/// **Creates nothing.** It hands back a name, and the caller decides what to make of it.
/// The counter is process-wide and atomic, so two threads asking at once get different names; two separate
/// processes are separated by the pid instead.
[[nodiscard]] cc::string temp_file_path(cc::string_view prefix, cc::string_view suffix = "");

/// Deletes `path`.
/// True when the file is gone afterwards, which includes it never having been there.
/// False means it exists and would not go — a permission problem, or an open handle on Windows.
bool remove_file(cc::string_view path);
} // namespace cc
