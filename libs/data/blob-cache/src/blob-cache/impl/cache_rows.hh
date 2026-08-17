#pragma once

#include <blob-cache/fwd.hh>
#include <blob-cache/keys.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>

/// The plain structs that cross the storage boundary.
/// No sqlite type appears in any signature here, which is what lets everything above cache_io stay engine-agnostic.

namespace bcache::impl
{
/// One entry joined to its object — what a lookup reads before it touches a single payload byte.
struct entry_row
{
    i64 entry_id = 0;
    i64 object_id = 0;
    content_hash hash;
    i64 size = 0;
    i64 chunk_count = 0;
    cc::optional<double> expires_at; ///< absent means never
    cc::vector<byte> metadata;
};

/// What a put needs to write, already hashed and measured.
struct put_row
{
    cache_key key;
    content_hash hash;
    i64 size = 0;
    double created_at = 0;
    cc::optional<double> expires_at;
    cc::optional<double> compute_secs;
    cc::vector<byte> metadata;
};

/// One eviction candidate, and the object it would decrement.
struct candidate_row
{
    i64 entry_id = 0;
    i64 object_id = 0;
    i64 size = 0;
};

/// The authoritative totals, re-read rather than trusted incrementally.
/// Another process's writes are invisible to a counter of ours, which is why this is a query and not a member.
struct size_totals
{
    i64 stored_bytes = 0;
    i64 entry_count = 0;
    i64 file_bytes = 0;
};

/// The inputs the eviction score is computed from, passed down so the policy is stated in ONE place.
struct score_parameters
{
    double now = 0;
    double default_compute_secs = 0.01;
    double half_life_secs = 3600;
};
} // namespace bcache::impl
