#include "blake3.hh"

#include <blake3.h>
#include <clean-core/common/utility.hh> // cc::placement_new

using namespace cc::primitive_defines;

namespace
{
/// The state buffer only ever holds a blake3_hasher, placement-new'd by the constructor.
[[nodiscard]] blake3_hasher* hasher_of(byte* state)
{
    return reinterpret_cast<blake3_hasher*>(state);
}
[[nodiscard]] blake3_hasher const* hasher_of(byte const* state)
{
    return reinterpret_cast<blake3_hasher const*>(state);
}

[[nodiscard]] cc::hash256 to_hash256(uint8_t const (&digest)[BLAKE3_OUT_LEN])
{
    return cc::hash256::from_bytes(cc::span<byte const>(reinterpret_cast<byte const*>(digest), BLAKE3_OUT_LEN));
}
} // namespace

cc::hash256 cc::blake3::create(cc::span<byte const> data)
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data.data(), static_cast<size_t>(data.size()));

    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, digest, BLAKE3_OUT_LEN);
    return to_hash256(digest);
}

cc::blake3::blake3()
{
    static_assert(sizeof(blake3_hasher) <= state_size, "BLAKE3 grew its state — raise cc::blake3::state_size");
    static_assert(alignof(blake3_hasher) <= 16, "BLAKE3 wants more alignment than the state buffer provides");

    new (cc::placement_new, _state) blake3_hasher;
    blake3_hasher_init(hasher_of(_state));
}

void cc::blake3::update(cc::span<byte const> data)
{
    blake3_hasher_update(hasher_of(_state), data.data(), static_cast<size_t>(data.size()));
}

cc::hash256 cc::blake3::finalize() const
{
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(hasher_of(_state), digest, BLAKE3_OUT_LEN);
    return to_hash256(digest);
}

void cc::blake3::reset()
{
    blake3_hasher_reset(hasher_of(_state));
}
