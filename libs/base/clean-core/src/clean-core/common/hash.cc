#include "hash.hh"

#include <clean-core/container/span.hh>
#include <xxhash.h>

using namespace cc::primitive_defines;

u64 cc::make_hash_of_bytes(cc::span<byte const> data, u64 seed)
{
    return XXH3_64bits_withSeed(data.data(), static_cast<size_t>(data.size()), seed);
}
