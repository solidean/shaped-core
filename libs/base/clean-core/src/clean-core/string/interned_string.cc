#include "interned_string.hh"

#include <clean-core/common/utility.hh>

using namespace cc::primitive_defines;

namespace
{
using entry = cc::impl::intern_entry;

constexpr isize entry_header = isize(sizeof(entry));
constexpr isize entry_align = isize(alignof(entry));

/// One arena block holds many entries; a string longer than this gets a block of its own.
constexpr isize block_size = 4096;

/// Bump-allocates an entry plus its bytes, and returns it.
/// The address is stable for the life of the shard: blocks are only ever appended to, never grown in place.
entry const* allocate_entry(cc::impl::intern_shard& shard, cc::string_view s, u64 h)
{
    auto const needed = cc::align_up(entry_header + s.size(), entry_align);

    if (shard.block_end - shard.cursor < needed)
    {
        shard.blocks.push_back(cc::allocation<byte>::create_uninitialized(cc::max(block_size, needed), nullptr));

        auto const block = shard.blocks.back().obj_span();
        shard.cursor = block.data();
        shard.block_end = block.data() + block.size();
    }

    // The bytes sit immediately behind the header, which is what intern_entry::as_string_view assumes.
    auto* const e = new (cc::placement_new, shard.cursor) entry{.hash = h, .size = s.size()};
    cc::memcpy(shard.cursor + entry_header, s.data(), size_t(s.size()));
    shard.cursor += needed;

    return e;
}

/// The process-wide table, deliberately never destroyed.
/// Handles from it must stay usable for the whole process, including from destructors running after main.
cc::string_interner& global_interner()
{
    alignas(cc::string_interner) static byte storage[sizeof(cc::string_interner)];
    static cc::string_interner* const table = new (cc::placement_new, storage) cc::string_interner();
    return *table;
}
} // namespace

cc::interned_string cc::string_interner::intern(cc::string_view s)
{
    // The empty string is the one value every table agrees on without storing anything, which is what makes a
    // default-constructed handle equal to intern("") — here and in any other table.
    if (s.empty())
        return interned_string();

    auto const h = cc::make_hash_of_bytes(s.as_bytes());

    // The map masks the low bits of the hash for its buckets, so the shard takes the high ones.
    auto& shard = _shards[(h >> 56) % shard_count];

    return shard.lock(
        [&](impl::intern_shard& sh)
        {
            if (auto const* const found = sh.index.get_or(s, nullptr))
                return interned_string(found);

            auto const* const e = allocate_entry(sh, s, h);

            // Keyed on the entry's own bytes, so the map holds no second copy and no pointer that can outlive it.
            sh.index[e->as_string_view()] = e;
            return interned_string(e);
        });
}

isize cc::string_interner::size()
{
    isize total = 0;
    for (auto& shard : _shards)
        total += shard.lock([](impl::intern_shard const& sh) { return sh.index.size(); });
    return total;
}

cc::interned_string cc::intern(cc::string_view s)
{
    return global_interner().intern(s);
}
