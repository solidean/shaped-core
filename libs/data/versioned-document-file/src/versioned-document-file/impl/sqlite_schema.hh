#pragma once

#include <babel-serializer/data/sqlite.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/fwd.hh>

/// The `.vdoc` schema: the SQL text in one place, and the checks that decide whether a file can be opened at all.
///
/// Exactly what [the format](../../../docs/format.md#schema) specifies, and nothing beside it.
/// **Create what is missing; never rewrite what is there.**
///
/// The shape check runs BEFORE anything is created, which is the whole point:
/// a `CREATE TABLE IF NOT EXISTS` over an incompatible old table leaves it in place and every later statement fails obscurely, so the real
/// problem is reported here, naming the table and the column.

namespace vdoc::file::impl
{
/// 'VDOC' — what identifies the file as ours to anything inspecting it, `file(1)` included.
/// SQLite itself never reads it.
inline constexpr i32 vdoc_application_id = 0x56444F43;

/// The format version this build writes and reads.
/// A HIGHER one is a hard failure: the file may use shapes this build would misread, and guessing is worse than refusing.
inline constexpr i32 current_user_version = 2;

/// What ensuring the schema found that the loader has to report.
struct schema_scan
{
    /// Tables this build does not know; ignored, and left untouched.
    cc::vector<cc::string> unknown_tables;
    /// Extra columns on tables this build does know, as "<table>.<column>"; ignored, and preserved.
    cc::vector<cc::string> unknown_columns;
};

/// Configures the connection, checks the identity and the version, and creates whatever tables are missing.
///
/// Every failure here is HARD, and each says the real thing rather than leaving a later statement to fail obscurely:
/// not a database, a damaged image, a foreign application_id, a version from the future, foreign keys that would not enable, an incompatible
/// table shape, or a schema transaction that would not commit.
[[nodiscard]] cc::result<schema_scan> ensure_schema(babel::sqlite::database& db);
} // namespace vdoc::file::impl
