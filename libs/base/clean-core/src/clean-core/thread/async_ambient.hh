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
///
/// It also carries the poll-depth counter behind cc::async_is_polling.
/// Not because depth is an ambient concern, but because this is the one scope every poll already constructs, so tracking it costs no further TLS resolution and no new site to keep in sync.
struct async_ambient_poll_scope
{
    explicit async_ambient_poll_scope(void* a) : _previous(async_tls().ambient)
    {
        ++async_tls().poll_depth;
        if (a != nullptr)
            async_tls().ambient = a;
    }
    ~async_ambient_poll_scope()
    {
        --async_tls().poll_depth;
        async_tls().ambient = _previous;
    }

    async_ambient_poll_scope(async_ambient_poll_scope const&) = delete;
    async_ambient_poll_scope& operator=(async_ambient_poll_scope const&) = delete;

private:
    void* _previous = nullptr;
};

/// Detach the calling thread's ambient for a scope, so what runs under it starts a fresh attribution root.
///
/// This is what a scheduler WORK ITEM is polled under, and the counterpart to async_ambient_poll_scope's inheritance.
/// A node reaching a queue always carries its own token — the three write sites in
/// libs/base/clean-core/docs/systems/async.md see to that — and a null
/// token there means "no context", never "inherit the thread that dequeued me".
/// Inheritance is for the inline dependency drive alone, where the driver genuinely is the parent.
///
/// Without it a participant parked inside a logical task bills every unrelated item it steals to that task.
struct async_ambient_root_scope
{
    async_ambient_root_scope() : _previous(async_tls().ambient) { async_tls().ambient = nullptr; }
    ~async_ambient_root_scope() { async_tls().ambient = _previous; }

    async_ambient_root_scope(async_ambient_root_scope const&) = delete;
    async_ambient_root_scope& operator=(async_ambient_root_scope const&) = delete;

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

/// True while this thread is inside async_node_base::poll, at any depth.
///
/// What it answers is whether a THROW from here has somewhere to land: poll contains an escaping exception and fails the node on its error channel.
/// Outside a poll the same throw would escape a pool worker's thread function and terminate the process, so a consumer reporting from arbitrary threads
/// — nexus' REQUIRE is the motivating one — must record instead.
[[nodiscard]] inline bool async_is_polling()
{
    return impl::async_tls().poll_depth > 0;
}
} // namespace cc

/// A chain head captured from one thread so it can be re-installed on another, retained for as long as it is held.
///
/// cc::async does this for you: a node carries its context to whichever worker drives it.
/// This is the manual door, for a thread cc never sees — one a consumer started itself, or a callback arriving on a library's own thread.
/// Copyable and cheap: a pointer plus a refcount bump.
struct cc::async_ambient_handle
{
    /// Capture the calling thread's ambient chain head; null if there is none.
    async_ambient_handle() : _head(async_current_ambient()) { impl::async_ambient_retain(_head); }

    async_ambient_handle(async_ambient_handle const& rhs) : _head(rhs._head) { impl::async_ambient_retain(_head); }
    async_ambient_handle(async_ambient_handle&& rhs) noexcept : _head(rhs._head) { rhs._head = nullptr; }
    async_ambient_handle& operator=(async_ambient_handle const& rhs)
    {
        impl::async_ambient_retain(rhs._head); // before the release, so self-assignment is safe
        impl::async_ambient_release(_head);
        _head = rhs._head;
        return *this;
    }
    async_ambient_handle& operator=(async_ambient_handle&& rhs) noexcept
    {
        if (this != &rhs)
        {
            impl::async_ambient_release(_head);
            _head = rhs._head;
            rhs._head = nullptr;
        }
        return *this;
    }
    ~async_ambient_handle() { impl::async_ambient_release(_head); }

    /// The captured chain head, for async_ambient_lookup_in.
    [[nodiscard]] void* head() const { return _head; }

private:
    void* _head = nullptr;
};

/// Install a captured head as the calling thread's ambient for a scope, restoring the previous one on the way out.
///
/// Takes no reference of its own: `handle` holds one, and must outlive the scope.
/// A handle that captured nothing installs nothing, so the thread keeps whatever it already had.
struct cc::async_ambient_install_scope
{
    explicit async_ambient_install_scope(async_ambient_handle const& handle) : _previous(impl::async_tls().ambient)
    {
        if (handle.head() != nullptr)
            impl::async_tls().ambient = handle.head();
    }
    ~async_ambient_install_scope() { impl::async_tls().ambient = _previous; }

    async_ambient_install_scope(async_ambient_install_scope const&) = delete;
    async_ambient_install_scope& operator=(async_ambient_install_scope const&) = delete;

private:
    void* _previous = nullptr;
};

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
