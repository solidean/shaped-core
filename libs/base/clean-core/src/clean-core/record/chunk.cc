#include "chunk.hh"

#include <clean-core/record/chunk_pool.hh>

void cc::rec::chunk::release_ref()
{
    if (refs.fetch_sub(1, cc::memory_order_acq_rel) != 1)
        return;

    CC_ASSERT(pool != nullptr, "a chunk without a pool cannot be recycled");
    pool->recycle(this);
}

bool cc::rec::chunk::try_add_pin(cc::rec::pin p)
{
    auto const n = pin_count.load(cc::memory_order_relaxed);
    if (isize(n) >= pin_capacity)
        return false;

    pins[n] = p;
    pin_count.store(n + 1, cc::memory_order_release); // publishes the pin before any event that references it
    return true;
}

void cc::rec::chunk::release_pins()
{
    auto const n = pin_count.load(cc::memory_order_relaxed);
    for (u32 i = 0; i < n; ++i)
        if (pins[i].release != nullptr)
            pins[i].release(pins[i].object);

    for (u32 i = 0; i < n; ++i)
        pins[i] = {};

    pin_count.store(0, cc::memory_order_relaxed);
}
