#include <babel-serializer/data/sqlite.hh>

// SQLite backend, conditionally compiled.
//
// BABEL_HAS_SQLITE is a PRIVATE define set by babel-serializer's CMakeLists: 1 when the fetched extern/sqlite
// target was linked, 0 otherwise.
// At 0 this file compiles a complete stub whose entry points report the backend unavailable at runtime.
// The switch never leaves this file (see docs/coding-guidelines.md).

#ifndef BABEL_HAS_SQLITE
#define BABEL_HAS_SQLITE 0
#endif

#if BABEL_HAS_SQLITE

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <sqlite3.h>


namespace babel::sqlite
{
namespace
{
/// Classify a SQLite result code into the handful of situations a caller responds to differently.
/// The primary code is what is switched on: the extended bits refine a reason without changing the response.
error_code code_of(int rc)
{
    switch (rc & 0xFF)
    {
    case SQLITE_NOTADB:
        return error_code::not_a_database;
    case SQLITE_CORRUPT:
        return error_code::corrupt;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return error_code::busy;
    case SQLITE_CANTOPEN:
        return error_code::cannot_open;
    case SQLITE_READONLY:
        return error_code::read_only;
    case SQLITE_IOERR:
        return error_code::io_error;
    case SQLITE_CONSTRAINT:
        return error_code::constraint;
    case SQLITE_FULL:
    case SQLITE_TOOBIG:
        return error_code::full;
    case SQLITE_ERROR:
    case SQLITE_MISUSE:
    case SQLITE_RANGE:
        return error_code::misuse;
    default:
        return error_code::unknown;
    }
}

/// Build an error from a connection's last failure.
error error_of(sqlite3* db)
{
    int const rc = sqlite3_errcode(db);
    return {.code = code_of(rc),
            .native_code = i32(sqlite3_extended_errcode(db)),
            .message = cc::format("sqlite error ({}): {}", rc, sqlite3_errmsg(db))};
}

/// Build an error from a result code the connection cannot be asked about — an open that handed back no handle, or an exec with its own message.
error error_from(int rc, cc::string message)
{
    return {.code = code_of(rc), .native_code = i32(rc), .message = cc::move(message)};
}

/// An error our own checks raise, which SQLite was never asked about.
error misuse_error(cc::string message)
{
    return {.code = error_code::misuse, .native_code = 0, .message = cc::move(message)};
}

/// The database handle a statement was prepared against — used to read its last error.
sqlite3* handle_of(sqlite3_stmt* stmt)
{
    return sqlite3_db_handle(stmt);
}

/// The SQL spelling of a journal mode, which is also what PRAGMA journal_mode reports back.
char const* sql_name_of(journal_mode mode)
{
    switch (mode)
    {
    case journal_mode::delete_journal:
        return "delete";
    case journal_mode::truncate:
        return "truncate";
    case journal_mode::persist:
        return "persist";
    case journal_mode::memory:
        return "memory";
    case journal_mode::wal:
        return "wal";
    case journal_mode::off:
        return "off";
    }
    return "delete";
}

/// Runs a PRAGMA that reports one integer, e.g. `PRAGMA foreign_keys`.
cc::result<i64, error> pragma_i64(sqlite3* db, cc::string_view sql)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.data(), int(sql.size()), &stmt, nullptr) != SQLITE_OK)
    {
        auto msg = error_of(db);
        sqlite3_finalize(stmt);
        return cc::error(cc::move(msg));
    }

    i64 value = 0;
    int const rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return cc::error(error_of(db));
    return value;
}

/// Runs a PRAGMA that reports one text value, copied out before the statement dies.
cc::result<cc::string, error> pragma_text(sqlite3* db, cc::string_view sql)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.data(), int(sql.size()), &stmt, nullptr) != SQLITE_OK)
    {
        auto msg = error_of(db);
        sqlite3_finalize(stmt);
        return cc::error(cc::move(msg));
    }

    auto value = cc::string();
    int const rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        auto const* text = sqlite3_column_text(stmt, 0);
        if (text != nullptr)
            value = cc::string::create_copy_of(
                cc::string_view(reinterpret_cast<char const*>(text), sqlite3_column_bytes(stmt, 0)));
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return cc::error(error_of(db));
    return value;
}
} // namespace

// database
// -------------------------------------------------------------------------------------------------

cc::result<database, error> database::open_with_flags(cc::string_view path, int flags)
{
    auto c_path = cc::string::create_copy_of(path);
    sqlite3* db = nullptr;
    int const rc = sqlite3_open_v2(c_path.c_str_materialize(), &db, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        auto e = db != nullptr ? error_of(db)
                               : error_from(rc, cc::format("sqlite error ({}): could not open database", rc));
        sqlite3_close_v2(db); // sqlite3_open_v2 may hand back a handle even on failure
        return cc::error(cc::move(e));
    }
    return database(db);
}

database::~database()
{
    if (_db != nullptr)
        sqlite3_close_v2(_db);
}

database::database(database&& other) noexcept : _db(other._db)
{
    other._db = nullptr;
}

database& database::operator=(database&& other) noexcept
{
    if (this != &other)
    {
        if (_db != nullptr)
            sqlite3_close_v2(_db);
        _db = other._db;
        other._db = nullptr;
    }
    return *this;
}

cc::result<database, error> database::open(cc::string_view path)
{
    return open_with_flags(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
}
cc::result<database, error> database::open_readonly(cc::string_view path)
{
    return open_with_flags(path, SQLITE_OPEN_READONLY);
}
cc::result<database, error> database::open_memory()
{
    return open_with_flags(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
}

cc::result<database, error> database::open_blob(cc::span<byte const> bytes)
{
    auto opened = open_memory();
    if (opened.has_error())
        return opened;
    auto db = cc::move(opened.value());

    // SQLite takes ownership of the image buffer (FREEONCLOSE), so it must come from sqlite3_malloc, not ours.
    auto const n = bytes.size();
    auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(sqlite3_uint64(n > 0 ? n : 1)));
    if (buffer == nullptr)
        return cc::error(error_from(SQLITE_NOMEM, cc::string("sqlite error: out of memory allocating the deserialize "
                                                             "buffer")));
    if (n > 0)
        cc::memcpy(buffer, bytes.data(), size_t(n));

    int const rc = sqlite3_deserialize(db._db, "main", buffer, sqlite3_int64(n), sqlite3_int64(n),
                                       SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
    if (rc != SQLITE_OK)
        return cc::error(error_of(db._db)); // FREEONCLOSE means SQLite already freed the buffer on failure
    return db;
}

cc::result<cc::unit, error> database::exec(cc::string_view sql)
{
    auto c_sql = cc::string::create_copy_of(sql);
    char* errmsg = nullptr;
    int const rc = sqlite3_exec(_db, c_sql.c_str_materialize(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        auto e = errmsg != nullptr ? error_from(rc, cc::format("sqlite error ({}): {}", rc, errmsg)) : error_of(_db);
        sqlite3_free(errmsg);
        return cc::error(cc::move(e));
    }
    return cc::unit{};
}

cc::result<statement, error> database::prepare(cc::string_view sql)
{
    sqlite3_stmt* stmt = nullptr;
    int const rc = sqlite3_prepare_v2(_db, sql.data(), int(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        auto msg = error_of(_db);
        sqlite3_finalize(stmt); // typically null on error, but finalize is null-safe
        return cc::error(cc::move(msg));
    }
    if (stmt == nullptr) // empty / whitespace / comment-only SQL compiles to no statement
        return cc::error(misuse_error(cc::string("sqlite error: SQL contained no statement to prepare")));
    return statement(stmt);
}

cc::result<blob_handle, error> database::open_blob_handle(blob_location where)
{
    auto c_db = cc::string::create_copy_of(where.db_name);
    auto c_table = cc::string::create_copy_of(where.table);
    auto c_column = cc::string::create_copy_of(where.column);

    sqlite3_blob* blob = nullptr;
    int const rc = sqlite3_blob_open(_db, c_db.c_str_materialize(), c_table.c_str_materialize(),
                                     c_column.c_str_materialize(), where.rowid, /*writable*/ 0, &blob);
    if (rc != SQLITE_OK)
    {
        auto msg = error_of(_db);
        sqlite3_blob_close(blob); // null on failure, but close is null-safe
        return cc::error(cc::move(msg));
    }
    return blob_handle(blob, _db);
}

cc::result<transaction, error> database::begin_transaction()
{
    CC_RETURN_IF_ERROR(exec("BEGIN"));
    return transaction(_db);
}

cc::result<cc::unit, error> database::set_journal_mode(journal_mode mode)
{
    return exec(cc::format("PRAGMA journal_mode = {}", sql_name_of(mode)));
}

cc::result<journal_mode, error> database::get_journal_mode()
{
    auto reported = pragma_text(_db, "PRAGMA journal_mode");
    CC_RETURN_IF_ERROR(reported);

    for (auto const mode : {journal_mode::delete_journal, journal_mode::truncate, journal_mode::persist,
                            journal_mode::memory, journal_mode::wal, journal_mode::off})
        if (reported.value() == cc::string_view(sql_name_of(mode)))
            return mode;

    return cc::error(misuse_error(cc::format("sqlite: unknown journal mode '{}'", reported.value())));
}

cc::result<cc::unit, error> database::set_busy_timeout(i32 milliseconds)
{
    if (sqlite3_busy_timeout(_db, milliseconds) != SQLITE_OK)
        return cc::error(error_of(_db));
    return cc::unit{};
}

cc::result<cc::unit, error> database::set_foreign_keys(bool enabled)
{
    return exec(enabled ? "PRAGMA foreign_keys = ON" : "PRAGMA foreign_keys = OFF");
}

cc::result<bool, error> database::get_foreign_keys()
{
    auto reported = pragma_i64(_db, "PRAGMA foreign_keys");
    CC_RETURN_IF_ERROR(reported);
    return reported.value() != 0;
}

cc::result<i32, error> database::get_application_id()
{
    auto reported = pragma_i64(_db, "PRAGMA application_id");
    CC_RETURN_IF_ERROR(reported);
    return i32(reported.value());
}

cc::result<cc::unit, error> database::set_application_id(i32 id)
{
    // A PRAGMA value cannot be bound, and this one is an integer we produce rather than caller text.
    return exec(cc::format("PRAGMA application_id = {}", id));
}

cc::result<i32, error> database::get_user_version()
{
    auto reported = pragma_i64(_db, "PRAGMA user_version");
    CC_RETURN_IF_ERROR(reported);
    return i32(reported.value());
}

cc::result<cc::unit, error> database::set_user_version(i32 version)
{
    return exec(cc::format("PRAGMA user_version = {}", version));
}

cc::vector<byte> database::serialize() const
{
    sqlite3_int64 size = 0;
    auto* data = sqlite3_serialize(_db, "main", &size, 0);
    if (data == nullptr)
        return {};
    auto out = cc::vector<byte>::create_copy_of(cc::span<byte const>(reinterpret_cast<byte const*>(data), isize(size)));
    sqlite3_free(data);
    return out;
}

i64 database::last_insert_rowid() const
{
    return sqlite3_last_insert_rowid(_db);
}
i64 database::changes() const
{
    return sqlite3_changes64(_db);
}

// statement
// -------------------------------------------------------------------------------------------------

statement::~statement()
{
    if (_stmt != nullptr)
        sqlite3_finalize(_stmt);
}

statement::statement(statement&& other) noexcept
  : _stmt(other._stmt), _at_end(other._at_end), _ok(other._ok), _error(cc::move(other._error))
{
    other._stmt = nullptr;
}

statement& statement::operator=(statement&& other) noexcept
{
    if (this != &other)
    {
        if (_stmt != nullptr)
            sqlite3_finalize(_stmt);
        _stmt = other._stmt;
        _at_end = other._at_end;
        _ok = other._ok;
        _error = cc::move(other._error);
        other._stmt = nullptr;
    }
    return *this;
}

cc::result<cc::unit, error> statement::bind(i32 index, i64 value)
{
    if (sqlite3_bind_int64(_stmt, index, value) != SQLITE_OK)
        return cc::error(error_of(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit, error> statement::bind(i32 index, double value)
{
    if (sqlite3_bind_double(_stmt, index, value) != SQLITE_OK)
        return cc::error(error_of(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit, error> statement::bind(i32 index, cc::string_view value)
{
    if (sqlite3_bind_text(_stmt, index, value.data(), int(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        return cc::error(error_of(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit, error> statement::bind(i32 index, cc::span<byte const> value)
{
    // An EMPTY span still binds an empty blob, never NULL.
    // sqlite3_bind_blob reads a null pointer as "bind NULL", and an empty container's data() is legitimately null — so
    // without this, binding an empty blob into a NOT NULL column fails for a value the caller did supply.
    static constexpr byte empty = {};
    auto const* data = value.data() != nullptr ? value.data() : &empty;

    if (sqlite3_bind_blob(_stmt, index, data, int(value.size_bytes()), SQLITE_TRANSIENT) != SQLITE_OK)
        return cc::error(error_of(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit, error> statement::bind_null(i32 index)
{
    if (sqlite3_bind_null(_stmt, index) != SQLITE_OK)
        return cc::error(error_of(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<bool, error> statement::next()
{
    int const rc = sqlite3_step(_stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    return cc::error(error_of(handle_of(_stmt)));
}

cc::result<cc::unit, error> statement::reset()
{
    _at_end = false;
    _ok = true;
    _error = {};
    if (sqlite3_reset(_stmt) != SQLITE_OK)
        return cc::error(error_of(handle_of(_stmt)));
    return cc::unit{};
}

void statement::clear_bindings()
{
    sqlite3_clear_bindings(_stmt);
}

void statement::_advance()
{
    int const rc = sqlite3_step(_stmt);
    if (rc == SQLITE_ROW)
    {
        _at_end = false;
        return;
    }
    _at_end = true;
    if (rc != SQLITE_DONE)
    {
        _ok = false;
        _error = error_of(handle_of(_stmt));
    }
}

statement::iterator statement::begin()
{
    _at_end = false;
    _advance();
    return iterator{this};
}

// blob_handle
// -------------------------------------------------------------------------------------------------

blob_handle::~blob_handle()
{
    if (_blob != nullptr)
        sqlite3_blob_close(_blob);
}

blob_handle::blob_handle(blob_handle&& other) noexcept : _blob(other._blob), _db(other._db)
{
    other._blob = nullptr;
    other._db = nullptr;
}

blob_handle& blob_handle::operator=(blob_handle&& other) noexcept
{
    if (this != &other)
    {
        if (_blob != nullptr)
            sqlite3_blob_close(_blob);
        _blob = other._blob;
        _db = other._db;
        other._blob = nullptr;
        other._db = nullptr;
    }
    return *this;
}

isize blob_handle::size() const
{
    return _blob != nullptr ? isize(sqlite3_blob_bytes(_blob)) : 0;
}

cc::result<cc::unit, error> blob_handle::read_at(isize offset, cc::span<byte> out)
{
    if (_blob == nullptr)
        return cc::error(misuse_error(cc::string("sqlite error: reading from a closed blob handle")));

    // Checked here rather than left to SQLite, so the message names the range instead of reporting SQLITE_ERROR.
    auto const total = size();
    if (offset < 0 || out.size() < 0 || offset + out.size() > total)
        return cc::error(misuse_error(cc::format("sqlite error: blob read [{}, {}) is outside the {}-byte value",
                                                 offset, offset + out.size(), total)));

    if (out.empty())
        return cc::unit{};

    if (sqlite3_blob_read(_blob, out.data(), int(out.size()), int(offset)) != SQLITE_OK)
        return cc::error(error_of(_db));
    return cc::unit{};
}

cc::result<cc::unit, error> blob_handle::reopen(i64 rowid)
{
    if (_blob == nullptr)
        return cc::error(misuse_error(cc::string("sqlite error: reopening a closed blob handle")));

    if (sqlite3_blob_reopen(_blob, rowid) != SQLITE_OK)
        return cc::error(error_of(_db));
    return cc::unit{};
}

// transaction
// -------------------------------------------------------------------------------------------------

transaction::~transaction()
{
    // Nothing to report to: a caller that needs to know the write landed calls commit() and reads its result.
    if (_db != nullptr)
        sqlite3_exec(_db, "ROLLBACK", nullptr, nullptr, nullptr);
}

transaction::transaction(transaction&& other) noexcept : _db(other._db)
{
    other._db = nullptr;
}

transaction& transaction::operator=(transaction&& other) noexcept
{
    if (this != &other)
    {
        if (_db != nullptr)
            sqlite3_exec(_db, "ROLLBACK", nullptr, nullptr, nullptr);
        _db = other._db;
        other._db = nullptr;
    }
    return *this;
}

cc::result<cc::unit, error> transaction::commit()
{
    if (_db == nullptr)
        return cc::error(misuse_error(cc::string("sqlite error: committing a transaction that is already finished")));

    char* errmsg = nullptr;
    int const rc = sqlite3_exec(_db, "COMMIT", nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        // Still live, so the destructor rolls back — a failed commit must not read as a silent success.
        auto e = errmsg != nullptr ? error_from(rc, cc::format("sqlite error ({}): {}", rc, errmsg)) : error_of(_db);
        sqlite3_free(errmsg);
        return cc::error(cc::move(e));
    }

    _db = nullptr;
    return cc::unit{};
}

// row
// -------------------------------------------------------------------------------------------------

i32 row::column_count() const
{
    return sqlite3_column_count(_stmt);
}

cc::string_view row::column_name(i32 col) const
{
    char const* name = sqlite3_column_name(_stmt, col);
    return name != nullptr ? cc::string_view(name) : cc::string_view();
}

column_kind row::column_type(i32 col) const
{
    switch (sqlite3_column_type(_stmt, col))
    {
    case SQLITE_INTEGER:
        return column_kind::integer;
    case SQLITE_FLOAT:
        return column_kind::real;
    case SQLITE_TEXT:
        return column_kind::text;
    case SQLITE_BLOB:
        return column_kind::blob;
    default:
        return column_kind::null;
    }
}

bool row::is_null(i32 col) const
{
    return sqlite3_column_type(_stmt, col) == SQLITE_NULL;
}

i64 row::as_i64(i32 col) const
{
    return sqlite3_column_int64(_stmt, col);
}
double row::as_double(i32 col) const
{
    return sqlite3_column_double(_stmt, col);
}

cc::string_view row::as_string(i32 col) const
{
    auto const* text = sqlite3_column_text(_stmt, col);
    int const n = sqlite3_column_bytes(_stmt, col);
    return text != nullptr ? cc::string_view(reinterpret_cast<char const*>(text), n) : cc::string_view();
}

cc::span<byte const> row::as_blob(i32 col) const
{
    void const* data = sqlite3_column_blob(_stmt, col);
    int const n = sqlite3_column_bytes(_stmt, col);
    if (data == nullptr)
        return {};
    return cc::span<byte const>(reinterpret_cast<byte const*>(data), n);
}

bool is_available()
{
    return true;
}
} // namespace babel::sqlite

#else // BABEL_HAS_SQLITE — stub: the backend was not fetched/compiled in

namespace babel::sqlite
{
namespace
{
// The one error every entry point reports when the backend is absent.
// backend_missing rather than a code, because no call reached SQLite for one to come back from.
error unavailable()
{
    return {.code = error_code::backend_missing,
            .native_code = 0,
            .message = cc::string("SQLite support was not compiled in (the extern/sqlite backend was not fetched; see "
                                  "SC_SKIP_SQLITE)")};
}
} // namespace

// database — no handle is ever held; every operation reports the backend as unavailable.
database::~database() = default;
database::database(database&&) noexcept = default;
database& database::operator=(database&&) noexcept = default;

cc::result<database, error> database::open(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<database, error> database::open_readonly(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<database, error> database::open_memory()
{
    return cc::error(unavailable());
}
cc::result<database, error> database::open_blob(cc::span<byte const>)
{
    return cc::error(unavailable());
}

cc::result<cc::unit, error> database::exec(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<statement, error> database::prepare(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<blob_handle, error> database::open_blob_handle(blob_location)
{
    return cc::error(unavailable());
}
cc::result<transaction, error> database::begin_transaction()
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> database::set_journal_mode(journal_mode)
{
    return cc::error(unavailable());
}
cc::result<journal_mode, error> database::get_journal_mode()
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> database::set_busy_timeout(i32)
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> database::set_foreign_keys(bool)
{
    return cc::error(unavailable());
}
cc::result<bool, error> database::get_foreign_keys()
{
    return cc::error(unavailable());
}
cc::result<i32, error> database::get_application_id()
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> database::set_application_id(i32)
{
    return cc::error(unavailable());
}
cc::result<i32, error> database::get_user_version()
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> database::set_user_version(i32)
{
    return cc::error(unavailable());
}

cc::vector<byte> database::serialize() const
{
    return {};
}

// blob_handle / transaction — never constructed in this build (no database hands one out); definitions exist so the API links.
blob_handle::~blob_handle() = default;
blob_handle::blob_handle(blob_handle&&) noexcept = default;
blob_handle& blob_handle::operator=(blob_handle&&) noexcept = default;

isize blob_handle::size() const
{
    return 0;
}
cc::result<cc::unit, error> blob_handle::read_at(isize, cc::span<byte>)
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> blob_handle::reopen(i64)
{
    return cc::error(unavailable());
}

transaction::~transaction() = default;
transaction::transaction(transaction&&) noexcept = default;
transaction& transaction::operator=(transaction&&) noexcept = default;

cc::result<cc::unit, error> transaction::commit()
{
    return cc::error(unavailable());
}
i64 database::last_insert_rowid() const
{
    return 0;
}
i64 database::changes() const
{
    return 0;
}

// statement — never constructed in this build (no database hands one out); definitions exist so the API links.
statement::~statement() = default;
statement::statement(statement&&) noexcept = default;
statement& statement::operator=(statement&&) noexcept = default;

cc::result<cc::unit, error> statement::bind(i32, i64)
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> statement::bind(i32, double)
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> statement::bind(i32, cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> statement::bind(i32, cc::span<byte const>)
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> statement::bind_null(i32)
{
    return cc::error(unavailable());
}

cc::result<bool, error> statement::next()
{
    return cc::error(unavailable());
}
cc::result<cc::unit, error> statement::reset()
{
    return cc::error(unavailable());
}
void statement::clear_bindings()
{
}
void statement::_advance()
{
    _at_end = true;
}

statement::iterator statement::begin()
{
    _at_end = true; // an empty range: the loop body never runs
    return iterator{this};
}

// row — never handed out without a backend; safe defaults keep the accessors defined.
i32 row::column_count() const
{
    return 0;
}
cc::string_view row::column_name(i32) const
{
    return {};
}
column_kind row::column_type(i32) const
{
    return column_kind::null;
}
bool row::is_null(i32) const
{
    return true;
}
i64 row::as_i64(i32) const
{
    return 0;
}
double row::as_double(i32) const
{
    return 0;
}
cc::string_view row::as_string(i32) const
{
    return {};
}
cc::span<byte const> row::as_blob(i32) const
{
    return {};
}

bool is_available()
{
    return false;
}
} // namespace babel::sqlite

#endif // BABEL_HAS_SQLITE
