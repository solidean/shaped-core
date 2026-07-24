#include <babel-serializer/data/sqlite.hh>

// SQLite backend, conditionally compiled.
//
// The public API in sqlite.hh is always present; only its implementation is gated. BABEL_HAS_SQLITE is a PRIVATE
// define set by babel-serializer's CMakeLists — it is 1 when the fetched extern/sqlite target was linked, 0 otherwise.
// When it is 0 we compile a stub: every entry point reports the backend as unavailable at runtime. The switch never
// leaves this file (see docs/coding-guidelines.md).

#ifndef BABEL_HAS_SQLITE
#define BABEL_HAS_SQLITE 0
#endif

#if BABEL_HAS_SQLITE

#include <clean-core/string/format.hh>
#include <sqlite3.h>

#include <cstring> // std::memcpy

namespace babel::sqlite
{
namespace
{
/// Build a "sqlite error (<code>): <message>" string from a connection's last error.
cc::string last_error(sqlite3* db)
{
    return cc::format("sqlite error ({}): {}", sqlite3_errcode(db), sqlite3_errmsg(db));
}

/// The database handle a statement was prepared against — used to read its last error.
sqlite3* handle_of(sqlite3_stmt* stmt)
{
    return sqlite3_db_handle(stmt);
}
} // namespace

// database
// -------------------------------------------------------------------------------------------------

cc::result<database> database::open_with_flags(cc::string_view path, int flags)
{
    auto c_path = cc::string::create_copy_of(path);
    sqlite3* db = nullptr;
    int const rc = sqlite3_open_v2(c_path.c_str_materialize(), &db, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        auto msg = db != nullptr ? last_error(db) : cc::format("sqlite error ({}): could not open database", rc);
        sqlite3_close_v2(db); // sqlite3_open_v2 may hand back a handle even on failure
        return cc::error(cc::move(msg));
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

cc::result<database> database::open(cc::string_view path)
{
    return open_with_flags(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
}
cc::result<database> database::open_readonly(cc::string_view path)
{
    return open_with_flags(path, SQLITE_OPEN_READONLY);
}
cc::result<database> database::open_memory()
{
    return open_with_flags(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
}

cc::result<database> database::open_blob(cc::span<cc::byte const> bytes)
{
    auto opened = open_memory();
    if (opened.has_error())
        return opened;
    auto db = cc::move(opened.value());

    // SQLite takes ownership of the image buffer (FREEONCLOSE), so it must come from sqlite3_malloc, not ours.
    auto const n = bytes.size();
    auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(sqlite3_uint64(n > 0 ? n : 1)));
    if (buffer == nullptr)
        return cc::error(cc::string("sqlite error: out of memory allocating the deserialize buffer"));
    if (n > 0)
        std::memcpy(buffer, bytes.data(), size_t(n));

    int const rc = sqlite3_deserialize(db._db, "main", buffer, sqlite3_int64(n), sqlite3_int64(n),
                                       SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
    if (rc != SQLITE_OK)
        return cc::error(last_error(db._db)); // FREEONCLOSE means SQLite already freed the buffer on failure
    return db;
}

cc::result<cc::unit> database::exec(cc::string_view sql)
{
    auto c_sql = cc::string::create_copy_of(sql);
    char* errmsg = nullptr;
    int const rc = sqlite3_exec(_db, c_sql.c_str_materialize(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        auto msg = errmsg != nullptr ? cc::format("sqlite error ({}): {}", rc, errmsg) : last_error(_db);
        sqlite3_free(errmsg);
        return cc::error(cc::move(msg));
    }
    return cc::unit{};
}

cc::result<statement> database::prepare(cc::string_view sql)
{
    sqlite3_stmt* stmt = nullptr;
    int const rc = sqlite3_prepare_v2(_db, sql.data(), int(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        auto msg = last_error(_db);
        sqlite3_finalize(stmt); // typically null on error, but finalize is null-safe
        return cc::error(cc::move(msg));
    }
    if (stmt == nullptr) // empty / whitespace / comment-only SQL compiles to no statement
        return cc::error(cc::string("sqlite error: SQL contained no statement to prepare"));
    return statement(stmt);
}

cc::vector<cc::byte> database::serialize() const
{
    sqlite3_int64 size = 0;
    auto* data = sqlite3_serialize(_db, "main", &size, 0);
    if (data == nullptr)
        return {};
    auto out = cc::vector<cc::byte>::create_copy_of(
        cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(data), isize(size)));
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

cc::result<cc::unit> statement::bind(i32 index, i64 value)
{
    if (sqlite3_bind_int64(_stmt, index, value) != SQLITE_OK)
        return cc::error(last_error(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit> statement::bind(i32 index, double value)
{
    if (sqlite3_bind_double(_stmt, index, value) != SQLITE_OK)
        return cc::error(last_error(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit> statement::bind(i32 index, cc::string_view value)
{
    if (sqlite3_bind_text(_stmt, index, value.data(), int(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        return cc::error(last_error(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit> statement::bind(i32 index, cc::span<cc::byte const> value)
{
    if (sqlite3_bind_blob(_stmt, index, value.data(), int(value.size_bytes()), SQLITE_TRANSIENT) != SQLITE_OK)
        return cc::error(last_error(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<cc::unit> statement::bind_null(i32 index)
{
    if (sqlite3_bind_null(_stmt, index) != SQLITE_OK)
        return cc::error(last_error(handle_of(_stmt)));
    return cc::unit{};
}

cc::result<bool> statement::next()
{
    int const rc = sqlite3_step(_stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    return cc::error(last_error(handle_of(_stmt)));
}

cc::result<cc::unit> statement::reset()
{
    _at_end = false;
    _ok = true;
    _error = {};
    if (sqlite3_reset(_stmt) != SQLITE_OK)
        return cc::error(last_error(handle_of(_stmt)));
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
        _error = last_error(handle_of(_stmt));
    }
}

statement::iterator statement::begin()
{
    _at_end = false;
    _advance();
    return iterator{this};
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

cc::span<cc::byte const> row::as_blob(i32 col) const
{
    void const* data = sqlite3_column_blob(_stmt, col);
    int const n = sqlite3_column_bytes(_stmt, col);
    if (data == nullptr)
        return {};
    return cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(data), n);
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
// The one message every entry point reports. IO is error-riddled anyway, so an absent backend is just one more
// ordinary runtime failure, not a hole in the API surface.
cc::string unavailable()
{
    return cc::string("SQLite support was not compiled in (the extern/sqlite backend was not fetched; see "
                      "SC_SKIP_SQLITE)");
}
} // namespace

// database — no handle is ever held; every operation reports the backend as unavailable.
database::~database() = default;
database::database(database&&) noexcept = default;
database& database::operator=(database&&) noexcept = default;

cc::result<database> database::open(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<database> database::open_readonly(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<database> database::open_memory()
{
    return cc::error(unavailable());
}
cc::result<database> database::open_blob(cc::span<cc::byte const>)
{
    return cc::error(unavailable());
}

cc::result<cc::unit> database::exec(cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<statement> database::prepare(cc::string_view)
{
    return cc::error(unavailable());
}
cc::vector<cc::byte> database::serialize() const
{
    return {};
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

cc::result<cc::unit> statement::bind(i32, i64)
{
    return cc::error(unavailable());
}
cc::result<cc::unit> statement::bind(i32, double)
{
    return cc::error(unavailable());
}
cc::result<cc::unit> statement::bind(i32, cc::string_view)
{
    return cc::error(unavailable());
}
cc::result<cc::unit> statement::bind(i32, cc::span<cc::byte const>)
{
    return cc::error(unavailable());
}
cc::result<cc::unit> statement::bind_null(i32)
{
    return cc::error(unavailable());
}

cc::result<bool> statement::next()
{
    return cc::error(unavailable());
}
cc::result<cc::unit> statement::reset()
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
cc::span<cc::byte const> row::as_blob(i32) const
{
    return {};
}

bool is_available()
{
    return false;
}
} // namespace babel::sqlite

#endif // BABEL_HAS_SQLITE
