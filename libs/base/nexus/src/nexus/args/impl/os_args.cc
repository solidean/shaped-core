#include "os_args.hh"

#include <clean-core/common/macros.hh>

#if defined(CC_OS_WINDOWS)
#include <cstdlib> // __argc / __argv
#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
#include <crt_externs.h>
#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)
#include <clean-core/streams/file_stream.hh>
#endif

// One branch per platform, each answering the same question three different ways.
// Emscripten and WASI fall through to the empty answer: a browser tab has no command line, and pretending
// otherwise would only move the surprise somewhere less obvious.

namespace
{
using namespace cc::primitive_defines;

cc::vector<cc::string> query_os_args()
{
    auto out = cc::vector<cc::string>();

#if defined(CC_OS_WINDOWS)
    // The CRT's own copy of what main() was given, already split.
    // GetCommandLineW plus CommandLineToArgvW would preserve characters outside the active code page, at
    // the cost of a shell32 dependency — and nx::run captures the same narrow argv anyway, so the wide
    // route would only make the FALLBACK better than the primary path.
    if (__argv == nullptr)
        return out;

    for (auto i = 0; i < __argc; ++i)
        if (__argv[i] != nullptr)
            out.push_back(cc::string(__argv[i]));

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
    auto* const argc = _NSGetArgc();
    auto* const argv = _NSGetArgv();
    if (argc == nullptr || argv == nullptr || *argv == nullptr)
        return out;

    for (auto i = 0; i < *argc; ++i)
        if ((*argv)[i] != nullptr)
            out.push_back(cc::string((*argv)[i]));

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)
    // /proc/self/cmdline is the arguments separated by NULs, with a trailing one.
    auto adapter = cc::file_read_stream_adapter::open("/proc/self/cmdline");
    if (adapter.has_error())
        return out;

    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    if (bytes.has_error())
        return out;

    auto const& data = bytes.value();
    auto start = isize(0);
    for (auto i = isize(0); i < data.size(); ++i)
    {
        if (data[i] != byte(0))
            continue;

        out.push_back(cc::string(reinterpret_cast<char const*>(data.data()) + start, i - start));
        start = i + 1;
    }

    // A final argument with no trailing NUL, which /proc does not produce but a truncated read could.
    if (start < data.size())
        out.push_back(cc::string(reinterpret_cast<char const*>(data.data()) + start, data.size() - start));
#endif

    return out;
}
} // namespace

cc::vector<cc::string> const& nx::impl::os_process_args()
{
    // Materialized on the first call rather than at static-init time, and never re-queried: the command
    // line cannot change, and a function-local static is the one initialization order that is defined.
    static auto const args = query_os_args();
    return args;
}
