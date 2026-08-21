#include "stack_capture.hh"

#include <clean-core/common/macros.hh>

#if defined(__EMSCRIPTEN__)
// No walkable native stack.
#elif defined(_WIN32)
#include <clean-core/platform/win32_sanitized.hh>
#else
#include <pthread.h>
#endif

using namespace cc::primitive_defines;

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
CC_DONT_INLINE cc::stack_capture_result capture_from_context(void* start,
                                                             cc::span<void*> out,
                                                             isize skip,
                                                             void const* stop_frame)
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

        if (!take(pc))
            break;
    }

    return result;
}

#elif !defined(__EMSCRIPTEN__) && (defined(__linux__) || defined(__APPLE__))

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

#if defined(__APPLE__)
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
                                                               void const* stop_frame)
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (native_context == nullptr || out.empty() || skip < 0)
        return {};
    return capture_from_context(native_context, out, skip, stop_frame);
#else
    (void)native_context;
    (void)out;
    (void)skip;
    (void)stop_frame;
    return {};
#endif
}

bool cc::stack_capture_available()
{
    return has_stack_capture;
}

cc::stack_capture_result cc::capture_stack(cc::span<void*> out, isize skip, void const* stop_frame)
{
    cc::stack_capture_result result;
    if (out.empty() || skip < 0)
        return result;

#if defined(__EMSCRIPTEN__)
    (void)stop_frame;
    return result;

#elif defined(_WIN32)
    // One extra, for this function's own frame: capture_from_context is deliberately not inlined, so the walk starts
    // one level below the caller either way and frame 0 must still be the CALLER's call site.
    return capture_from_context(nullptr, out, skip + 1, stop_frame);

#else
    auto const& bounds = current_stack_bounds();
    auto const* f = static_cast<frame const*>(__builtin_frame_address(0));
    auto const stop = reinterpret_cast<u64>(stop_frame);

    // No implicit skip: this frame's RETURN address is the caller's call site, which is exactly frame 0.
    // Adding one here would quietly drop the caller and disagree with the Windows path by a frame.
    auto remaining_skip = skip;

    frame const* previous = nullptr;
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

        if (remaining_skip > 0)
            --remaining_skip;
        else if (result.count < out.size())
            out[result.count++] = ra;
        else
        {
            result.truncated = true;
            break;
        }

        previous = f;
        f = f->enclosing;
    }

    return result;
#endif
}
