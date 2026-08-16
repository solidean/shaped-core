#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/fwd.hh>

/// Aggregate forward declarations for blob-cache.
///
/// This header is also the API index: every name the library exposes is declared here, with the one line that says what it is.
/// The design behind it is [docs/design.md](../../docs/design.md).
///
/// The cache stores immutable bytes on local disk, keyed by (namespace, key, version) and deduplicated by content hash.
/// It is shared by unrelated processes on the same machine, and it is an OPTIMIZATION AND NOTHING ELSE:
/// deleting the file at any moment changes nothing a caller computes, only how long it takes.

namespace bcache
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside bcache without leaking them into the global namespace.
using namespace cc::primitive_defines;

/// The bytes of one cached object.
///
/// A shared owning view, so handing the same object to several callers copies nothing.
/// It OUTLIVES the cache: a hit taken before close() stays valid after it, because the pin owns the buffer outright.
using blob = cc::pinned_data<byte const>;
} // namespace bcache

// ---- the cache ---------------------------------------------------------------------------------

namespace bcache
{
/// A persistent, multi-process blob cache: (namespace, key, version) -> content-addressed bytes.
class blob_cache;

/// How a cache is opened: where the file is, what it may consume, and how it keeps itself.
struct cache_config;

/// The ceilings a garbage-collection pass enforces.
/// All approximate — see the field docs.
struct cache_limits;
} // namespace bcache

// ---- identity ----------------------------------------------------------------------------------

namespace bcache
{
/// The coarse partition a caller owns: "shader", "texture-mips", "mesh-bvh".
struct cache_namespace;

/// The caller's identity for an entry inside a namespace — opaque bytes this library never interprets.
struct logical_key;

/// What a caller looks up: (namespace, key, version).
struct cache_key;

/// BLAKE3-256 over the stored bytes: the object identity every logical entry points at.
struct content_hash;

enum class version : i32;
} // namespace bcache

/// The producer's schema or algorithm version for an entry.
/// Bumping it makes every older entry under the same logical key unreachable, and GC reclaims them as ordinary cold entries.
enum class bcache::version : bcache::i32
{
    none = 0
};

// ---- operations --------------------------------------------------------------------------------

namespace bcache
{
/// What get() found: the bytes, their content hash, and whatever metadata the writer attached.
struct cache_hit;

/// What one put actually did.
enum class put_status : u8;

/// The outcome of a put, plus the content hash it computed on the way.
struct put_result;

/// Applied to the entry a put creates.
struct put_options;

/// Applied to the entry an acquire may create, plus the knobs that only make sense around a compute.
struct acquire_options;

/// What one garbage-collection pass reclaimed.
struct gc_result;

/// Counters since the cache was created.
/// Cheap, and never touches the database.
struct cache_stats;
} // namespace bcache

// ---- internals ---------------------------------------------------------------------------------

namespace bcache::impl
{
/// The shared state an in-flight acquire reaches back into, so a caller destroying its handle mid-flight is safe.
struct cache_core;

/// The in-process singleflight table.
class flight_table;

/// One registration in that table: the operation, held weakly, plus the generation that dates it.
struct flight_slot;
} // namespace bcache::impl
