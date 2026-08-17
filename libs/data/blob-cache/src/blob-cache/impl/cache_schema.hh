#pragma once

#include <babel-serializer/data/sqlite.hh>
#include <blob-cache/fwd.hh>
#include <clean-core/error/result.hh>

/// The on-disk shape, and the one rule that keeps it from ever needing a migration.
///
/// **An incompatible file is discarded, never refused.** vdoc::file rightly refuses a format version from the future, because guessing would risk somebody's document.
/// A cache holds nothing anyone would miss, so refusing would only strand a caller on a stale file forever — and the disposability invariant says throwing it away is always safe.
/// That is why there is no migration code here and never will be.

namespace bcache::impl
{
/// 'BCHE' — what this file is, to anything inspecting it.
/// SQLite never reads it.
inline constexpr i32 bcache_application_id = 0x42434845;

/// The format version.
/// Any mismatch in EITHER direction discards the file.
inline constexpr i32 current_user_version = 1;

/// One stored object is split into chunks of at most this many bytes.
///
/// Chunking is not optional: SQLite caps a single value near a gigabyte, so a large object could not be one row.
/// 1 MiB then puts per-row overhead under 0.01%, bounds the buffer a bind() copies on the write path, and makes even a gigabyte object only 1024 rows.
/// It does NOT bound the read path, which streams into the caller's buffer through a blob handle and stages nothing.
inline constexpr i64 chunk_size_bytes = i64(1) << 20;

/// What ensure_schema had to do to make the connection usable.
enum class schema_outcome : u8
{
    opened_existing,
    created_fresh,
    discarded_and_recreated
};

/// Brings `db` to the current schema, recreating it from scratch where the file is not usable as it stands.
///
/// Configures the connection first — busy timeout, foreign keys (object_chunk's cascade depends on them and so they are read back), WAL, synchronous, and incremental auto-vacuum.
///
/// **auto_vacuum must be set before the first table exists**, since it cannot be changed later without a full VACUUM.
/// Get that one statement out of order and the file's freed pages never return to the operating system.
[[nodiscard]] cc::result<schema_outcome> ensure_schema(babel::sqlite::database& db);

/// Drops every table this build knows and recreates them, in one transaction on the open connection.
///
/// This rather than unlinking the file: other processes hold the same path, and they see the new schema at their next statement instead of a file that vanished under them.
[[nodiscard]] cc::result<cc::unit> recreate_schema(babel::sqlite::database& db);
} // namespace bcache::impl
