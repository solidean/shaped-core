#include "source_root.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>

#if defined(CC_OS_WINDOWS)
#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh>
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
#include <mach-o/dyld.h>
#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)
#include <unistd.h>
#elif defined(CC_OS_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

using namespace cc::primitive_defines;

namespace
{
constexpr char const* k_variable = "SC_SOURCE_ROOT";

#if defined(CC_OS_EMSCRIPTEN)
// A wasm binary runs under node or deno, whose environment emscripten does not forward.
// Its script is the nearest thing to an executable; a browser has neither, and both answer null there.
// Each writes its answer into `out`, up to `capacity` bytes and a terminator, and gives the bytes it needs; 0 for none.
// Nothing is allocated in JS, since a binary need not export malloc.
// clang-format off
EM_JS(int, nx_impl_host_environment_variable, (char const* name, char* out, int capacity), {
    var value = (typeof process !== 'undefined' && process.env) ? process.env[UTF8ToString(name)] : undefined;
    if (!value) return 0;
    if (capacity > 0) stringToUTF8(value, out, capacity);
    return lengthBytesUTF8(value);
});
EM_JS(int, nx_impl_host_script_path, (char* out, int capacity), {
    var path = (typeof process !== 'undefined' && process.argv && process.argv.length > 1) ? process.argv[1] : undefined;
    if (!path) return 0;
    if (capacity > 0) stringToUTF8(path, out, capacity);
    return lengthBytesUTF8(path);
});
// a library function EM_JS calls is linked only where something names it
EM_JS_DEPS(nx_impl_source_root, "$stringToUTF8,$lengthBytesUTF8");
// clang-format on

/// What `ask(out, capacity)` writes, asked once for its size and once more for its bytes.
template <class Ask>
[[nodiscard]] cc::string host_string(Ask const& ask)
{
    auto const length = ask(nullptr, 0);
    if (length <= 0)
        return {};
    auto buffer = cc::vector<char>::create_filled(isize(length) + 1, '\0');
    (void)ask(buffer.data(), int(buffer.size()));
    return cc::string(buffer.data());
}
#endif

[[nodiscard]] cc::optional<cc::string> variable_value()
{
#if defined(CC_OS_EMSCRIPTEN)
    if (auto value = host_string([](char* out, int capacity)
                                 { return nx_impl_host_environment_variable(k_variable, out, capacity); });
        !value.empty())
    {
        // a Windows host names it with backslashes, which emscripten's paths take as part of a name
        for (auto i = isize(0); i < value.size(); ++i)
            if (value[i] == '\\')
                value[i] = '/';
        return value;
    }
#endif
    return cc::environment_variable(k_variable);
}

/// The running executable's own path, or empty where the platform cannot say.
[[nodiscard]] cc::string executable_path()
{
#if defined(CC_OS_WINDOWS)
    auto buffer = cc::vector<wchar_t>::create_filled(32768, L'\0');
    auto const length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (length == 0 || isize(length) >= buffer.size())
        return {};
    return cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(buffer.data()), isize(length)));
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
    auto size = u32(0);
    _NSGetExecutablePath(nullptr, &size);
    auto buffer = cc::vector<char>::create_filled(isize(size) + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    return cc::string(buffer.data());
#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)
    auto buffer = cc::vector<char>::create_filled(4096, '\0');
    auto const length = readlink("/proc/self/exe", buffer.data(), size_t(buffer.size()));
    if (length <= 0 || isize(length) >= buffer.size())
        return {};
    return cc::string(cc::string_view(buffer.data(), isize(length)));
#elif defined(CC_OS_EMSCRIPTEN)
    return host_string([](char* out, int capacity) { return nx_impl_host_script_path(out, capacity); });
#else
    return {};
#endif
}

/// `path` with `/` separators and no trailing one, a root such as `/` or `C:/` excepted.
[[nodiscard]] cc::string normalized(cc::string_view path)
{
    auto out = cc::string(path);
    for (auto i = isize(0); i < out.size(); ++i)
        if (out[i] == '\\')
            out[i] = '/';
    while (out.size() > 1 && out.back() == '/' && !(out.size() == 3 && out[1] == ':'))
        out.resize_down_to(out.size() - 1);
    return out;
}

/// The directory holding `path`, or empty when `path` is a root or names no directory.
[[nodiscard]] cc::string parent_of(cc::string_view path)
{
    auto const slash = path.rfind('/');
    if (slash < 0)
        return {};
    auto parent = normalized(path.subview({.offset = 0, .size = slash == 0 ? 1 : slash}));
    if (parent.size() == 2 && parent[1] == ':')
        parent += '/';
    return parent.size() < path.size() ? parent : cc::string();
}

/// The `CMAKE_HOME_DIRECTORY` that `<directory>/CMakeCache.txt` records, or empty where there is no such file or entry.
[[nodiscard]] cc::string cache_home_directory(cc::string_view directory)
{
    auto const sep = directory.ends_with("/") ? "" : "/";
    auto adapter = cc::file_read_stream_adapter::open(cc::format("{}{}CMakeCache.txt", directory, sep));
    if (adapter.has_error())
        return {};
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return {};
    auto const text = cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());

    // A cache line is `NAME:TYPE=VALUE`; CMake writes this one as `CMAKE_HOME_DIRECTORY:INTERNAL=<dir>`.
    constexpr auto key = cc::string_view("CMAKE_HOME_DIRECTORY:");
    auto rest = text;
    while (!rest.empty())
    {
        auto const end = rest.find('\n');
        auto line = end < 0 ? rest : rest.subview({.offset = 0, .size = end});
        rest = end < 0 ? cc::string_view() : rest.subview(end + 1);
        if (!line.starts_with(key))
            continue;
        auto const equals = line.find('=');
        if (equals < 0)
            return {};
        auto value = line.subview(equals + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value = value.subview({.offset = 0, .size = value.size() - 1});
        return normalized(value);
    }
    return {};
}

[[nodiscard]] cc::string resolve_source_root()
{
    if (auto const value = variable_value(); value.has_value())
        return normalized(value.value());

    auto const executable = normalized(executable_path());
    for (auto directory = parent_of(executable); !directory.empty(); directory = parent_of(directory))
        if (auto home = cache_home_directory(directory); !home.empty())
            return home;

    cc::eprintln("nx::source_root: no source root for {}", executable.empty() ? "this process" : executable);
    CC_ASSERT_ALWAYS(false, "nx::source_root: set SC_SOURCE_ROOT to the top-level source directory, or run the binary "
                            "from inside its build directory, which a CMakeCache.txt above it identifies");
    return {};
}
} // namespace

cc::string_view nx::source_root()
{
    static auto const root = resolve_source_root();
    return root;
}

cc::string nx::source_path(cc::string_view relative)
{
    CC_ASSERT(!relative.starts_with("/") && !relative.starts_with("\\") && !(relative.size() >= 2 && relative[1] == ':'),
              "nx::source_path takes a path relative to the source root");
    auto const root = source_root();
    return normalized(relative.empty() ? cc::string(root) : cc::format("{}/{}", root, relative));
}
