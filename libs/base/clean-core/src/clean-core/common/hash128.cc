#include "hash128.hh"

#include <xxhash.h>

cc::hash128 cc::hash128::create(cc::span<cc::byte const> data, cc::u64 seed)
{
    XXH128_hash_t const h = XXH3_128bits_withSeed(data.data(), static_cast<size_t>(data.size()), seed);
    return {h.low64, h.high64};
}
