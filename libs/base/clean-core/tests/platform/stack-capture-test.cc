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

/// How many frames two captures agree on, counting back from the outermost.
///
/// The right invariant to assert, because the frames they DISAGREE on are the interesting ones and there is no
/// build-independent count of them: a call site differs, a helper gets inlined, a body is reached through one more
/// wrapper in one preset than another.
/// What must hold in every build is that two captures of the same program share almost all of their ancestry.
[[nodiscard]] isize common_suffix(cc::span<void* const> a, cc::span<void* const> b)
{
    auto shared = isize(0);
    while (shared < a.size() && shared < b.size() && a[a.size() - 1 - shared] == b[b.size() - 1 - shared])
        ++shared;
    return shared;
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

/// How many leading frames two captures agree on.
///
/// A prefix rather than a suffix here, because the two walkers can legitimately reach different DEPTHS — the chain
/// ends where a frame pointer stops being kept — so their outermost frames are not comparable.
[[nodiscard]] isize common_prefix(cc::span<void* const> a, cc::span<void* const> b)
{
    auto shared = isize(0);
    while (shared < a.size() && shared < b.size() && a[shared] == b[shared])
        ++shared;
    return shared;
}

[[nodiscard]] isize common_suffix_of_prefixes(cc::span<void* const> a, cc::span<void* const> b)
{
    return common_prefix(a, b);
}

/// Takes one capture with each walker, from one frame so they are comparable.
CC_DONT_INLINE void take_both_walks(capture& chased, capture& unwound)
{
    chased.frames.resize_to_constructed(max_frames, nullptr);
    chased.result = cc::capture_stack(cc::span<void*>(chased.frames), 0, nullptr, cc::stack_walk::frame_pointers);
    chased.frames.resize_down_to(chased.result.count);

    unwound.frames.resize_to_constructed(max_frames, nullptr);
    unwound.result = cc::capture_stack(cc::span<void*>(unwound.frames), 0, nullptr, cc::stack_walk::unwind_tables);
    unwound.frames.resize_down_to(unwound.result.count);

    g_no_tail_call = int(chased.result.count + unwound.result.count);
}

/// The same anchoring, for the pair that differs only in `skip`.
CC_DONT_INLINE void take_both_skipping(capture& all, capture& skipped)
{
    all = take(0);
    skipped = take(2);
    g_no_tail_call = int(all.result.count + skipped.result.count);
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

    REQUIRE(shallow.result.count > 3);
    CHECK(deep.result.count == shallow.result.count + 1);

    // One frame deeper, and everything above the innermost couple is the same ancestry.
    // The two that may differ are each capture's own call site and take_both's, which calls the two helpers from two
    // different places — neither of which the walk has any say in.
    CHECK(common_suffix(deep.frames, shallow.frames) >= shallow.result.count - 2);
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

    capture all;
    capture skipped;
    take_both_skipping(all, skipped);

    REQUIRE(all.result.count > 3);

    CHECK(skipped.result.count == all.result.count - 2);
    CHECK(common_suffix(all.frames, skipped.frames) >= skipped.result.count - 1);
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

    auto const unbounded = take();
    REQUIRE(unbounded.result.count > 0);

    CC_RECORD_SCOPE("stack-capture-test");
    auto const* const frame = cc::rec::current_scope_frame();
    REQUIRE(frame != nullptr);

    auto const bounded = take(0, frame);

    // The walk stopped where the scope stack takes over, and said so rather than merely running short.
    CHECK(bounded.result.stopped);
    CHECK(bounded.result.count < unbounded.result.count);

    // ... and it only stops when there is a scope to stop at.
    CHECK(!unbounded.result.stopped);
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

TEST("stack capture - the available walk matches the platform")
{
    // Exactly one mechanism per platform, and the enum reports which rather than leaving a caller to guess what a
    // capture costs — an order of magnitude separates them.
    auto const chase = cc::stack_walk_available(cc::stack_walk::frame_pointers);
    auto const tables = cc::stack_walk_available(cc::stack_walk::unwind_tables);

    CHECK(cc::stack_walk_available(cc::stack_walk::automatic) == cc::stack_capture_available());
    CHECK(!(chase && tables)); // no platform has both

#if defined(_WIN32)
    // Win64's frame pointer is rsp+offset, not the head of a chain, so there is nothing to chase whatever the compiler.
    CHECK(tables);
    CHECK(!chase);
#endif
}
