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
struct sqlite3_blob;

// SQLite reader (data/).
//
// Unlike json / obj this is NOT a one-shot stream parser: SQLite is a live database engine.
// babel::sqlite is a thin RAII wrapper over an open connection you keep talking to — open a file (or :memory: /
// a byte image), run SQL, iterate result rows, execute statements.
// There is deliberately no read(cc::read_stream&): a live engine wants a file or an in-memory image, not a
// forward-only byte window, and open_blob covers "I already have the bytes".
//
// The engine backend is fetched on demand (extern/sqlite), so it may be absent from a raw checkout.
// Then is_available() is false and every open_* factory returns an error, while the API stays declared and callable.
// See docs/coding-guidelines.md for that rule.
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
/// When false, every database::open_* returns an error.
/// A runtime probe — callers never need a macro.
[[nodiscard]] bool is_available();

} // namespace babel::sqlite

/// Why a SQLite call failed, for the codes worth branching on.
///
/// The distinction that matters is between a file that is not a database, a database that is damaged, and a lock somebody else holds.
/// Those are three different situations calling for three different responses, and matching on message text to tell them apart is not a mechanism.
/// Anything this enum does not name arrives as `unknown`, with the real code in `error::native_code`.
enum class babel::sqlite::error_code : babel::u8
{
    /// A code this enum does not name; `native_code` carries it.
    unknown,
    /// The engine was not compiled in, so no call reached SQLite at all.
    backend_missing,
    /// SQLITE_NOTADB: the file is not a SQLite database, or is encrypted.
    not_a_database,
    /// SQLITE_CORRUPT: the database image is malformed.
    corrupt,
    /// SQLITE_BUSY / SQLITE_LOCKED: another connection holds the lock.
    busy,
    /// SQLITE_CANTOPEN: the file could not be opened.
    cannot_open,
    /// SQLITE_READONLY: a write against a read-only database.
    read_only,
    /// SQLITE_IOERR: the underlying filesystem failed.
    io_error,
    /// SQLITE_CONSTRAINT: a UNIQUE, NOT NULL, foreign key or CHECK constraint rejected the write.
    constraint,
    /// SQLITE_FULL / SQLITE_TOOBIG: out of space, or a value beyond SQLite's per-value ceiling.
    full,
    /// SQLITE_ERROR / SQLITE_MISUSE / SQLITE_RANGE: a bug in the SQL or in the calling code.
    misuse,
};

/// What a failed SQLite call reports.
///
/// Copyable on purpose, unlike cc::any_error, so a caller can latch the first failure and still read it afterwards.
/// It converts into cc::result<T, cc::any_error> implicitly, so a caller that does not care about the code loses nothing.
struct babel::sqlite::error
{
    error_code code = error_code::unknown;
    /// SQLite's own result code, extended bits included; 0 when no call reached the engine.
    i32 native_code = 0;
    cc::string message;

    /// The ADL hook cc::to_debug_string finds, which is what makes the erasure into cc::any_error carry the message.
    [[nodiscard]] friend cc::string to_string(error const& e) { return e.message; }
};

/// How the connection journals, set with database::set_journal_mode.
///
/// The first four differ only in what happens to the rollback journal between transactions, so they trade
/// per-transaction filesystem cost against nothing else.
/// `wal` is a different mechanism altogether, and `off` gives up crash safety.
enum class babel::sqlite::journal_mode : babel::u8
{
    /// SQLite's default: a rollback journal beside the database file, deleted at the end of every transaction.
    /// Committing therefore costs one file deletion.
    delete_journal, // SQLite spells this DELETE, which is a keyword here

    /// The same journal, truncated to zero length instead of deleted.
    /// Faster wherever truncating a file is cheaper than deleting and recreating it, which is most filesystems.
    truncate,

    /// The same journal again, left in place with its header zeroed.
    /// Avoids per-transaction file creation entirely, at the cost of a journal file that stays on disk.
    persist,

    /// The rollback journal lives in RAM.
    /// Fastest of the four, and a crash or power loss mid-transaction leaves the database corrupt — there is
    /// nothing on disk to roll back from.
    memory,

    /// A write-ahead log: writers append pages to a `-wal` file instead of rewriting the database in place.
    /// One writer and any number of readers proceed without blocking each other, which is what a file being
    /// written incrementally while it is read wants.
    ///
    /// Unlike every other mode this is a property of the database FILE rather than the connection, so it survives
    /// close and reopen and is seen by every connection.
    /// Unavailable for an in-memory database, and it needs a VFS with shared-memory support — which is why
    /// get_journal_mode exists rather than the request being assumed to have taken.
    wal,

    /// No rollback journal at all.
    /// ROLLBACK stops meaning anything and a crash mid-transaction corrupts the database, so this is for data
    /// that can be regenerated and nothing else.
    off,
};

/// The dynamic type of a result column, as reported by SQLite for the current row.
enum class babel::sqlite::column_kind : babel::u8
{
    null,
    integer,
    real,
    text,
    blob,
};

/// A non-owning view of the statement's current result row.
/// Valid only until the next step, or until the statement dies.
/// Columns are 0-based, and accessors follow SQLite's type coercion (as_i64 on a text cell parses it).
struct babel::sqlite::row
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
    [[nodiscard]] cc::span<byte const> as_blob(i32 col) const;

private:
    sqlite3_stmt* _stmt = nullptr;
};

/// A prepared statement: bind parameters, then step through result rows.
/// Move-only (owns the sqlite3_stmt). Obtain one from database::prepare / database::query.
///
/// Iterate with a range-for (single pass). Row-stepping cannot return a result per iteration, so a step failure is
/// sticky: it ends the loop and is readable afterwards via is_ok() / last_error(). For explicit control use next().
class babel::sqlite::statement
{
public:
    statement() = default;
    ~statement();
    statement(statement&&) noexcept;
    statement& operator=(statement&&) noexcept;
    statement(statement const&) = delete;
    statement& operator=(statement const&) = delete;

    /// Bind a value to parameter `index` (1-based, SQLite convention). Text / blob bytes are copied by SQLite.
    cc::result<cc::unit, error> bind(i32 index, i64 value);
    cc::result<cc::unit, error> bind(i32 index, double value);
    cc::result<cc::unit, error> bind(i32 index, cc::string_view value);
    cc::result<cc::unit, error> bind(i32 index, cc::span<byte const> value);
    cc::result<cc::unit, error> bind_null(i32 index);

    /// Advance to the next result row.
    /// True means a row is now current — read it via the range-for row, or column_* / as_* on current().
    /// False means no more rows.
    /// Errors surface here and also set the sticky error.
    [[nodiscard]] cc::result<bool, error> next();

    /// The current row view (valid after next() returned true).
    [[nodiscard]] row current() const { return row(_stmt); }

    /// Reset back to before the first row so the statement can be re-executed; keeps bound parameters.
    cc::result<cc::unit, error> reset();
    /// Clear all bound parameters back to NULL.
    void clear_bindings();

    [[nodiscard]] bool is_ok() const { return _ok; }
    /// The sticky failure a range-for loop ended on, meaningful only once is_ok() is false.
    /// Named last_error rather than error so it does not hide the error type inside this class.
    [[nodiscard]] sqlite::error const& last_error() const { return _error; }

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
    sqlite::error _error;
};

/// Which BLOB cell an incremental handle is opened over.
/// The row is named by rowid, so the table must be a rowid table — a WITHOUT ROWID table cannot be reached this way.
struct babel::sqlite::blob_location
{
    cc::string_view table;
    cc::string_view column;
    i64 rowid = 0;
    cc::string_view db_name = "main";
};

/// A read handle over one BLOB cell, reading at an offset without materializing the whole value.
/// Move-only (owns the sqlite3_blob). Obtain one from database::open_blob_handle.
///
/// **A write to the row invalidates the handle**, including a write through another connection.
/// read_at then reports an error rather than stale bytes, and reopen() is how a handle is brought back to life.
///
/// Read-only for now: writing through a handle is a separate capability and is not implemented.
class babel::sqlite::blob_handle
{
public:
    blob_handle() = default;
    ~blob_handle();
    blob_handle(blob_handle&&) noexcept;
    blob_handle& operator=(blob_handle&&) noexcept;
    blob_handle(blob_handle const&) = delete;
    blob_handle& operator=(blob_handle const&) = delete;

    /// The size of the whole BLOB in bytes, whatever slice of it has been read.
    [[nodiscard]] isize size() const;

    /// Reads out.size() bytes starting at `offset`.
    /// The range must lie inside the blob: a partial read is an error, never a short fill.
    cc::result<cc::unit, error> read_at(isize offset, cc::span<byte> out);

    /// Points the same handle at another row of the same table and column.
    /// Cheaper than opening a new handle, which is what makes walking many rows worth doing this way.
    cc::result<cc::unit, error> reopen(i64 rowid);

private:
    friend class database;
    explicit blob_handle(sqlite3_blob* blob, sqlite3* db) : _blob(blob), _db(db) {}

    sqlite3_blob* _blob = nullptr;
    sqlite3* _db = nullptr; // for error text only; the connection outlives the handle
};

/// A scoped transaction: commit() publishes it, and anything else rolls it back.
/// Move-only; obtain one from database::begin_transaction.
///
/// This is what makes a multi-table write all-or-nothing — an early return, a thrown exception or a plain forgotten
/// commit all leave the database as it was, rather than half-written.
///
/// **A rollback in the destructor cannot report.** commit() is the reporting path, so a caller that needs to know
/// the write landed must call it and read the result.
/// Transactions do not nest: SQLite rejects a nested BEGIN, and that surfaces as an ordinary error here.
class babel::sqlite::transaction
{
public:
    transaction() = default;
    ~transaction();
    transaction(transaction&&) noexcept;
    transaction& operator=(transaction&&) noexcept;
    transaction(transaction const&) = delete;
    transaction& operator=(transaction const&) = delete;

    /// Commits, after which the transaction is inert and the destructor does nothing.
    /// A failed commit leaves it live, so the destructor still rolls back.
    cc::result<cc::unit, error> commit();

    /// True while the transaction is still open — neither committed nor moved from.
    [[nodiscard]] bool is_open() const { return _db != nullptr; }

private:
    friend class database;
    explicit transaction(sqlite3* db) : _db(db) {}

    sqlite3* _db = nullptr; // null once committed or moved from
};

/// A live SQLite database connection.
/// Move-only: it owns the sqlite3 handle and closes it in the destructor.
/// Full read/write: exec arbitrary SQL, prepare/query statements, run DDL and transactions.
class babel::sqlite::database
{
public:
    database() = default;
    ~database();
    database(database&&) noexcept;
    database& operator=(database&&) noexcept;
    database(database const&) = delete;
    database& operator=(database const&) = delete;

    /// Open (create if missing) an on-disk database for reading and writing.
    [[nodiscard]] static cc::result<database, error> open(cc::string_view path);
    /// Open an existing on-disk database read-only.
    /// Errors if the file does not exist.
    [[nodiscard]] static cc::result<database, error> open_readonly(cc::string_view path);
    /// A transient in-memory database (":memory:").
    [[nodiscard]] static cc::result<database, error> open_memory();
    /// Load a database from a serialized in-memory image (a copy is taken; the source bytes need not outlive this).
    [[nodiscard]] static cc::result<database, error> open_blob(cc::span<byte const> bytes);

    /// Run one or more SQL statements that yield no result rows (DDL, INSERT/UPDATE/DELETE, PRAGMA, transactions).
    cc::result<cc::unit, error> exec(cc::string_view sql);
    /// Prepare a single statement for binding + stepping.
    [[nodiscard]] cc::result<statement, error> prepare(cc::string_view sql);
    /// Convenience: prepare a single statement ready to iterate.
    /// Same as prepare; it reads as intent at the call site.
    [[nodiscard]] cc::result<statement, error> query(cc::string_view sql) { return prepare(sql); }

    /// Open a read handle over one BLOB cell, for reading it in pieces.
    /// Note the neighbour: open_blob loads a whole *database* from a byte image, this opens one *value* in a row.
    [[nodiscard]] cc::result<blob_handle, error> open_blob_handle(blob_location where);

    /// Begin a transaction that commits on request and rolls back on anything else.
    [[nodiscard]] cc::result<transaction, error> begin_transaction();

    /// How the connection journals.
    /// WAL is the mode a file written incrementally wants.
    cc::result<cc::unit, error> set_journal_mode(journal_mode mode);
    /// What the connection actually journals as — a requested mode can be refused (WAL needs a VFS that supports it),
    /// so this is read back rather than assumed.
    [[nodiscard]] cc::result<journal_mode, error> get_journal_mode();

    /// How long a write blocked by another connection waits before giving up.
    /// 0 means do not wait at all.
    cc::result<cc::unit, error> set_busy_timeout(i32 milliseconds);

    /// Whether foreign key constraints — and the ON DELETE CASCADE that rides on them — are enforced.
    /// SQLite defaults this OFF, so a schema whose correctness depends on a cascade must turn it on explicitly.
    cc::result<cc::unit, error> set_foreign_keys(bool enabled);
    [[nodiscard]] cc::result<bool, error> get_foreign_keys();

    /// The `application_id` header field: four bytes saying whose file this is, to anything inspecting it — file(1) included.
    /// SQLite itself never reads it, and a database that has never been stamped reports 0.
    [[nodiscard]] cc::result<i32, error> get_application_id();
    cc::result<cc::unit, error> set_application_id(i32 id);

    /// The `user_version` header field: a format version the application owns and SQLite only stores.
    /// Reading it is also the first real page read of a file, which is where a not-a-database or a corrupt image surfaces.
    [[nodiscard]] cc::result<i32, error> get_user_version();
    cc::result<cc::unit, error> set_user_version(i32 version);

    /// Serialize the main database to a contiguous byte image (round-trips through open_blob). Empty on failure.
    [[nodiscard]] cc::vector<byte> serialize() const;

    [[nodiscard]] i64 last_insert_rowid() const;
    [[nodiscard]] i64 changes() const;

private:
    explicit database(sqlite3* db) : _db(db) {}
    [[nodiscard]] static cc::result<database, error> open_with_flags(cc::string_view path, int flags);

    sqlite3* _db = nullptr;
};
