#include "hash.hh"

#include <clean-core/container/span.hh>
#include <xxhash.h>

cc::u64 cc::make_hash_of_bytes(cc::span<cc::byte const> data, cc::u64 seed)
{
    return XXH3_64bits_withSeed(data.data(), static_cast<size_t>(data.size()), seed);
}
