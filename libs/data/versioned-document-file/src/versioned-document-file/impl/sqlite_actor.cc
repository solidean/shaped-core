#include <babel-serializer/data/sqlite.hh>
#include <clean-core/string/format.hh>
#include <versioned-document-file/impl/sqlite_actor.hh>
#include <versioned-document-file/impl/sqlite_io.hh>
#include <versioned-document-file/impl/sqlite_schema.hh>
#include <versioned-document-file/impl/store_sqlite.hh>

namespace vdoc::file::impl
{
namespace
{
namespace sql = babel::sqlite;

cc::async_error as_async_error(cc::any_error e)
{
    return cc::async_error::make_error(cc::move(e));
}

/// The one thing that touches a connection.
///
/// The handle is a member HERE, so nothing outside this class can reach it — which is what makes exclusive ownership
/// structural rather than a rule somebody has to remember.
class sqlite_connection_actor final
  : public cc::threaded_actor_impl<open_request, publish_request, snapshot_write_request, reclaim_request, workspace_request, blob_request, close_request>
{
public:
    // A string literal, so it outlives the thread it names.
    cc::string_view actor_name() const noexcept override { return "vdoc-file-store"; }

protected:
    void on_message(open_request msg) override
    {
        if (!sql::is_available())
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string("SQLite support was not compiled in, so no "
                                                                            ".vdoc file can be opened"))));
            return;
        }

        auto opened = sql::database::open(msg.path);
        if (opened.has_error())
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string(opened.error().message))));
            return;
        }
        _db = cc::move(opened.value());
        _is_open = true;

        auto scan = ensure_schema(_db);
        if (scan.has_error())
        {
            _db = sql::database();
            _is_open = false;
            msg.promise->push_error(as_async_error(cc::move(scan).error()));
            return;
        }

        // The handle exists nowhere but inside this message until the promise resolves, so filling the store's plain
        // members directly is safe by construction: no second observer can exist yet.
        auto reader = sqlite_reader(_db, cc::move(scan.value()));
        auto loaded = load(reader, *msg.store);
        if (loaded.has_error())
        {
            _db = sql::database();
            _is_open = false;
            msg.promise->push_error(as_async_error(cc::move(loaded).error()));
            return;
        }

        msg.promise->push_value(cc::unit{});
    }

    void on_message(publish_request msg) override
    {
        if (!_is_open)
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string("publishing to a store whose connection "
                                                                            "is closed"))));
            return;
        }

        auto writer = sqlite_writer(_db);
        auto applied = apply_publish(writer, msg.job);
        if (applied.has_error())
            msg.promise->push_error(as_async_error(cc::move(applied).error()));
        else
            msg.promise->push_value(applied.value());
    }

    void on_message(snapshot_write_request msg) override
    {
        if (!_is_open)
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string("pruning a store whose connection "
                                                                            "is closed"))));
            return;
        }

        auto writer = sqlite_writer(_db);
        auto applied = apply_snapshot_write(writer, msg.job);
        if (applied.has_error())
            msg.promise->push_error(as_async_error(cc::move(applied).error()));
        else
            msg.promise->push_value(applied.value());
    }

    void on_message(reclaim_request msg) override
    {
        if (!_is_open)
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string("reclaiming in a store whose connection "
                                                                            "is closed"))));
            return;
        }

        auto writer = sqlite_writer(_db);
        auto applied = apply_reclaim(writer, msg.job);
        if (applied.has_error())
            msg.promise->push_error(as_async_error(cc::move(applied).error()));
        else
            msg.promise->push_value(applied.value());
    }

    void on_message(blob_request msg) override
    {
        if (!_is_open)
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string("fetching a blob from a store whose "
                                                                            "connection is closed"))));
            return;
        }

        auto reader = sqlite_blob_payload_reader(_db);
        auto fetched = fetch_blob(reader, msg.hash, msg.range);
        if (fetched.has_error())
            msg.promise->push_error(as_async_error(cc::move(fetched).error()));
        else
            msg.promise->push_value(cc::move(fetched.value()));
    }

    void on_message(workspace_request msg) override
    {
        if (!_is_open)
        {
            msg.promise->push_error(as_async_error(cc::any_error(cc::string("flushing the workspace of a store whose "
                                                                            "connection is closed"))));
            return;
        }

        auto writer = sqlite_writer(_db);
        auto applied = apply_workspace(writer, msg.entries);
        if (applied.has_error())
            msg.promise->push_error(as_async_error(cc::move(applied).error()));
        else
            msg.promise->push_value(cc::unit{});
    }

    void on_message(close_request) override
    {
        _db = sql::database();
        _is_open = false;
    }

private:
    sql::database _db;
    bool _is_open = false;
};
} // namespace

cc::unique_ptr<sqlite_actor> make_sqlite_actor()
{
    // threaded_if_possible, so a build with no threads runs every handler on the calling thread instead — which is
    // exactly what impl_pump_until_idle then drives, with no declaration compiled away anywhere.
    return cc::make_and_start_threaded_actor<sqlite_connection_actor>();
}
} // namespace vdoc::file::impl
