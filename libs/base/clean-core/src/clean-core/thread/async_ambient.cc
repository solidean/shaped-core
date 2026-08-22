#include <clean-core/memory/node_allocation.hh>
#include <clean-core/thread/async_ambient.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr auto ambient_class = cc::node_class_index_for<cc::async_ambient_link>();
}

void cc::impl::async_ambient_free(async_ambient_link* l)
{
    // Iterative: releasing a link releases its parent, and a chain is as deep as a consumer nests scopes.
    while (l != nullptr)
    {
        auto* const parent = l->parent;
        l->~async_ambient_link();
        cc::node_allocation_free(reinterpret_cast<byte*>(l), ambient_class);

        if (parent == nullptr || parent->refs.fetch_sub(1, cc::memory_order_acq_rel) != 1)
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

    _link = new (cc::placement_new, raw) async_ambient_link{tag, value, parent, {1}};
    impl::async_tls().ambient = _link;

    // Creating a scope always changes the context, so this never needs the compare the poll sites do.
    cc::rec::impl::note_ambient_change(_link);
}

// TEMPORARY ARM PROBE
namespace cc::impl
{
cc::atomic<int> g_probe_ambient_scope_dtors = {0};
} // namespace cc::impl

cc::async_ambient_scope::~async_ambient_scope()
{
    CC_ASSERT(impl::async_tls().ambient == _link,
              "ambient scopes must pop in LIFO order — something installed a link without a scope, or a scope "
              "outlived the thread it was pushed on");

    // Deliberately no "nothing outstanding" assert here.
    // The link is refcounted, so work outliving this scope is safe rather than dangling — see the type's docs.

    impl::g_probe_ambient_scope_dtors.fetch_add(1, cc::memory_order_relaxed);
    impl::async_tls().ambient = _link->parent;
    cc::rec::impl::note_ambient_change(_link->parent);
    impl::async_ambient_release(_link);
}

i32 cc::async_ambient_scope::outstanding() const
{
    return cc::async_ambient_outstanding(_link);
}
