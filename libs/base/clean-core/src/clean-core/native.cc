#include "native.hh"

#include <clean-core/macros.hh>
#include <clean-core/string.hh>

#include <mutex>


// Platform-specific includes for symbol demangling
#ifdef CC_COMPILER_MSVC
#include <Windows.h>

// NOTE: must be _after_ windows.h
#include <DbgHelp.h>

// TODO: Future - decide if dbghelp.lib linking should be exposed in CMake target_link_libraries
//       instead of using #pragma comment(lib, ...) for better cross-project control
#pragma comment(lib, "dbghelp.lib")
#endif

#ifdef CC_COMPILER_POSIX
#include <cxxabi.h>

#include <cstdlib>

#endif

cc::string cc::demangle_symbol(cc::string_view symbol)
{
    // Static mutex to protect thread-unsafe demangling functions
    // UnDecorateSymbolName (Windows) is documented as single-threaded
    // __cxa_demangle (POSIX) thread-safety is not guaranteed
    static std::mutex demangle_mutex;
    std::lock_guard<std::mutex> lock(demangle_mutex);

#ifdef CC_COMPILER_MSVC
    // MSVC implementation using UnDecorateSymbolName
    // Allocate buffer for demangled name (MSVC symbols are typically < 4KB)
    constexpr DWORD buffer_size = 4096;
    char buffer[buffer_size];

    // UnDecorateSymbolName expects a null-terminated string
    // Create a temporary null-terminated copy of the symbol
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
        // Successfully demangled
        return cc::string(buffer, result);
    }
    else
    {
        // Failed to demangle, return original symbol
        return cc::string::create_copy_of(symbol);
    }

#elif defined(CC_COMPILER_POSIX)
    // GCC/Clang implementation using __cxa_demangle
    // __cxa_demangle expects a null-terminated string
    cc::string symbol_nt = cc::string::create_copy_c_str_materialized(symbol);
    char const* nt_ptr = symbol_nt.c_str_if_terminated();
    CC_ASSERT(nt_ptr != nullptr, "should always succeed");

    int status = 0;
    char* demangled = abi::__cxa_demangle(nt_ptr, nullptr, nullptr, &status);

    if (status == 0 && demangled != nullptr)
    {
        // Successfully demangled
        cc::string result = cc::string(demangled);
        std::free(demangled);
        return result;
    }
    else
    {
        // Failed to demangle, return original symbol
        if (demangled != nullptr)
        {
            std::free(demangled);
        }
        return cc::string::create_copy_of(symbol);
    }

#else
    // Unsupported platform - return original symbol unchanged
    return cc::string::create_copy_of(symbol);
#endif
}
