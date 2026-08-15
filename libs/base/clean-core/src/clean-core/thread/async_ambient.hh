#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/impl/async_tls.hh>

// Ambient context for cc::async: "which logical task is this work part of?", answerable from anywhere inside a frame.
// The node-side half — how a context reaches a frame running on an arbitrary worker — is in async_node.hh / async.cc.
// This file is the composition layer above it, and knows nothing about nodes.
//
// One word rides the async graph.
// cc never learns what is in it, so several consumers share it through the immutable chain below: a test runner naming the running test, a profiler naming the enclosing zone, whatever comes next.
//
// The chain is the whole composition mechanism.
// Pushing a scope builds a link pointing at the current head and installs it, which is O(1).
// Propagating one is copying a single pointer, O(1) whatever the number of consumers.
// Only lookup is O(consumers), since it walks until the tag matches.
//
// That asymmetry is the point: lookup happens inside a CHECK or a profiler zone-enter, never per spawn and never per poll.
// If it ever shows up in a profile, add a present_mask to the link so a miss costs one AND.
//
// The chain is already a stack, so a profiler gets enclosing-scope nesting for free.

/// One link of the ambient chain: an immutable (tag, value) pair plus the enclosing link.
///
/// Refcounted, and the parent reference is STRONG — so retaining the head retains the whole chain in O(1), and a chain walk can never reach a freed link.
/// That is what makes it safe for a node to outlive the scope that named it (see cc::async_ambient_scope).
///
/// Never build one by hand: async_ambient_scope owns the allocation and the install.
struct cc::async_ambient_link
{
    void const* tag = nullptr;
    void* value = nullptr;
    async_ambient_link* parent = nullptr;
    cc::atomic<i32> refs = {1};
};

namespace cc::impl
{
/// Free `l` and every ancestor whose count its release drops to zero.
/// Iterative rather than recursive: chain depth is a consumer's business, not ours.
void async_ambient_free(async_ambient_link* l);

inline void async_ambient_retain(void* a)
{
    if (a != nullptr)
        static_cast<async_ambient_link*>(a)->refs.fetch_add(1, cc::memory_order_relaxed);
}

inline void async_ambient_release(void* a)
{
    if (a == nullptr)
        return;
    auto* const l = static_cast<async_ambient_link*>(a);
    if (l->refs.fetch_sub(1, cc::memory_order_acq_rel) == 1)
        async_ambient_free(l);
}

/// Install `a` as the calling thread's ambient for a scope, restoring the previous head on the way out.
/// A null `a` installs nothing, so a node with no context inherits whatever is driving it.
///
/// Unlike async_ambient_scope this takes NO reference: the caller owns one for the duration, and the whole point is to be free on the poll hot path.
/// Restoring from `_previous` rather than from the source is load-bearing where poll() uses it — the node the ambient came from can be destroyed before the scope ends.
struct async_ambient_poll_scope
{
    explicit async_ambient_poll_scope(void* a) : _previous(async_tls().ambient)
    {
        if (a != nullptr)
            async_tls().ambient = a;
    }
    ~async_ambient_poll_scope() { async_tls().ambient = _previous; }

    async_ambient_poll_scope(async_ambient_poll_scope const&) = delete;
    async_ambient_poll_scope& operator=(async_ambient_poll_scope const&) = delete;

private:
    void* _previous = nullptr;
};

/// Point `slot` at `a`, adjusting counts.
/// The compare is what keeps the repeat writers free: a node's context is written at every hand-off, and after the first those all store the same word, so only the first pays the atomics.
inline void async_ambient_store(void*& slot, void* a)
{
    if (slot == a)
        return;
    async_ambient_retain(a);
    async_ambient_release(slot);
    slot = a;
}
} // namespace cc::impl

namespace cc
{
/// The ambient context installed on the calling thread, or null.
/// A raw chain head — walk it with async_ambient_lookup_in.
[[nodiscard]] inline void* async_current_ambient()
{
    return impl::async_tls().ambient;
}

/// Walk `head`'s chain for `tag`, returning its value or null.
/// For code that already holds a head; a profiler storing one per sample wants this rather than the TLS form.
[[nodiscard]] inline void* async_ambient_lookup_in(void const* head, void const* tag)
{
    for (auto const* l = static_cast<async_ambient_link const*>(head); l != nullptr; l = l->parent)
        if (l->tag == tag)
            return l->value;
    return nullptr;
}

/// Look `tag` up in the ambient context installed on the calling thread.
/// This is the form for code reached indirectly — a CHECK deep inside a call stack — and the only one that touches TLS.
[[nodiscard]] inline void* async_ambient_lookup(void const* tag)
{
    return async_ambient_lookup_in(async_current_ambient(), tag);
}
} // namespace cc

/// RAII push/pop of an ambient scope, and the ONLY supported way to install a link.
///
/// That exclusivity is load-bearing rather than stylistic.
/// A node records only the chain HEAD, so an outer scope's count never sees a node created under an inner one.
/// That is sound only because scopes are strictly LIFO, so the inner scope pops — and checks — first.
///
/// Popping does NOT require the work started under it to have finished.
/// The refcount keeps the context alive for whoever still carries it, which is what makes async prewarming legal.
/// A consumer wanting the stricter rule reads outstanding() and decides for itself what a leak means — a test
/// runner in particular wants a reported failure naming the test, not the abort a cc-side assert would give it.
struct cc::async_ambient_scope
{
    /// `tag` identifies the consumer and must be address-unique — see CC_ASYNC_AMBIENT_TAG.
    /// `value` is opaque to cc and is what a lookup returns.
    explicit async_ambient_scope(void const* tag, void* value);
    ~async_ambient_scope();

    async_ambient_scope(async_ambient_scope const&) = delete;
    async_ambient_scope(async_ambient_scope&&) = delete;
    async_ambient_scope& operator=(async_ambient_scope const&) = delete;
    async_ambient_scope& operator=(async_ambient_scope&&) = delete;

    /// This scope's link, for a consumer that wants to hand the head somewhere explicitly.
    [[nodiscard]] async_ambient_link* link() const { return _link; }

    /// How many holders the link has beyond this scope — outstanding async work carrying this context.
    /// Racy on a live graph; for diagnostics and tests, not control flow.
    [[nodiscard]] i32 outstanding() const;

private:
    async_ambient_link* _link = nullptr;
};

/// Define a consumer's ambient tag as `inline void const* <name>()`.
///
/// The address of a function-local static, deliberately NOT of a constant.
/// MSVC's /OPT:ICF and the gold/lld ICF passes fold identical read-only data.
/// So two consumers each writing `inline constexpr char tag = 0;` can end up sharing one address, and then silently reading each other's context.
#define CC_ASYNC_AMBIENT_TAG(name)                      \
    inline void const* name()                           \
    {                                                   \
        static char const cc_impl_ambient_tag_byte = 0; \
        return &cc_impl_ambient_tag_byte;               \
    }
