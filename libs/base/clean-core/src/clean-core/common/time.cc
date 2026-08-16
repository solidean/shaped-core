#include <clean-core/common/time.hh>

#include <chrono>

// The ONLY file in shaped-core allowed to include <chrono>.
// It is 1.16 s and 148 files entered on MSVC, so every header that reached for it taxed everything downstream.
// The .shaped-lint.yml entry beside this file is what keeps that true; see docs/notes/build-times.md.

double cc::current_time_steady_secs()
{
    auto const now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}
