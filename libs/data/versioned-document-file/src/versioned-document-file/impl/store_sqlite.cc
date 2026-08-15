#include <babel-serializer/data/sqlite.hh>
#include <versioned-document-file/impl/store_sqlite.hh>

namespace vdoc::file::impl
{
namespace
{
cc::async_error rejected(cc::string_view what)
{
    return cc::async_error::make_error(cc::any_error(cc::format("the store is shutting down, so {} was rejected", what)));
}
} // namespace

cc::shared_async<publish_result> sqlite_store::on_publish(publish_job job)
{
    auto promise = cc::make_async_manual<publish_result>();
    if (!_actor->enqueue_message(publish_request{.job = cc::move(job), .promise = promise}))
        promise->push_error(rejected("a publish"));
    return promise;
}

cc::shared_async<cc::unit> sqlite_store::on_flush_workspace(cc::vector<workspace_entry> entries)
{
    auto promise = cc::make_async_manual<cc::unit>();
    if (!_actor->enqueue_message(workspace_request{.entries = cc::move(entries), .promise = promise}))
        promise->push_error(rejected("a workspace flush"));
    return promise;
}

void sqlite_store::on_close()
{
    // shutdown() drains what was already accepted and joins, so the close message is the last thing the actor sees.
    (void)_actor->enqueue_message(close_request{});
    _actor->shutdown();
}

bool sqlite_store::on_pump()
{
    // A no-op returning false in a threaded build, and the whole engine in an unthreaded one.
    // Calling it unconditionally is how CC_HAS_THREADS is honoured without a single #if in this library.
    return _actor->process_messages_if_unthreaded();
}
} // namespace vdoc::file::impl

namespace vdoc::file
{
bool store::is_file_storage_available()
{
    // A runtime probe, never a macro: the API stays declared and callable either way, and open() reports the absence.
    return babel::sqlite::is_available();
}

open_result store::open(cc::string_view path)
{
    // Constructed and started here, but NOTHING touches the disk on this thread: the open is a message like any other.
    auto handle = std::make_shared<impl::sqlite_store>();
    auto promise = cc::make_async_manual<cc::unit>();

    // The actor gets a BORROWED pointer, never ownership.
    // The caller owns the handle from here on, so a failed open destroys the store on the caller's thread — never on
    // the actor's, where tearing the actor down would be a join against itself.
    if (!handle->actor().enqueue_message(
            impl::open_request{.path = cc::string(path), .store = handle.get(), .promise = promise}))
    {
        promise->push_error(cc::async_error::make_error(cc::any_error(cc::string("the store actor would not accept an "
                                                                                 "open"))));
        return {.store = cc::move(handle), .loaded = promise};
    }

    // Unthreaded, this runs the load right here, so the async is already resolved on return.
    // Threaded, it returns at once and the actor resolves the promise.
    handle->impl_pump_until_idle();
    return {.store = cc::move(handle), .loaded = promise};
}
} // namespace vdoc::file
