#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// Opaque SQLite C handles — forward-declared so <sqlite3.h> stays out of this header (and out of consumers).
// These are the exact tag names SQLite typedefs, so `sqlite3*` / `sqlite3_stmt*` resolve without the real header.
struct sqlite3;
struct sqlite3_stmt;

// SQLite reader (data/).
//
// Unlike json / obj this is NOT a one-shot stream parser: SQLite is a live database engine, so babel::sqlite is a
// thin RAII wrapper over an open connection you keep talking to — open a file (or :memory: / a byte image), run SQL,
// iterate result rows, execute statements. There is deliberately no read(cc::read_stream&): a live engine wants a
// file or an in-memory image, not a forward-only byte window. open_blob covers "I already have the bytes".
//
// The engine backend is fetched on demand (extern/sqlite), so it may be absent from a raw checkout. That is babel's
// private concern: this API is ALWAYS declared and callable. When the backend was not compiled in, is_available()
// returns false and every open_* factory returns an error — absence is an ordinary runtime failure on a path that is
// error-riddled anyway, never a compile-time hole in the API. See docs/coding-guidelines.md.
//
//   auto db = babel::sqlite::database::open_memory().value();
//   db.exec("CREATE TABLE t(id INTEGER, name TEXT)");
//   db.exec("INSERT INTO t VALUES (1, 'shaped')");
//   auto stmt = db.query("SELECT id, name FROM t WHERE id = ?1").value();
//   stmt.bind(1, 1);
//   for (auto row : stmt)
//       use(row.as_i64(0), row.as_string(1));
//
// [planned] A compile-time-validated, typed query layer (in the spirit of cc::format) is intended on top of this.

namespace babel::sqlite
{
/// True when the SQLite backend was compiled in (the extern/sqlite target was fetched and linked).
/// When false, every database::open_* returns an error. A runtime probe — callers never need a macro.
[[nodiscard]] bool is_available();

/// The dynamic type of a result column, as reported by SQLite for the current row.
enum class column_kind : cc::u8
{
    null,
    integer,
    real,
    text,
    blob,
};

/// A non-owning view of the statement's current result row. Valid only until the next step / the statement dies.
/// Columns are 0-based. Accessors follow SQLite's type coercion (e.g. as_i64 on a text cell parses it).
struct row
{
    row() = default;
    explicit row(sqlite3_stmt* stmt) : _stmt(stmt) {}

    [[nodiscard]] i32 column_count() const;
    [[nodiscard]] cc::string_view column_name(i32 col) const;
    [[nodiscard]] column_kind column_type(i32 col) const;
    [[nodiscard]] bool is_null(i32 col) const;

    [[nodiscard]] i64 as_i64(i32 col) const;
    [[nodiscard]] double as_double(i32 col) const;
    [[nodiscard]] cc::string_view as_string(i32 col) const; // bytes owned by SQLite; valid until the next step
    [[nodiscard]] cc::span<cc::byte const> as_blob(i32 col) const;

private:
    sqlite3_stmt* _stmt = nullptr;
};

/// A prepared statement: bind parameters, then step through result rows.
/// Move-only (owns the sqlite3_stmt). Obtain one from database::prepare / database::query.
///
/// Iterate with a range-for (single pass). Row-stepping cannot return a result per iteration, so a step failure is
/// sticky: it ends the loop and is readable afterwards via is_ok() / error(). For explicit control use next().
class statement
{
public:
    statement() = default;
    ~statement();
    statement(statement&&) noexcept;
    statement& operator=(statement&&) noexcept;
    statement(statement const&) = delete;
    statement& operator=(statement const&) = delete;

    /// Bind a value to parameter `index` (1-based, SQLite convention). Text / blob bytes are copied by SQLite.
    cc::result<cc::unit> bind(i32 index, i64 value);
    cc::result<cc::unit> bind(i32 index, double value);
    cc::result<cc::unit> bind(i32 index, cc::string_view value);
    cc::result<cc::unit> bind(i32 index, cc::span<cc::byte const> value);
    cc::result<cc::unit> bind_null(i32 index);

    /// Advance to the next result row. true = a row is now current (read it via the range-for row, or column_*/as_*
    /// on current()), false = no more rows. Errors surface here and also set the sticky error.
    [[nodiscard]] cc::result<bool> next();

    /// The current row view (valid after next() returned true).
    [[nodiscard]] row current() const { return row(_stmt); }

    /// Reset back to before the first row so the statement can be re-executed; keeps bound parameters.
    cc::result<cc::unit> reset();
    /// Clear all bound parameters back to NULL.
    void clear_bindings();

    [[nodiscard]] bool is_ok() const { return _ok; }
    [[nodiscard]] cc::string_view error() const { return _error; }

    // range-for support (single-pass input iteration over rows)
    struct end_sentinel
    {
    };
    struct iterator
    {
        statement* stmt = nullptr;
        [[nodiscard]] row operator*() const { return stmt->current(); }
        iterator& operator++()
        {
            stmt->_advance();
            return *this;
        }
        [[nodiscard]] bool operator==(end_sentinel) const { return stmt == nullptr || stmt->_at_end; }
        [[nodiscard]] bool operator!=(end_sentinel s) const { return !(*this == s); }
    };
    [[nodiscard]] iterator begin();
    [[nodiscard]] end_sentinel end() const { return {}; }

private:
    friend class database;
    explicit statement(sqlite3_stmt* stmt) : _stmt(stmt) {}
    void _advance(); // step once, updating _at_end and the sticky error

    sqlite3_stmt* _stmt = nullptr;
    bool _at_end = false;
    bool _ok = true;
    cc::string _error;
};

/// A live SQLite database connection. Move-only (owns the sqlite3 handle; closed in the destructor).
/// Full read/write: exec arbitrary SQL, prepare/query statements, run DDL and transactions.
class database
{
public:
    database() = default;
    ~database();
    database(database&&) noexcept;
    database& operator=(database&&) noexcept;
    database(database const&) = delete;
    database& operator=(database const&) = delete;

    /// Open (create if missing) an on-disk database for reading and writing.
    [[nodiscard]] static cc::result<database> open(cc::string_view path);
    /// Open an existing on-disk database read-only. Errors if the file does not exist.
    [[nodiscard]] static cc::result<database> open_readonly(cc::string_view path);
    /// A transient in-memory database (":memory:").
    [[nodiscard]] static cc::result<database> open_memory();
    /// Load a database from a serialized in-memory image (a copy is taken; the source bytes need not outlive this).
    [[nodiscard]] static cc::result<database> open_blob(cc::span<cc::byte const> bytes);

    /// Run one or more SQL statements that yield no result rows (DDL, INSERT/UPDATE/DELETE, PRAGMA, transactions).
    cc::result<cc::unit> exec(cc::string_view sql);
    /// Prepare a single statement for binding + stepping.
    [[nodiscard]] cc::result<statement> prepare(cc::string_view sql);
    /// Convenience: prepare a single statement ready to iterate. Same as prepare; reads as intent at the call site.
    [[nodiscard]] cc::result<statement> query(cc::string_view sql) { return prepare(sql); }

    /// Serialize the main database to a contiguous byte image (round-trips through open_blob). Empty on failure.
    [[nodiscard]] cc::vector<cc::byte> serialize() const;

    [[nodiscard]] i64 last_insert_rowid() const;
    [[nodiscard]] i64 changes() const;

private:
    explicit database(sqlite3* db) : _db(db) {}
    [[nodiscard]] static cc::result<database> open_with_flags(cc::string_view path, int flags);

    sqlite3* _db = nullptr;
};
} // namespace babel::sqlite
