#pragma once

#include <blob-cache/fwd.hh>
#include <clean-core/common/blake3.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/hash256.hh>
#include <clean-core/common/utility.hh> // cc::memcmp
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

/// What a cache entry is identified BY, and what its bytes are identified by.
///
/// The two are deliberately different types and must never be confused:
///
///   logical key  -> identifies a COMPUTATION, and exists before it runs
///   content hash -> identifies the resulting BYTES, and cannot exist until it has
///
/// Singleflight keys on the first.
/// Deduplication keys on the second.

/// The coarse partition a caller owns: "shader", "texture-mips", "mesh-bvh".
///
/// Namespaces never collide, and clear() is scoped to exactly one.
/// Keep it short and stable — it is stored verbatim on every entry row, and a renamed namespace strands its entries until GC gets to them.
struct bcache::cache_namespace
{
    cc::string name;

    cache_namespace() = default;
    explicit cache_namespace(cc::string_view n) : name(n) {}

    [[nodiscard]] friend bool operator==(cache_namespace const& a, cache_namespace const& b)
    {
        return a.name == b.name;
    }
    [[nodiscard]] friend u64 hash(cache_namespace const& v) { return cc::make_hash(v.name); }
};

/// The caller's identity for an entry inside a namespace — opaque bytes this library never interprets.
///
/// Stored VERBATIM and compared byte-for-byte, never hashed down.
/// Hashing the key here would let a collision hand back another computation's blob, which is the one failure mode a cache may not have: a cache is allowed to miss, never to lie.
/// A caller who wants a fixed-size key hashes its own inputs and passes the digest — that collision is then its own to reason about.
struct bcache::logical_key
{
    cc::vector<cc::byte> bytes;

    [[nodiscard]] static logical_key create_from_string(cc::string_view s);
    [[nodiscard]] static logical_key create_from_bytes(cc::span<cc::byte const> b);

    /// The composite-key path: fold the inputs into one digest — a cc::byte_stream_builder through cc::hash256::create — and hand that here.
    /// Those 32 bytes are then what gets stored and compared.
    [[nodiscard]] static logical_key create_from_hash(cc::hash256 const& h);

    [[nodiscard]] friend bool operator==(logical_key const& a, logical_key const& b)
    {
        if (a.bytes.size() != b.bytes.size())
            return false;
        return cc::memcmp(a.bytes.data(), b.bytes.data(), size_t(a.bytes.size())) == 0;
    }

    /// XXH3 over the bytes.
    /// Feeds in-memory maps only and is NEVER persisted, so it may change between builds;
    /// equality still compares the bytes, which is why a collision here costs a probe rather than a wrong answer.
    [[nodiscard]] friend u64 hash(logical_key const& v) { return cc::make_hash_of_bytes(v.bytes); }
};

/// What a caller looks up: (namespace, key, version).
///
///     auto const k = bcache::cache_key{.space = bcache::cache_namespace("shader"), .key = bcache::logical_key::create_from_string(source_id), .version = bcache::version(3)};
struct bcache::cache_key
{
    cache_namespace space;
    logical_key key;
    bcache::version version = bcache::version::none;

    [[nodiscard]] friend bool operator==(cache_key const& a, cache_key const& b)
    {
        return a.version == b.version && a.space == b.space && a.key == b.key;
    }
    [[nodiscard]] friend u64 hash(cache_key const& v)
    {
        return cc::make_hash_finalized(v.space, v.key, i32(v.version));
    }
};

/// BLAKE3-256 over the stored bytes: the object identity every logical entry points at.
///
/// Two entries whose blobs are byte-identical name ONE object, which is what makes evicting one of them free nothing.
/// Cryptographic on purpose: an object is addressed by this, so a collision would serve the wrong bytes.
struct bcache::content_hash
{
    cc::hash256 value;

    [[nodiscard]] static content_hash create(cc::span<cc::byte const> bytes) { return {cc::blake3::create(bytes)}; }

    [[nodiscard]] friend bool operator==(content_hash const& a, content_hash const& b) { return a.value == b.value; }
    [[nodiscard]] friend u64 hash(content_hash const& v) { return v.value.l0; }
};
