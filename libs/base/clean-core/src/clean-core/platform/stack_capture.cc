#include "stack_capture.hh"

#include <clean-core/common/macros.hh>
#include <clean-core/platform/intrinsics.hh>

#if defined(__EMSCRIPTEN__)
// No walkable native stack.
#elif defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#else
#include <pthread.h>
#endif

using namespace cc::primitive_defines;

// Chasing needs a frame pointer that is the head of a CHAIN, and Windows keeps one that is not.
//
// The x64 ABIs disagree about what a frame pointer means.
// SysV and AAPCS store {enclosing frame, return address} at it, which is a linked list.
// Win64 sets it to `rsp + offset` with the offset in the unwind info, so the words there are whatever the frame
// happens to hold — measured, and they are not a chain.
// So `/Oy-` under clang-cl buys a frame pointer and no chain to walk, and Windows unwinds from tables whatever the
// compiler.
#if defined(__EMSCRIPTEN__) || defined(_WIN32)
#define CC_CAN_CHASE_FRAME_POINTERS 0
#elif defined(CC_FRAME_POINTERS) && (defined(__clang__) || defined(__GNUC__)) \
    && (defined(__linux__) || defined(__APPLE__))
#define CC_CAN_CHASE_FRAME_POINTERS 1
#else
#define CC_CAN_CHASE_FRAME_POINTERS 0
#endif

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define CC_CAN_UNWIND_TABLES 1
#else
#define CC_CAN_UNWIND_TABLES 0
#endif

namespace
{
#if defined(__EMSCRIPTEN__)
constexpr bool has_stack_capture = false;
#elif defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
constexpr bool has_stack_capture = true;
#else
constexpr bool has_stack_capture = false;
#endif

#if defined(_WIN32)

/// Where execution is, and where the stack is, in a CONTEXT — spelled differently per architecture.
[[nodiscard]] DWORD64& context_pc(CONTEXT& c)
{
#if defined(CC_ARCH_ARM64)
    return c.Pc;
#else
    return c.Rip;
#endif
}

[[nodiscard]] DWORD64& context_sp(CONTEXT& c)
{
#if defined(CC_ARCH_ARM64)
    return c.Sp;
#else
    return c.Rsp;
#endif
}

/// Unwinds `start` (or the caller's own context when null) one frame at a time.
///
/// Hand-rolled rather than RtlCaptureStackBackTrace for two reasons that both matter here.
/// It offers a per-frame hook, which is what `stop_frame` needs; and it walks an arbitrary CONTEXT, which is what
/// sampling a suspended thread needs — RtlCaptureStackBackTrace only ever walks its own caller.
/// The history table is reused across frames, which is where most of the per-frame lookup cost goes.
///
/// `skip_below` is a STACK ADDRESS, and every frame at or below it is dropped before `skip` is applied at all.
/// That is how a self-capture leaves the recorder's own frames out WITHOUT counting them: the count is not knowable
/// here, because whether cc::capture_stack still has a frame of its own is the optimizer's business — MSVC on arm64
/// tail-calls into this function and leaves none.
CC_DONT_INLINE cc::stack_capture_result capture_from_context(void* start,
                                                             cc::span<void*> out,
                                                             isize skip,
                                                             void const* stop_frame,
                                                             u64 skip_below = 0)
{
    cc::stack_capture_result result;

    // A foreign context's own PC is the most interesting frame it has — it is where that thread IS — while a
    // self-capture's is inside capture_stack and never wanted.
    auto const emit_start_pc = start != nullptr;

    CONTEXT ctx;
    if (start != nullptr)
        ctx = *static_cast<CONTEXT const*>(start);
    else
        ::RtlCaptureContext(&ctx);

    UNWIND_HISTORY_TABLE history = {};
    auto const stop = reinterpret_cast<u64>(stop_frame);
    auto remaining_skip = skip;

    auto const take = [&](u64 pc)
    {
        if (remaining_skip > 0)
        {
            --remaining_skip;
            return true;
        }

        if (result.count >= out.size())
        {
            result.truncated = true;
            return false;
        }

        out[result.count++] = reinterpret_cast<void*>(pc);
        return true;
    };

    if (emit_start_pc && context_pc(ctx) != 0 && !take(context_pc(ctx)))
        return result;

    // A capture is bounded work even when the chain is a lie, which is what keeps a crash handler from hanging.
    constexpr isize max_frames = 512;

    for (isize walked = 0; walked < max_frames; ++walked)
    {
        DWORD64 image_base = 0;
        auto* const function = ::RtlLookupFunctionEntry(context_pc(ctx), &image_base, &history);
        if (function == nullptr)
        {
            // A leaf with no unwind data: its return address is at the stack pointer, and there is nothing to unwind.
            result.broken = true;
            break;
        }

        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context_pc(ctx), function, &ctx, &handler_data,
                           &establisher_frame, nullptr);

        auto const pc = context_pc(ctx);
        if (pc == 0)
            break; // the outermost frame of the thread

        if (stop != 0 && u64(context_sp(ctx)) >= stop)
        {
            result.stopped = true;
            break;
        }

        // Still inside our own frames, which the caller never asked about.
        if (skip_below != 0 && u64(context_sp(ctx)) <= skip_below)
            continue;

        if (!take(pc))
            break;
    }

    return result;
}

#endif // _WIN32

#if CC_CAN_CHASE_FRAME_POINTERS

/// One frame's worth of the chain, and the same layout on x86-64 and arm64.
///
/// x86-64 pushes the caller's rbp then the return address; arm64 stores x29 then x30 at the frame base.
/// Both leave `{ enclosing frame, return address }` at the frame pointer, which is why one walk covers both.
struct frame
{
    frame const* enclosing;
    void* return_address;
};

/// This thread's stack, low address first.
///
/// Cached because querying it costs a pthread call, and validated against on every frame — a chain that leaves the
/// stack is a corrupted or mid-prologue frame, and following it reads arbitrary memory.
struct stack_bounds
{
    u64 low = 0;
    u64 high = 0;
    bool known = false;
};

thread_local stack_bounds tl_bounds;

stack_bounds const& current_stack_bounds()
{
    auto& b = tl_bounds;
    if (b.known)
        return b;

    b.known = true; // a failed query is cached too, so it is asked exactly once per thread

#if defined(_WIN32)
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    ::GetCurrentThreadStackLimits(&low, &high);
    b.low = u64(low);
    b.high = u64(high);
#elif defined(__APPLE__)
    auto* const top = pthread_get_stackaddr_np(pthread_self()); // the HIGH end on Apple, unlike Linux
    auto const size = pthread_get_stacksize_np(pthread_self());
    if (top != nullptr && size > 0)
    {
        b.high = reinterpret_cast<u64>(top);
        b.low = b.high - size;
    }
#else
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0)
    {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0 && addr != nullptr)
        {
            b.low = reinterpret_cast<u64>(addr);
            b.high = b.low + size;
        }
        pthread_attr_destroy(&attr);
    }
#endif

    return b;
}

/// Whether `f` can be believed as the next link.
///
/// Three tests, and each one is a real failure mode rather than paranoia: outside the stack means a corrupted or
/// mid-prologue frame, not increasing means a cycle that would never terminate, and misalignment means we are not
/// looking at a frame at all.
[[nodiscard]] bool is_plausible(frame const* f, frame const* previous, stack_bounds const& b)
{
    auto const a = reinterpret_cast<u64>(f);

    if (a <= reinterpret_cast<u64>(previous))
        return false; // the stack grows down, so an enclosing frame is always higher
    if ((a & (alignof(void*) - 1)) != 0)
        return false;
    if (b.low != 0 && (a < b.low || a + sizeof(frame) > b.high))
        return false;

    return true;
}
#endif
} // namespace

namespace
{
#if CC_CAN_CHASE_FRAME_POINTERS
/// Chases the frame chain from `start_frame`, optionally emitting `start_pc` first.
///
/// `start_pc` is what a sampled context contributes and a self-capture does not have: for a foreign thread the
/// program counter is where it IS, and the chain only ever yields return addresses.
cc::stack_capture_result chase_chain(frame const* start_frame,
                                     void* start_pc,
                                     cc::span<void*> out,
                                     isize skip,
                                     void const* stop_frame)
{
    cc::stack_capture_result result;

    auto const& bounds = current_stack_bounds();
    auto const stop = reinterpret_cast<u64>(stop_frame);
    auto remaining_skip = skip;

    auto const take = [&](void* pc)
    {
        if (remaining_skip > 0)
        {
            --remaining_skip;
            return true;
        }
        if (result.count >= out.size())
        {
            result.truncated = true;
            return false;
        }
        out[result.count++] = pc;
        return true;
    };

    if (start_pc != nullptr && !take(start_pc))
        return result;

    frame const* previous = nullptr;
    auto const* f = start_frame;

    while (f != nullptr)
    {
        if (!is_plausible(f, previous, bounds))
        {
            result.broken = true;
            break;
        }

        if (stop != 0 && reinterpret_cast<u64>(f) >= stop)
        {
            result.stopped = true;
            break;
        }

        auto* const ra = f->return_address;
        if (ra == nullptr)
            break; // the outermost frame of a thread, which parks a null there

        if (!take(ra))
            break;

        previous = f;
        f = f->enclosing;
    }

    return result;
}
#endif

/// Which walk `automatic` means here.
/// Chasing where the build keeps a frame pointer, because it is an order of magnitude cheaper; tables otherwise.
[[nodiscard]] cc::stack_walk resolve(cc::stack_walk walk)
{
    if (walk != cc::stack_walk::automatic)
        return walk;

#if CC_CAN_CHASE_FRAME_POINTERS
    return cc::stack_walk::frame_pointers;
#elif CC_CAN_UNWIND_TABLES
    return cc::stack_walk::unwind_tables;
#else
    return cc::stack_walk::automatic;
#endif
}
} // namespace

bool cc::stack_walk_available(cc::stack_walk walk)
{
    switch (walk)
    {
    case cc::stack_walk::automatic:
        return has_stack_capture;
    case cc::stack_walk::frame_pointers:
        return CC_CAN_CHASE_FRAME_POINTERS != 0;
    case cc::stack_walk::unwind_tables:
        return CC_CAN_UNWIND_TABLES != 0;
    }
    return false;
}

bool cc::stack_capture_from_context_available()
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}

cc::stack_capture_result cc::capture_stack_from_native_context(void* native_context,
                                                               cc::span<void*> out,
                                                               isize skip,
                                                               void const* stop_frame,
                                                               cc::stack_walk walk)
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (native_context == nullptr || out.empty() || skip < 0)
        return {};

    auto const* const ctx = static_cast<CONTEXT const*>(native_context);

    (void)ctx;
    (void)walk;
    return capture_from_context(native_context, out, skip, stop_frame);
#else
    (void)native_context;
    (void)out;
    (void)skip;
    (void)stop_frame;
    (void)walk;
    return {};
#endif
}

bool cc::stack_capture_available()
{
    return has_stack_capture;
}

cc::stack_capture_result cc::capture_stack(cc::span<void*> out, isize skip, void const* stop_frame, cc::stack_walk walk)
{
    if (out.empty() || skip < 0)
        return {};

    // Named here rather than left unnamed in the signature: only the chasing path reads it, and Windows compiles
    // neither that path nor the fallback below — so the parameter is genuinely unused there and nowhere else.
    (void)walk;

#if CC_CAN_CHASE_FRAME_POINTERS
    if (resolve(walk) == cc::stack_walk::frame_pointers)
    {
        // This frame's own return address is the caller's call site, which is exactly frame 0 — no implicit skip.
        return chase_chain(static_cast<frame const*>(__builtin_frame_address(0)), nullptr, out, skip, stop_frame);
    }
#endif

#if CC_CAN_UNWIND_TABLES
    // Our own frames are dropped by ADDRESS rather than by count.
    // A count assumes each of them still exists, and a frame whose function does nothing but forward a call need not:
    // MSVC on arm64 tail-calls such a function and leaves it none, which is measured — not for this pair, but for a
    // caller shaped exactly like it (see the symbolize test's helper).
    // A threshold needs no such assumption, since a frame that is not there is simply one fewer frame below the mark.
    return capture_from_context(nullptr, out, skip, stop_frame, reinterpret_cast<u64>(cc::impl::current_frame_address()));
#else
    (void)stop_frame;
    (void)walk;
    return {};
#endif
}
