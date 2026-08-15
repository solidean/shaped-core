#pragma once

#include <clean-core/common/hash256.hh>
#include <clean-core/common/macros.hh> // CC_PURE on create
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

/// BLAKE3-256, the cryptographic hash.
///
/// clean-core carries two hashes and they are not interchangeable.
/// cc::hash / cc::hash128 are XXH3: fast, and collidable on purpose by anyone who wants to.
/// Reach for them for in-memory maps, caches and change detection — anything where an adversary cannot profit.
/// Reach for cc::blake3 only where a value must survive an adversary: content addressing, integrity of data
/// received from a peer, anything a second party recomputes to decide whether to trust what it was handed.
///
/// It costs the better part of an order of magnitude more than XXH3 — see tests/benchmarks/hash-benchmark.cc,
/// which measures both side by side rather than leaving that a claim.
/// So it belongs at the boundary where data enters or leaves, once per item, and never on a query, a parse or a
/// per-frame path.
/// libs/data/versioned-document/docs/decisions.md ("BLAKE3, over 32-byte ids") argues both halves of that,
/// and records the standing reservation about the cost.
///
/// Both roles live on this one type: create() for a byte range that already exists, and the streaming state
/// for a sequence built in pieces, which is how a payload is hashed without concatenating it first.
class cc::blake3
{
public:
    /// Computes the BLAKE3-256 hash of `data`. Empty data is valid.
    [[nodiscard]] CC_PURE static hash256 create(cc::span<byte const> data);

    /// A fresh state, equivalent to having hashed nothing.
    blake3();

    /// Appends `data` to the byte sequence being hashed.
    /// Splitting a sequence into different chunks never changes the result.
    void update(cc::span<byte const> data);

    /// The hash of everything appended so far.
    /// Does not consume the state: update() may continue afterwards, and finalizing twice gives the same value.
    [[nodiscard]] hash256 finalize() const;

    /// Returns to the fresh state, dropping everything appended.
    void reset();

private:
    /// Opaque storage for upstream's blake3_hasher, so <blake3.h> stays private to blake3.cc.
    /// The .cc static_asserts that the real state fits and is not over-aligned; a version bump that outgrows
    /// this fails the build rather than the tests.
    static constexpr isize state_size = 1984;

    alignas(16) byte _state[state_size] = {};
};
