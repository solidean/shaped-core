#include "native.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/string/string.hh>

#include <mutex>


#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>

// Must come after windows.h.
#include <DbgHelp.h>

// TODO: Future - decide if dbghelp.lib linking should be exposed in CMake target_link_libraries
//       instead of using #pragma comment(lib, ...) for better cross-project control
#pragma comment(lib, "dbghelp.lib")
#endif

#ifndef CC_OS_WINDOWS
#include <cxxabi.h>

#include <cstdlib>

#endif

cc::string cc::demangle_symbol(cc::string_view symbol)
{
    // UnDecorateSymbolName is documented as single-threaded, and __cxa_demangle makes no thread-safety guarantee either.
    static std::mutex demangle_mutex;
    std::lock_guard<std::mutex> lock(demangle_mutex);

#ifdef CC_OS_WINDOWS
    // MSVC symbols are typically well under 4 KB.
    constexpr DWORD buffer_size = 4096;
    char buffer[buffer_size];

    // UnDecorateSymbolName needs a null-terminated string.
    cc::string symbol_nt = cc::string::create_copy_c_str_materialized(symbol);
    char const* nt_ptr = symbol_nt.c_str_if_terminated();
    CC_ASSERT(nt_ptr != nullptr, "should always succeed");

    DWORD result = UnDecorateSymbolName(nt_ptr,          // Decorated name
                                        buffer,          // Output buffer
                                        buffer_size,     // Size of output buffer
                                        UNDNAME_COMPLETE // Undecorate options
    );

    if (result > 0)
    {
        return cc::string(buffer, result);
    }
    else
    {
        return cc::string::create_copy_of(symbol);
    }

#else
    // __cxa_demangle needs a null-terminated string.
    cc::string symbol_nt = cc::string::create_copy_c_str_materialized(symbol);
    char const* nt_ptr = symbol_nt.c_str_if_terminated();
    CC_ASSERT(nt_ptr != nullptr, "should always succeed");

    int status = 0;
    char* demangled = abi::__cxa_demangle(nt_ptr, nullptr, nullptr, &status);

    if (status == 0 && demangled != nullptr)
    {
        cc::string result = cc::string(demangled);
        std::free(demangled);
        return result;
    }
    else
    {
        if (demangled != nullptr)
        {
            std::free(demangled);
        }
        return cc::string::create_copy_of(symbol);
    }
#endif
}
