#pragma once

#include <clean-core/common/macros.hh> // CC_PURE on create
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

/// A 256-bit hash value, stored as four u64 limbs.
/// Structurally comparable; the default operator<=> orders lexicographically by (l0, l1, l2, l3).
/// That order is total and deterministic, which is all a tiebreak needs — but it is not the order of the
/// 32 canonical bytes, so it does not agree with sorting the hex digests a tool like b3sum prints.
struct cc::hash256
{
    u64 l0 = 0;
    u64 l1 = 0;
    u64 l2 = 0;
    u64 l3 = 0;

    [[nodiscard]] friend constexpr bool operator==(hash256 const&, hash256 const&) = default;
    [[nodiscard]] friend constexpr auto operator<=>(hash256 const&, hash256 const&) = default;

    /// Computes the BLAKE3-256 hash of `data`.
    /// Empty data is valid (hashes the empty input).
    /// This is the one-shot form of cc::blake3, which also hashes a byte sequence built in pieces.
    [[nodiscard]] CC_PURE static hash256 create(cc::span<byte const> data);

    /// The canonical 32-byte form: each limb little-endian, l0 first.
    /// This is what a file or a wire protocol stores, and it is the same bytes on every platform.
    /// `out` must be exactly 32 bytes.
    void to_bytes(cc::span<byte> out) const;

    /// Reads back what to_bytes wrote; `data` must be exactly 32 bytes.
    [[nodiscard]] static hash256 from_bytes(cc::span<byte const> data);

    /// ADL customization point (see common/hash.hh).
    /// A hash256 is already a hash, so its first limb is surfaced as the 64-bit hash.
    [[nodiscard]] friend constexpr u64 hash(hash256 const& v) { return v.l0; }
};
