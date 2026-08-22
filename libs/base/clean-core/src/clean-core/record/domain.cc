#include "domain.hh"

#include <clean-core/string/string_view.hh>

using namespace cc::primitive_defines;

namespace
{
/// The head of the process-wide domain list.
/// Constant-initialized, so a domain registering from static initialization finds a valid list whatever the link order.
cc::atomic<cc::rec::domain*> g_registry_head = nullptr;

/// The mask set_all_domains_enabled_mask last applied, or `no_override`.
/// Kept so a domain registering LATER — a plugin's static initializers, a lazily loaded module — inherits the same
/// configuration rather than silently reverting to the compile-time default.
constexpr u32 no_override = ~u32(0);
cc::atomic<u32> g_mask_override = no_override;

/// The bits enable_all_domains_mask_bits has turned on so far, for the same reason g_mask_override exists.
/// Separate from the override rather than folded into it: one is "this is the mask", the other is "these are on",
/// and a later domain wants both applied in that order.
cc::atomic<u32> g_mask_enabled_bits = 0;
} // namespace

cc::rec::impl::domain_registrar::domain_registrar(cc::rec::domain& d)
{
    if (auto const mask = g_mask_override.load(cc::memory_order_relaxed); mask != no_override)
        d.set_enabled_mask(mask);

    if (auto const bits = g_mask_enabled_bits.load(cc::memory_order_relaxed); bits != 0)
        d.set_enabled_mask(d.enabled_mask() | bits);

    auto* head = g_registry_head.load(cc::memory_order_relaxed);
    do
    {
        d._registry_next = head;
    } while (!g_registry_head.compare_exchange_weak(head, &d, cc::memory_order_release, cc::memory_order_relaxed));

    d._is_registered.store(true, cc::memory_order_relaxed);
}

void cc::rec::for_each_domain(cc::function_ref<void(cc::rec::domain&)> f)
{
    for (auto* d = g_registry_head.load(cc::memory_order_acquire); d != nullptr; d = d->registry_next())
        f(*d);
}

cc::rec::domain* cc::rec::find_domain(cc::string_view name)
{
    for (auto* d = g_registry_head.load(cc::memory_order_acquire); d != nullptr; d = d->registry_next())
        if (cc::string_view(d->name()) == name)
            return d;
    return nullptr;
}

void cc::rec::set_all_domains_enabled_mask(u32 mask)
{
    g_mask_override.store(mask, cc::memory_order_relaxed);
    cc::rec::for_each_domain([mask](cc::rec::domain& d) { d.set_enabled_mask(mask); });
}

void cc::rec::enable_all_domains_mask_bits(u32 bits)
{
    if (bits == 0)
        return;

    g_mask_enabled_bits.store(g_mask_enabled_bits.load(cc::memory_order_relaxed) | bits, cc::memory_order_relaxed);
    cc::rec::for_each_domain([bits](cc::rec::domain& d) { d.set_enabled_mask(d.enabled_mask() | bits); });
}

namespace cc::rec
{
CC_REC_DEFINE_DOMAIN(g_default_domain, "default");
CC_REC_DEFINE_DOMAIN(g_system_domain, "cc.record");
} // namespace cc::rec

namespace cc
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "cc");
} // namespace cc
