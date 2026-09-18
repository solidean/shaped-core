#include <clean-core/common/macros.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/impl/wasm_frames.hh>
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

/// How many frames two otherwise-comparable captures are allowed to disagree on.
///
/// Two of them are ours — each capture's own call site, and the call site of whatever took both.
/// The rest is the platform's: a toolchain may interpose frames between ours that no source-level call explains,
/// and wasm built with -fexceptions does, routing anything that might throw through a JS `invoke_*` trampoline.
constexpr isize max_interposed_frames = 4;

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
///
/// Thread-local, and that part is not incidental: every test in this file writes it, nexus runs them in parallel, and
/// a shared `volatile` orders nothing between threads -- ThreadSanitizer reports it as the race it is.
/// One sink per thread removes the sharing without touching what the sink is for.
thread_local int volatile g_no_tail_call = 0;

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

/// Captures at several depths from ONE call site, which is what makes the counts comparable.
///
/// The same rule take_both follows, and it bites harder here: a call site is free to need cleanup the one beside
/// it does not, and under -fexceptions that cleanup is a JS trampoline frame the engine reports.
/// So three captures written as three statements measure three call sites, not three depths.
CC_DONT_INLINE void take_at_depths(cc::span<int const> depths, cc::span<capture> out)
{
    for (auto i = isize(0); i < depths.size(); ++i)
        out[i] = take_at_depth(depths[i]);
    g_no_tail_call = int(out[0].result.count);
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

    // An exact count assumes the build reports a frame per call the source makes, which a Release wasm build does
    // not — see CC_WASM_KEEPS_FRAME_STRUCTURE.
    if (CC_WASM_KEEPS_FRAME_STRUCTURE)
        CHECK(deep.result.count == shallow.result.count + 1);
    else
        CHECK(deep.result.count >= shallow.result.count);

    // One frame deeper, and everything above the innermost few is the same ancestry.
    //
    // What may differ is each capture's own call site and take_both's, which calls the two helpers from two
    // different places — neither of which the walk has any say in.
    // The budget is four rather than two because a platform may interpose frames of its own between ours: wasm
    // built with -fexceptions routes a call that might throw through a JS `invoke_*` trampoline, which is a real
    // frame the engine reports and nothing in this file put there.
    CHECK(common_suffix(deep.frames, shallow.frames) >= shallow.result.count - max_interposed_frames);
}

TEST("stack capture - a recursive call site repeats once per level")
{
    if (!cc::stack_capture_available())
        SKIP("no stack walking on this platform");

    int const depths[] = {2, 6, 10};
    capture taken[3];
    take_at_depths(depths, taken);

    auto const& shallow = taken[0];
    auto const& middle = taken[1];
    auto const& deep = taken[2];

    REQUIRE(shallow.result.count > 0);

    // Four more levels of the same call site, so four more frames — unless the capture ran out of room.
    if (!deep.result.truncated && !deep.result.broken && !middle.result.broken && !shallow.result.broken)
    {
        // Four per four levels only where a call is a frame; elsewhere the levels must still cost something, and
        // cost it evenly.
        auto const first = middle.result.count - shallow.result.count;
        auto const second = deep.result.count - middle.result.count;

        CHECK(first > 0);
        CHECK(second == first);
        if (CC_WASM_KEEPS_FRAME_STRUCTURE)
            CHECK(first == 4);
    }

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
    if (!cc::stack_capture_supports_stop_frame())
        SKIP("no stack addresses to bound a walk by on this platform");

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
    if (!CC_HAS_THREADS)
        SKIP("no threads in this build, so there is no second thread to walk");

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

    // Not under ThreadSanitizer, which starts the thread through a trampoline of its own that the walk cannot get
    // past: the capture is correct as far as it goes and then reports that it stopped early.
    // That the walk RAN on an unseen thread is the claim here, and `count` is what carries it.
    if constexpr (CC_HAS_THREAD_SANITIZER == 0)
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

// wasm has no walkable native stack, so its capture goes through the JS engine's frame text and the parser in
// platform/impl/wasm_frames.hh.
//
// What recorded sample strings cannot check is the SKEW: how many of the capture's own frames sit above the caller.
// Getting it wrong drops the caller's own frame and reports its parent as the innermost, which reads as correct and is
// not — so it is pinned here rather than trusted.
#if defined(__EMSCRIPTEN__)

namespace
{
/// Deliberately not inlined, and deliberately not tail-calling, so each of these is a frame the engine reports.
/// `g_no_tail_call` is what forces the second to have something left to do once the first returns — without it
/// `return f(...)` is a jump and the frame is never pushed.
CC_DONT_INLINE cc::stack_capture_result wasm_capture_at_depth_1(cc::span<void*> out)
{
    auto const r = cc::capture_stack(out);
    g_no_tail_call = int(r.count);
    return r;
}

CC_DONT_INLINE cc::stack_capture_result wasm_capture_at_depth_2(cc::span<void*> out)
{
    auto const r = wasm_capture_at_depth_1(out);
    g_no_tail_call = int(r.count);
    return r;
}
} // namespace

TEST("capture_stack - wasm captures wasm frames")
{
    void* frames[64];
    auto const result = wasm_capture_at_depth_1(frames);

    REQUIRE(result.count > 0);

    // **A wasm stack genuinely alternates**, so this counts rather than asserting about any one frame.
    //
    // Built with -fexceptions, a call that might throw goes wasm -> JS `invoke_*` trampoline -> wasm, and the engine
    // reports that trampoline as a frame like any other.
    // So JS frames appear THROUGHOUT a stack rather than only under it, and an assertion that the innermost is a wasm
    // frame is simply false.
    // What must hold is that most of the stack is ours.
    auto wasm_frames = 0;
    for (auto i = 0; i < result.count; ++i)
        if ((reinterpret_cast<u32>(frames[i]) & cc::impl::wasm_js_frame_bit) == 0)
            ++wasm_frames;

    CHECK(wasm_frames > result.count / 2);
}

TEST("capture_stack - wasm's skew is right, so the caller is the innermost frame")
{
    if (!CC_WASM_KEEPS_FRAME_STRUCTURE)
        SKIP("this build collapses calls the source keeps apart, so there is no skew to pin");

    void* shallow[64];
    void* deep[64];
    auto const shallow_result = wasm_capture_at_depth_1(shallow);
    auto const deep_result = wasm_capture_at_depth_2(deep);

    REQUIRE(shallow_result.count > 0);
    REQUIRE(deep_result.count > 0);

    // One extra frame between the capture and this test, and exactly one extra frame reported.
    CHECK(deep_result.count == shallow_result.count + 1);

    // **This is what pins the skew**, and it is an equality rather than a count.
    //
    // Both stacks reach `cc::capture_stack` through the one call site inside `wasm_capture_at_depth_1`, so if the
    // innermost frame reported is the caller of `capture_stack` — which is what the contract says — then both
    // captures must report that same call site, and the addresses are equal.
    //
    // A skew one too large would instead report each capture's GRANDparent: this test's call site for the shallow
    // one and `wasm_capture_at_depth_2`'s for the deep one, which are different places and compare unequal.
    // A skew one too small reports a frame inside `cc::capture_stack` itself, which likewise differs from neither
    // consistently nor usefully.
    CHECK(shallow[0] == deep[0]);

    // And the two stacks are otherwise the same stack, one frame apart: the shallow capture's second frame is
    // this test, and the deep one's third is too -- different call sites in this body, hence compared from the
    // frame above them, where both are this test's own caller.
    REQUIRE(deep_result.count >= 4);
    for (auto i = 2; i < shallow_result.count; ++i)
        CHECK(shallow[i] == deep[i + 1]);
}

TEST("capture_stack - wasm is available and prices itself honestly")
{
    CHECK(cc::stack_capture_available());
    CHECK(cc::stack_walk_available(cc::stack_walk::automatic));

    // The whole reason the query exists: a caller that samples at a rate must be able to tell that this platform
    // is three orders of magnitude off the cheap one.
    CHECK(cc::stack_capture_cost_ns() > 1'000);

    // No stack addresses here, so nothing to compare a stop frame against, and no foreign thread can be walked.
    CHECK(!cc::stack_capture_supports_stop_frame());
    CHECK(!cc::stack_capture_from_context_available());
}

#endif // __EMSCRIPTEN__
