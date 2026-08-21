#include <clean-core/common/time.hh>

#include <chrono>

using namespace cc::primitive_defines;

// The ONLY file in shaped-core allowed to include <chrono>.
// It is 1.16 s and 148 files entered on MSVC, so every header that reached for it taxed everything downstream.
// The .shaped-lint.yml entry beside this file is what keeps that true; see docs/notes/build-times.md.

double cc::current_time_steady_secs()
{
    auto const now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

double cc::current_time_wall_secs()
{
    // system_clock's epoch is the Unix epoch as of C++20, so the doc comment's promise is the standard's.
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

u64 cc::impl::monotonic_ticks()
{
    // Nanoseconds, so the rate a caller calibrates comes out at 1e9 and nothing downstream has to special-case the
    // unit — a tick is a tick, and only its rate differs by platform.
    auto const now = std::chrono::steady_clock::now().time_since_epoch();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}
