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

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32) && (defined(__linux__) || defined(__APPLE__))

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
    cc::uintptr low = 0;
    cc::uintptr high = 0;
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
        b.high = reinterpret_cast<cc::uintptr>(top);
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
            b.low = reinterpret_cast<cc::uintptr>(addr);
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
    auto const a = reinterpret_cast<cc::uintptr>(f);

    if (a <= reinterpret_cast<cc::uintptr>(previous))
        return false; // the stack grows down, so an enclosing frame is always higher
    if ((a & (alignof(void*) - 1)) != 0)
        return false;
    if (b.low != 0 && (a < b.low || a + sizeof(frame) > b.high))
        return false;

    return true;
}
#endif
} // namespace

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
    // Table-driven, because x64 Windows has no frame-pointer ABI to chase.
    //
    // No early-out for stop_frame: RtlCaptureStackBackTrace offers no per-frame hook, so honouring it would mean
    // hand-rolling the walk over RtlVirtualUnwind.
    // That is the follow-up; today a Windows caller passing one gets a full capture rather than a wrong one.
    (void)stop_frame;

    // One extra so a stack deeper than `out` is distinguishable from one that exactly fills it.
    auto const wanted = ULONG(cc::min(out.size() + 1, isize(0xFFFF)));
    void* scratch[257];
    auto const room = ULONG(cc::min(isize(wanted), isize(CC_ARRAY_COUNT_OF(scratch))));

    auto const captured = ULONG(::RtlCaptureStackBackTrace(ULONG(skip + 1), room, scratch, nullptr));
    auto const usable = isize(captured);

    result.count = cc::min(usable, out.size());
    result.truncated = usable > out.size();
    for (isize i = 0; i < result.count; ++i)
        out[i] = scratch[i];

    return result;

#else
    auto const& bounds = current_stack_bounds();
    auto const* f = static_cast<frame const*>(__builtin_frame_address(0));
    auto const stop = reinterpret_cast<cc::uintptr>(stop_frame);

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

        if (stop != 0 && reinterpret_cast<cc::uintptr>(f) >= stop)
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
