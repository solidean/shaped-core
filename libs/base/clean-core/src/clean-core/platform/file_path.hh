#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::temp_directory_path / cc::temp_file_path / cc::user_config_directory_path / cc::remove_file
// — where the OS puts scratch files and a user's settings, and how to unmake a file
// =========================================================================================================
//
// This is not a filesystem layer: there is no directory creation, no iteration, no metadata and no path arithmetic.
// It answers what the OS alone can answer — where scratch files go, where a user's settings go, and whether a file is gone — and leaves reading and writing to the file streams.
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

/// The directory the OS keeps a user's application settings in, without a trailing separator.
///
/// `%APPDATA%` on Windows, `~/Library/Application Support` on macOS, and elsewhere `$XDG_CONFIG_HOME`, or `~/.config` when that is unset or relative.
/// None where the platform has no such place, as on the web and Android, or where the environment names none.
/// The OS makes every one of them but `~/.config`, which a fresh account may not have yet.
/// Shared by every application of the user: name a file in it after the application, or make a folder of its own.
[[nodiscard]] cc::optional<cc::string> user_config_directory_path();

/// Deletes `path`.
/// True when the file is gone afterwards, which includes it never having been there.
/// False means it exists and would not go — a permission problem, or an open handle on Windows.
bool remove_file(cc::string_view path);
} // namespace cc
