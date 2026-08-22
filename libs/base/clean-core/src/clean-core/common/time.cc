#include <clean-core/common/time.hh>

#include <chrono>
#include <ctime>

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

cc::calendar_time cc::local_calendar_time(double wall_secs)
{
    // Floor rather than truncate: a cast rounds toward zero, so a pre-epoch reading would round UP and leave the
    // fraction below negative.
    // Done in integers to keep <cmath> out of a file this cheap.
    auto whole = i64(wall_secs);
    if (f64(whole) > wall_secs)
        --whole;

    auto const fraction = wall_secs - f64(whole);
    auto const stamp = std::time_t(whole);
    std::tm parts = {};

    // localtime_s / localtime_r rather than localtime: the plain one returns a pointer into a shared static buffer,
    // and two threads formatting a log line at once is the normal case here rather than an unlucky one.
#ifdef CC_OS_WINDOWS
    auto const ok = ::localtime_s(&parts, &stamp) == 0;
#else
    auto const ok = ::localtime_r(&stamp, &parts) != nullptr;
#endif

    if (!ok)
        return {};

    return {
        .year = i32(parts.tm_year) + 1900,
        .month = u8(parts.tm_mon + 1),
        .day = u8(parts.tm_mday),
        .hour = u8(parts.tm_hour),
        .minute = u8(parts.tm_min),
        .second = u8(parts.tm_sec),
        .millisecond = u16(fraction * 1000.0),
    };
}

u64 cc::impl::monotonic_ticks()
{
    // Nanoseconds, so the rate a caller calibrates comes out at 1e9 and nothing downstream has to special-case the
    // unit — a tick is a tick, and only its rate differs by platform.
    auto const now = std::chrono::steady_clock::now().time_since_epoch();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}
