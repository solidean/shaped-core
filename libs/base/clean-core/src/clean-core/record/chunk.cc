#include "chunk.hh"

#include <clean-core/record/chunk_pool.hh>

#include <new> // std::nothrow, so a spill that cannot be allocated falls back to an inline copy rather than throwing

void cc::rec::chunk::release_ref()
{
    if (refs.fetch_sub(1, cc::memory_order_acq_rel) != 1)
        return;

    CC_ASSERT(pool != nullptr, "a chunk without a pool cannot be recycled");
    pool->recycle(this);
}

namespace
{
/// One full array of pins, moved off the chunk header so the header can take more.
struct pin_spill
{
    cc::rec::pin pins[cc::rec::chunk::pin_capacity] = {};
};

/// Releases a spill block's pins and frees it.
/// A block's first slot may itself be an earlier spill, so this unwinds the whole chain one link at a time.
void release_pin_spill(void* object)
{
    auto* const block = static_cast<pin_spill*>(object);
    for (auto const& p : block->pins)
        if (p.release != nullptr)
            p.release(p.object);
    delete block;
}
} // namespace

bool cc::rec::chunk::try_add_pin(cc::rec::pin p)
{
    auto n = pin_count.load(cc::memory_order_relaxed);

    if (isize(n) >= pin_capacity)
    {
        // The array moves into a heap block and the block becomes ONE pin, so 63 slots come free.
        // Repeatable without bound: the next spill carries the previous block along as its first entry, which is why
        // releasing one walks a chain rather than a flat list.
        auto* const block = new (std::nothrow) pin_spill();
        if (block == nullptr)
            return false;

        for (isize i = 0; i < pin_capacity; ++i)
        {
            block->pins[i] = pins[i];
            pins[i] = {};
        }

        pins[0] = {.object = block, .release = &release_pin_spill};
        n = 1;
        pin_count.store(n, cc::memory_order_release);
    }

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
