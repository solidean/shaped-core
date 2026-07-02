#include <clean-core/common/macros.hh>
#include <clean-core/thread/thread.hh>

#if CC_HAS_THREADS

#if defined(CC_OS_WINDOWS)

#include <clean-core/string/conversion.hh>

// Declared here (not via <windows.h>) to keep this TU light, mirroring how assert.cc imports
// IsDebuggerPresent. char16_t and Windows wchar_t are both 16-bit, so the wide buffer maps directly.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread() noexcept;
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void*, wchar_t const*) noexcept;

void cc::set_current_thread_name(string_view name)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t must be 16-bit");

    auto wide = cc::utf8_to_utf16(name);
    wide.push_back(u'\0');
    SetThreadDescription(GetCurrentThread(), reinterpret_cast<wchar_t const*>(wide.data()));
}

#elif defined(CC_OS_LINUX) || defined(CC_OS_ANDROID)

#include <pthread.h>

void cc::set_current_thread_name(string_view name)
{
    // Linux caps the thread name at 16 bytes including the NUL terminator.
    char buf[16];
    isize const n = name.size() < 15 ? name.size() : 15;
    for (isize i = 0; i < n; ++i)
        buf[i] = name.data()[i];
    buf[n] = '\0';

    pthread_setname_np(pthread_self(), buf);
}

#elif defined(CC_OS_MACOS) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)

#include <pthread.h>

void cc::set_current_thread_name(string_view name)
{
    char buf[64];
    isize const n = name.size() < 63 ? name.size() : 63;
    for (isize i = 0; i < n; ++i)
        buf[i] = name.data()[i];
    buf[n] = '\0';

    pthread_setname_np(buf); // Darwin names only the current thread
}

#else // threads exist but naming is unsupported on this OS

void cc::set_current_thread_name(string_view)
{
}

#endif

#else // CC_HAS_THREADS == 0

void cc::set_current_thread_name(string_view)
{
}

#endif
