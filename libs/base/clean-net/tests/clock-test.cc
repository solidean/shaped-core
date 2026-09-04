#include <clean-net/common/clock.hh>
#include <clean-net/common/deadline.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace cnet;

TEST("cnet - a manual clock moves only when a test moves it")
{
    auto clock = manual_clock(1000);
    CHECK(clock.now_ns() == 1000);
    CHECK(clock.now_ns() == 1000); // reading does not advance it

    clock.advance_ms(5);
    CHECK(clock.now_ns() == 1000 + 5 * 1000 * 1000);

    clock.set_ns(10'000'000'000);
    CHECK(clock.now_ns() == 10'000'000'000);
}

TEST("cnet - the system clock is monotonic")
{
    auto& clock = system_clock();
    auto const first = clock.now_ns();
    auto const second = clock.now_ns();
    CHECK(second >= first);

    // Two references are the same clock, so a deadline armed against one is readable through the other.
    CHECK(&clock == &system_clock());
}

TEST("cnet - a deadline says finite or never, and never by accident")
{
    CHECK(!deadline::never().is_finite());
    CHECK(deadline::after_ms(30000).is_finite());
    CHECK(deadline::after_ms(30000).timeout_ms == 30000);
    CHECK(deadline::after_secs(1.5).timeout_ms == 1500);

    // Zero is finite: it means "already expired", not "no deadline".
    CHECK(deadline::after_ms(0).is_finite());
    CHECK(deadline() == deadline::never());
}
