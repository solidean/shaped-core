#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// The two OS questions a default cache has to answer, and that clean-core deliberately does not.
/// clean-core's platform/file_path.hh is explicit that it is not a filesystem layer, so this is the minimum bcache adds on top of it.

namespace bcache::impl
{
/// Where this user's cached data belongs, without a trailing separator.
/// %LOCALAPPDATA% on Windows, $XDG_CACHE_HOME or ~/.cache on Linux, ~/Library/Caches on Apple platforms.
/// Falls back to cc::temp_directory_path() where the environment names none, so the result is always usable as a prefix.
[[nodiscard]] cc::string user_cache_directory();

/// Creates `path` and every missing ancestor.
/// True when the directory exists afterwards, which includes it having existed all along.
[[nodiscard]] bool create_directories(cc::string_view path);
} // namespace bcache::impl
