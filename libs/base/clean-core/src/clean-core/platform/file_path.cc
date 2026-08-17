#include <clean-core/common/macros.hh> // CC_OS_WINDOWS
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>

#ifdef CC_OS_WINDOWS

#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh> // utf16_to_utf8 / utf8_to_utf16

#else

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#endif

namespace cc
{
namespace
{
/// Distinguishes two names asked for in the same process, where the pid cannot.
/// Relaxed is enough: uniqueness needs every reader to see a different value, not an order between them.
auto g_temp_counter = cc::atomic<u64>(0);

/// The path separator to join with.
/// Windows accepts '/' everywhere, but a name that ends up in a message reads better in the platform's own spelling.
constexpr char separator()
{
#ifdef CC_OS_WINDOWS
    return '\\';
#else
    return '/';
#endif
}

/// Trims trailing separators so joining never produces a doubled one.
cc::string_view without_trailing_separator(cc::string_view path)
{
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path = path.subview_clamped(0, path.size() - 1);
    return path;
}
} // namespace

#ifdef CC_OS_WINDOWS

cc::string temp_directory_path()
{
    wchar_t buffer[MAX_PATH + 2] = {};
    DWORD const n = ::GetTempPathW(MAX_PATH + 1, buffer);
    if (n == 0 || n > MAX_PATH + 1)
        return cc::string(".");

    auto const utf8 = cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(buffer), isize(n)));
    return cc::string(without_trailing_separator(utf8)); // GetTempPathW always ends in a backslash
}

bool remove_file(cc::string_view path)
{
    auto wpath = cc::utf8_to_utf16(path);
    wpath.push_back(u'\0');

    if (::DeleteFileW(reinterpret_cast<wchar_t const*>(wpath.data())))
        return true;
    return ::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND;
}

namespace
{
u64 current_process_id()
{
    return u64(::GetCurrentProcessId());
}
} // namespace

#else

cc::string temp_directory_path()
{
    // The POSIX convention, in the order the standard tools try them.
    for (auto const* name : {"TMPDIR", "TMP", "TEMP"})
        if (auto const* value = std::getenv(name); value != nullptr && value[0] != '\0')
            return cc::string(without_trailing_separator(cc::string_view(value)));
    return cc::string("/tmp");
}

bool remove_file(cc::string_view path)
{
    auto c_path = cc::string::create_copy_of(path);
    if (std::remove(c_path.c_str_materialize()) == 0)
        return true;
    return errno == ENOENT;
}

namespace
{
u64 current_process_id()
{
    return u64(::getpid());
}
} // namespace

#endif

cc::string temp_file_path(cc::string_view prefix, cc::string_view suffix)
{
    auto const counter = g_temp_counter.fetch_add(1, cc::memory_order_relaxed);
    return cc::format("{}{}{}-{}-{}{}", temp_directory_path(), separator(), prefix, current_process_id(), counter,
                      suffix);
}
} // namespace cc
