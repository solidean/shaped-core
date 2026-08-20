#include "hash256.hh"

#include <clean-core/bytes/blake3.hh>
#include <clean-core/common/assert.hh>

cc::hash256 cc::hash256::create(cc::span<byte const> data)
{
    return cc::blake3::create(data);
}

void cc::hash256::to_bytes(cc::span<byte> out) const
{
    CC_ASSERT(out.size() == 32, "hash256 is exactly 32 bytes");

    u64 const limbs[4] = {l0, l1, l2, l3};
    for (auto i = 0; i < 4; ++i)
        for (auto b = 0; b < 8; ++b)
            out[i * 8 + b] = static_cast<byte>((limbs[i] >> (b * 8)) & 0xFF);
}

cc::hash256 cc::hash256::from_bytes(cc::span<byte const> data)
{
    CC_ASSERT(data.size() == 32, "hash256 is exactly 32 bytes");

    u64 limbs[4] = {};
    for (auto i = 0; i < 4; ++i)
        for (auto b = 0; b < 8; ++b)
            limbs[i] |= u64(data[i * 8 + b]) << (b * 8);

    return {limbs[0], limbs[1], limbs[2], limbs[3]};
}
