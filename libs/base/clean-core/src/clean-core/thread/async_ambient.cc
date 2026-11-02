#include <clean-core/memory/node_allocation.hh>
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/thread.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr auto ambient_class = cc::node_class_index_for<cc::async_ambient_link>();

#if CC_ASYNC_AMBIENT_TRACE
// The armed watch's hook and its argument, written before the link is published and read after it is seen.
cc::atomic<cc::async_ambient_release_hook> g_watch_hook = nullptr;
cc::atomic<void*> g_watch_user = nullptr;
// Set by whoever arms a watch, so two cannot interleave their writes; cleared by the unwatch, last.
cc::atomic<bool> g_watch_claimed = false;
// Drops of the watched link inside their hook check, which the unwatch waits out.
cc::atomic<i32> g_watch_in_flight = 0;
#endif
} // namespace

#if CC_ASYNC_AMBIENT_TRACE
constinit cc::atomic<cc::async_ambient_link const*> cc::impl::async_ambient_watched = nullptr;

bool cc::impl::async_ambient_drop_watched(async_ambient_link* l)
{
    // Counted in before the count goes down, so a watcher that saw the drop also sees this in flight.
    // Seq_cst against the unwatch's store and load: either it waits for this drop, or this drop sees it gone.
    g_watch_in_flight.fetch_add(1, cc::memory_order_seq_cst);
    auto const watched = async_ambient_watched.load(cc::memory_order_seq_cst) == l;
    auto const left = l->refs.fetch_sub(1, cc::memory_order_acq_rel) - 1;
    if (watched)
        if (auto const hook = g_watch_hook.load(cc::memory_order_acquire); hook != nullptr)
            hook(l, left, g_watch_user.load(cc::memory_order_acquire));
    g_watch_in_flight.fetch_sub(1, cc::memory_order_seq_cst);
    return left == 0;
}
#endif

bool cc::async_ambient_watch_releases(void const* head, async_ambient_release_hook hook, void* user)
{
#if CC_ASYNC_AMBIENT_TRACE
    CC_ASSERT(head != nullptr && hook != nullptr, "a watch needs a link and a hook");
    if (g_watch_claimed.exchange(true, cc::memory_order_acquire))
        return false;
    g_watch_hook.store(hook, cc::memory_order_relaxed);
    g_watch_user.store(user, cc::memory_order_relaxed);
    impl::async_ambient_watched.store(static_cast<async_ambient_link const*>(head), cc::memory_order_seq_cst);
    return true;
#else
    (void)head, (void)hook, (void)user;
    return false;
#endif
}

void cc::async_ambient_unwatch_releases(void const* head)
{
#if CC_ASYNC_AMBIENT_TRACE
    auto expected = static_cast<async_ambient_link const*>(head);
    if (head == nullptr
        || !impl::async_ambient_watched.compare_exchange_strong(expected, nullptr, cc::memory_order_seq_cst))
        return;
    while (g_watch_in_flight.load(cc::memory_order_seq_cst) != 0)
        cc::this_thread_yield();
    g_watch_hook.store(nullptr, cc::memory_order_relaxed);
    g_watch_user.store(nullptr, cc::memory_order_relaxed);
    g_watch_claimed.store(false, cc::memory_order_release);
#else
    (void)head;
#endif
}

void cc::impl::async_ambient_free(async_ambient_link* l)
{
    // Iterative: releasing a link releases its parent, and a chain is as deep as a consumer nests scopes.
    while (l != nullptr)
    {
        auto* const parent = l->parent;
        l->~async_ambient_link();
        cc::node_allocation_free(reinterpret_cast<byte*>(l), ambient_class);

        if (parent == nullptr || !async_ambient_drop(parent))
            return;
        l = parent;
    }
}

cc::async_ambient_scope::async_ambient_scope(void const* tag, u64 value)
{
    CC_ASSERT(tag != nullptr, "an ambient tag must be a real address — see CC_ASYNC_AMBIENT_TAG");

    // The node slab: cross-thread safe to free, which this needs — the last holder of a link is whichever worker
    // finished the last node carrying it, not the thread that pushed the scope.
    auto* const raw = cc::default_node_allocator().allocate_node_bytes(ambient_class, sizeof(async_ambient_link),
                                                                       alignof(async_ambient_link));
    auto* const parent = static_cast<async_ambient_link*>(impl::async_tls().ambient);
    impl::async_ambient_retain(parent); // the link holds its parent strongly, so retaining a head retains the chain

    auto const mask = impl::async_ambient_tag_bit(tag) | (parent != nullptr ? parent->present_mask : 0);
    _link = new (cc::placement_new, raw) async_ambient_link{tag, value, parent, {1}, mask};
    impl::async_tls().ambient = _link;

    // Creating a scope always changes the context, so this never needs the compare the poll sites do.
    cc::rec::impl::note_ambient_change(_link);
}

cc::async_ambient_scope::~async_ambient_scope()
{
    CC_ASSERT(impl::async_tls().ambient == _link,
              "ambient scopes must pop in LIFO order — something installed a link without a scope, or a scope "
              "outlived the thread it was pushed on");

    // Deliberately no "nothing outstanding" assert here.
    // The link is refcounted, so work outliving this scope is safe rather than dangling — see the type's docs.

    impl::async_tls().ambient = _link->parent;
    cc::rec::impl::note_ambient_change(_link->parent);
    impl::async_ambient_release(_link);
}

i32 cc::async_ambient_scope::outstanding() const
{
    return cc::async_ambient_outstanding(_link);
}
