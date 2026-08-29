#pragma once

// Annotations for allocations that are never freed on purpose, so LeakSanitizer stops reporting a decision we made.
//
// The case this exists for is the process-lifetime singleton: an object deliberately outliving main, because destroying it
// at exit would leave something naming freed memory.
// That is a leak by LeakSanitizer's definition and not by ours, and the difference has to be written down somewhere the
// tool can read — otherwise every sanitizer run carries a failure whose real content is "yes, still intentional".
//
// Annotate at the allocation, never with a name-matching suppression file: a suppression silently covers whatever else
// grows into matching it, while this covers exactly the object handed to it.
// CC_HAS_LEAK_ANNOTATIONS reflects whether an active LeakSanitizer is there to tell; both facilities compile and do
// nothing when it is not, so a call site needs no #ifdef.
//
// Reach for leak_scope more often than its wording suggests, because LeakSanitizer cannot follow a pointer held in a
// cc:: container: our default memory resource allocates through mimalloc, which LeakSanitizer does not intercept, so it
// never scans that memory for pointers.
// An object owned only by a cc::vector or cc::map is therefore reported as a DIRECT leak rather than a reachable one,
// and leak_intentionally on the object that owns the container does not cover it — reachability is exactly what broke.
// Annotating a singleton whose members own anything means wrapping its construction in a leak_scope.
// SC_MIMALLOC=OFF removes that blind spot, and the sanitize-* presets set it — but a leak_scope is what stays correct
// in both builds, so prefer it over narrowing an annotation to a configuration that may not be the one running.

#include <clean-core/fwd.hh>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) && __has_include(<sanitizer/lsan_interface.h>)
#define CC_HAS_LEAK_ANNOTATIONS 1
#endif
#elif defined(__SANITIZE_ADDRESS__) && defined(__has_include)
#if __has_include(<sanitizer/lsan_interface.h>)
#define CC_HAS_LEAK_ANNOTATIONS 1
#endif
#endif
#ifndef CC_HAS_LEAK_ANNOTATIONS
#define CC_HAS_LEAK_ANNOTATIONS 0
#endif

#if CC_HAS_LEAK_ANNOTATIONS
#include <sanitizer/lsan_interface.h>
#endif

namespace cc
{
/// Declare that the heap object `p` points to is never freed on purpose, so LeakSanitizer must not report it.
/// Everything LeakSanitizer can reach from `p` is covered too, since the object joins the root set.
/// Read the note above on cc:: containers before relying on that, because mimalloc memory is not reachable in that sense.
/// `p` must be a live pointer that an allocator returned; a null or interior pointer annotates nothing.
/// State WHY the object is never freed at the call site, since this only records that the leak is deliberate and not what makes it correct.
inline void leak_intentionally(void const* p)
{
#if CC_HAS_LEAK_ANNOTATIONS
    __lsan_ignore_object(p);
#else
    (void)p;
#endif
}
} // namespace cc

/// Scope guard suppressing leak reports for every allocation made while it lives, on the calling thread.
/// This is what covers an intentional leak whose owned objects LeakSanitizer cannot reach — a singleton holding a cc:: container — and a leak inside code we do not own, reached through a call we do.
/// It hides any genuine leak that falls inside the same window, so keep the scope to the construction or the one foreign call, never a whole function that also does work.
struct cc::leak_scope
{
    leak_scope()
    {
#if CC_HAS_LEAK_ANNOTATIONS
        __lsan_disable();
#endif
    }

    ~leak_scope()
    {
#if CC_HAS_LEAK_ANNOTATIONS
        __lsan_enable();
#endif
    }

    leak_scope(leak_scope const&) = delete;
    leak_scope& operator=(leak_scope const&) = delete;
};
