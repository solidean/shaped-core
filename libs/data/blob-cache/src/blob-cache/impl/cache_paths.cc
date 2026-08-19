#include <blob-cache/fwd.hh> // the bare sized aliases, inside namespace bcache
#include <blob-cache/impl/cache_paths.hh>
#include <clean-core/common/macros.hh> // CC_OS_WINDOWS / CC_OS_MACOS
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/container/vector.hh>
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh> // utf8_to_utf16 — a user profile path is not necessarily ASCII

#else

#include <sys/stat.h>

#endif

namespace bcache::impl
{
namespace
{
bool is_separator(char c)
{
    return c == '/' || c == '\\';
}

cc::string_view without_trailing_separator(cc::string_view path)
{
    while (path.size() > 1 && is_separator(path.back()))
        path = path.subview_clamped(0, path.size() - 1);
    return path;
}

#ifdef CC_OS_WINDOWS

/// The wide, NUL-terminated form the Win32 entry points need.
cc::vector<char16_t> wide_path(cc::string_view path)
{
    auto wide = cc::utf8_to_utf16(path);
    wide.push_back(u'\0');
    return wide;
}

bool directory_exists(cc::string_view path)
{
    auto const wide = wide_path(path);
    auto const attributes = ::GetFileAttributesW(reinterpret_cast<wchar_t const*>(wide.data()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool create_one(cc::string_view path)
{
    auto const wide = wide_path(path);
    if (::CreateDirectoryW(reinterpret_cast<wchar_t const*>(wide.data()), nullptr) != 0)
        return true;

    // ALREADY_EXISTS is the ordinary case, but a drive or share root reports ACCESS_DENIED instead.
    // So ask whether the directory is there rather than classifying the error.
    return directory_exists(path);
}

#else

bool create_one(cc::string_view path)
{
    auto const terminated = cc::string::create_copy_c_str_materialized(path);
    if (::mkdir(terminated.c_str_if_terminated(), 0755) == 0)
        return true;

    struct stat info = {};
    return ::stat(terminated.c_str_if_terminated(), &info) == 0 && S_ISDIR(info.st_mode);
}

#endif
} // namespace

cc::string user_cache_directory()
{
#if defined(CC_OS_WINDOWS)
    if (auto const local = cc::environment_variable("LOCALAPPDATA"); local.has_value())
        return cc::string(without_trailing_separator(local.value()));
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
    if (auto const home = cc::environment_variable("HOME"); home.has_value())
        return cc::format("{}/Library/Caches", without_trailing_separator(home.value()));
#else
    if (auto const xdg = cc::environment_variable("XDG_CACHE_HOME"); xdg.has_value())
        return cc::string(without_trailing_separator(xdg.value()));
    if (auto const home = cc::environment_variable("HOME"); home.has_value())
        return cc::format("{}/.cache", without_trailing_separator(home.value()));
#endif
    return cc::temp_directory_path();
}

bool create_directories(cc::string_view path)
{
    path = without_trailing_separator(path);
    if (path.empty())
        return false;

    // Ancestors first, shortest to longest: creating the leaf alone fails whenever its parent is missing too.
    // An ancestor that will not be created is not reported here — it surfaces as the leaf failing, which is the answer the caller wants.
    for (isize i = 1; i < path.size(); ++i)
        if (is_separator(path[i]))
            (void)create_one(path.subview_clamped(0, i));

    return create_one(path);
}
} // namespace bcache::impl
