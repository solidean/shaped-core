#include <clean-core/common/profiling.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/stack_capture.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

#include <thread>

using namespace cc::primitive_defines;

// Testing a stack walk without symbols.
//
// Nothing here may name a function, because the whole point of cc::capture_stack is that it never asks who a return
// address belongs to.
// So the assertions are structural: a deeper capture extends a shallower one, a recursive call site repeats, skipping
// drops exactly what it says, and a stop frame yields a prefix.
// Those hold on any platform that walks at all, and are vacuous on one that does not — which is why every test below
// bails out early rather than pretending.

namespace
{
constexpr isize max_frames = 64;

/// A capture plus its frames, so a test can compare two of them.
struct capture
{
    cc::vector<void*> frames;
    cc::stack_capture_result result;
};

CC_DONT_INLINE capture take(isize skip = 0, void const* stop_frame = nullptr)
{
    capture c;
    c.frames.resize_to_constructed(max_frames, nullptr);
    c.result = cc::capture_stack(cc::span<void*>(c.frames), skip, stop_frame);
    c.frames.resize_down_to(c.result.count);
    return c;
}

/// Where a frame is forced to do something after its callee returns.
///
/// Without it these helpers TAIL CALL: `return f(...)` becomes a jump, the frame is never pushed, and a walk correctly
/// reports fewer frames than the source suggests.
/// Any test that counts frames has to defeat that first, or it measures the optimizer rather than the walker.
int volatile g_no_tail_call = 0;

/// Recurses exactly `depth` times, then captures.
/// The recursive call site is one address, so the capture must repeat it — which is what proves the walk advances one
/// real frame at a time rather than sliding along the stack.
CC_DONT_INLINE capture take_at_depth(int depth)
{
    if (depth > 0)
    {
        auto c = take_at_depth(depth - 1);
        g_no_tail_call = int(c.result.count);
        return c;
    }
    return take();
}

/// Whether `deep` ends with the whole of `shallow`.
[[nodiscard]] bool ends_with(cc::span<void* const> deep, cc::span<void* const> shallow)
{
    if (shallow.size() > deep.size())
        return false;

    auto const offset = deep.size() - shallow.size();
    for (isize i = 0; i < shallow.size(); ++i)
        if (deep[offset + i] != shallow[i])
            return false;

    return true;
}

CC_DONT_INLINE capture take_one_deeper()
{
    auto c = take();
    g_no_tail_call = int(c.result.count);
    return c;
}

/// Takes both captures from ONE frame, which is what makes them comparable.
///
/// Taking them from the test body instead compares two different call sites in the body's caller, and those return
/// addresses differ for a reason that has nothing to do with the walk.
CC_DONT_INLINE void take_both(capture& shallow, capture& deep)
{
    shallow = take();
    deep = take_one_deeper();
    g_no_tail_call = int(shallow.result.count + deep.result.count);
}
} // namespace

TEST("stack capture - availability is consistent with what a capture returns")
{
    auto const c = take();

    // The one thing that must hold everywhere: a platform that says it cannot walk returns nothing, and one that says
    // it can returns something.
    CHECK(cc::stack_capture_available() == (c.result.count > 0));
}

TEST("stack capture - every frame is a real address")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    auto const c = take();
    REQUIRE(c.result.count > 0);

    for (auto* const f : c.frames)
        CHECK(f != nullptr);
}

TEST("stack capture - a deeper capture extends a shallower one")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    capture shallow;
    capture deep;
    take_both(shallow, deep);

    REQUIRE(shallow.result.count > 2);
    REQUIRE(deep.result.count == shallow.result.count + 1);

    // Two frames differ for reasons that are not the walk's.
    // Frame 0 is each capture's own call site inside take/take_one_deeper, and the frame after it is take_both's own
    // call site — take_both calls the two helpers from two different places, so those addresses are simply not equal.
    // Everything above is the same ancestry seen from one level further in, which is the strongest thing you can say
    // about a walk without knowing a single name.
    for (isize i = 2; i < shallow.result.count; ++i)
        CHECK(shallow.frames[i] == deep.frames[i + 1]);
}

TEST("stack capture - a recursive call site repeats once per level")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    auto const shallow = take_at_depth(2);
    auto const deep = take_at_depth(6);

    REQUIRE(shallow.result.count > 0);

    // Four more levels of the same call site, so four more frames — unless the capture ran out of room.
    if (!deep.result.truncated && !deep.result.broken && !shallow.result.broken)
        CHECK(deep.result.count == shallow.result.count + 4);

    // ... and they are literally the same address, because it is one call site.
    cc::set<void*> distinct;
    for (auto* const f : deep.frames)
        distinct.insert(f);
    CHECK(distinct.size() < deep.frames.size());
}

TEST("stack capture - skip drops exactly the innermost frames")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    auto const all = take(0);
    auto const skipped = take(2);

    REQUIRE(all.result.count > 2);
    REQUIRE(skipped.result.count > 0);

    CHECK(skipped.result.count == all.result.count - 2);
    CHECK(ends_with(all.frames, skipped.frames));
}

TEST("stack capture - a full buffer reports truncation rather than lying")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    void* two[2] = {};
    auto const r = cc::capture_stack(cc::span<void*>(two, 2));

    CHECK(r.count == 2);
    CHECK(r.truncated); // a test runs nowhere near the outermost frame
}

TEST("stack capture - an empty output captures nothing and says so")
{
    auto const r = cc::capture_stack(cc::span<void*>());

    CHECK(r.count == 0);
    CHECK(!r.truncated);
    CHECK(!static_cast<bool>(r));
}

TEST("stack capture - a scope frame stops the walk short")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

#if defined(_WIN32)
    SKIP("RtlCaptureStackBackTrace has no per-frame hook, so stop_frame is not honoured yet");
#else
    auto const unbounded = take();
    REQUIRE(unbounded.result.count > 0);

    CC_RECORD_SCOPE("stack-capture-test");
    auto const* const frame = cc::rec::current_scope_frame();
    REQUIRE(frame != nullptr);

    auto const bounded = take(0, frame);

    // Strictly shorter, and a prefix: the walk stopped where the scope stack takes over.
    CHECK(bounded.result.stopped);
    CHECK(bounded.result.count < unbounded.result.count);
    CHECK(ends_with(unbounded.frames, bounded.frames) == false); // a PREFIX, not a suffix
    for (isize i = 0; i < bounded.result.count; ++i)
        CHECK(bounded.frames[i] == unbounded.frames[i]);
#endif
}

TEST("stack capture - works on a thread we did not start it on")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    // The stack bounds are cached per thread, so a fresh thread exercises the query rather than the cache.
    isize count = 0;
    bool broken = true;
    std::thread t(
        [&]
        {
            auto const c = take();
            count = c.result.count;
            broken = c.result.broken;
        });
    t.join();

    CHECK(count > 0);
    CHECK(!broken);
}
