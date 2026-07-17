#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <shaped-shader-library/filesystem/impl/watch_backend.hh>

#if !CC_HAS_THREADS

// A backend has to wait on the OS somewhere, and there is no thread to wait on. Reporting "cannot notify"
// puts the reload watcher back on polling, which is the one thing that still works here.
std::unique_ptr<slib::impl::watch_backend> slib::impl::create_watch_backend()
{
    return nullptr;
}

#else

#include <clean-core/container/map.hh>
#include <clean-core/platform/win32_sanitized.hh> // the sanctioned way to reach <Windows.h> in shaped-core
#include <clean-core/string/conversion.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <shaped-shader-library/filesystem/impl/watch_registry.hh>

#include <thread>

// ReadDirectoryChangesW over one I/O completion port, on one thread for every directory this filesystem
// watches — rather than a thread apiece. Together with real_filesystem.cc, this is the only part of slib
// allowed to reach the OS (see libs/graphics/shaped-shader-library/docs/coding-guidelines.md).
//
// It never has to say *what* changed. A watch is a hint to rescan and revision() is the truth, so a buffer
// overflow, an editor saving via write-temp-then-rename, and a plain modify all collapse to one action:
// fire the sink.

namespace
{
using namespace cc::primitive_defines;

/// Completion key that means "stop", posted by the destructor. Not a valid watch id, which start at 1.
constexpr ULONG_PTR k_quit_key = 0;

/// Big enough that an ordinary save never fills it. If one does, the OS hands back zero bytes and we fire
/// anyway — losing the details costs nothing when the details were never used.
constexpr DWORD k_buffer_bytes = 16 * 1024;

constexpr DWORD k_notify_filter = FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE
                                | FILE_NOTIFY_CHANGE_CREATION;

/// One watched directory: its handle, the read in flight, and the sink it feeds.
struct dir_watch
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped = {};
    std::shared_ptr<slib::impl::watch_slot> slot;

    alignas(DWORD) unsigned char buffer[k_buffer_bytes] = {}; // ReadDirectoryChangesW wants DWORD alignment
};

bool queue_read(dir_watch& w)
{
    return ReadDirectoryChangesW(w.handle, w.buffer, k_buffer_bytes, FALSE /* this directory only */, k_notify_filter,
                                 nullptr, &w.overlapped, nullptr)
        != 0;
}

class iocp_watch_backend final : public slib::impl::watch_backend
{
public:
    explicit iocp_watch_backend(HANDLE iocp) : _iocp(iocp)
    {
        _thread = std::thread([this] { run(); });
    }
    ~iocp_watch_backend() override;

    [[nodiscard]] cc::optional<slib::watch_subscription> watch_dir(cc::string_view native_dir,
                                                                   slib::watch_sink sink) override;

    /// Ends one watch. Called from the subscription's destructor, so by the time it returns the sink must be
    /// neither running nor callable again.
    void unwatch(u64 id);

private:
    void run();

    struct state
    {
        // Keyed by id, not by address: unwatch frees the watch, and an id cannot be recycled into a
        // different directory the way an address can — so a completion that arrives late is simply not found.
        cc::map<u64, std::shared_ptr<dir_watch>> watches;
        u64 next_id = 1;
    };

    HANDLE _iocp;
    std::thread _thread;
    cc::mutex<state> _state;
};

/// Ends its watch on destruction. The backend must outlive it — real_filesystem owns both, and the reload
/// watcher drops every subscription before the library tears its mounts down.
struct dir_subscription final : slib::watch_subscription::impl_base
{
    dir_subscription(iocp_watch_backend* backend, u64 id) : backend(backend), id(id) {}
    ~dir_subscription() override { backend->unwatch(id); }

    iocp_watch_backend* backend;
    u64 id;
};

iocp_watch_backend::~iocp_watch_backend()
{
    // Cancel every read still in flight before the thread goes, so nothing is left half-torn-down. Each
    // wait below is bounded: the operation is already cancelled, we are only collecting it.
    _state.lock(
        [](state& s)
        {
            for (auto [id, w] : s.watches)
            {
                CancelIoEx(w->handle, &w->overlapped);

                DWORD bytes = 0;
                (void)GetOverlappedResult(w->handle, &w->overlapped, &bytes, TRUE);
                CloseHandle(w->handle);
            }
            s.watches.clear();
        });

    PostQueuedCompletionStatus(_iocp, 0, k_quit_key, nullptr);
    _thread.join();
    CloseHandle(_iocp);
}

cc::optional<slib::watch_subscription> iocp_watch_backend::watch_dir(cc::string_view native_dir, slib::watch_sink sink)
{
    auto wide = cc::utf8_to_utf16(native_dir);
    wide.push_back(u'\0'); // utf8_to_utf16 does not terminate

    // FILE_FLAG_BACKUP_SEMANTICS is what makes CreateFileW open a directory at all; the share flags let the
    // editor we are watching for keep doing its job.
    HANDLE const handle = CreateFileW(reinterpret_cast<wchar_t const*>(wide.data()), FILE_LIST_DIRECTORY,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return cc::nullopt; // no such directory, most likely: a shipped build has no source tree

    auto w = std::make_shared<dir_watch>();
    w->handle = handle;
    w->slot = std::make_shared<slib::impl::watch_slot>(cc::move(sink));

    auto const id = _state.lock(
        [&](state& s)
        {
            auto const fresh = s.next_id++;
            s.watches[fresh] = w;
            return fresh;
        });

    if (CreateIoCompletionPort(handle, _iocp, ULONG_PTR(id), 0) == nullptr || !queue_read(*w))
    {
        unwatch(id);
        return cc::nullopt;
    }

    return slib::watch_subscription(std::make_unique<dir_subscription>(this, id));
}

void iocp_watch_backend::unwatch(u64 id)
{
    std::shared_ptr<dir_watch> w;
    _state.lock(
        [&](state& s)
        {
            if (auto* const found = s.watches.get_ptr(id); found != nullptr)
                w = cc::move(*found);
            s.watches.erase(id);
        });

    if (w == nullptr)
        return;

    // Before anything else: once this returns the sink is neither running nor callable again, which is the
    // whole promise a watch_subscription makes. The run loop below can no longer find this watch either.
    w->slot->cancel();

    // Cancel and *collect* rather than just CloseHandle: the OVERLAPPED and the buffer behind it die with
    // this object, and only a completed operation is guaranteed not to be written to again.
    CancelIoEx(w->handle, &w->overlapped);

    DWORD bytes = 0;
    (void)GetOverlappedResult(w->handle, &w->overlapped, &bytes, TRUE);
    CloseHandle(w->handle);
}

void iocp_watch_backend::run()
{
    cc::set_current_thread_name("slib fs watch");

    while (true)
    {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        BOOL const ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &overlapped, INFINITE);

        if (key == k_quit_key)
            break;

        std::shared_ptr<dir_watch> w;
        _state.lock(
            [&](state& s)
            {
                if (auto* const found = s.watches.get_ptr(u64(key)); found != nullptr)
                    w = *found;
            });

        // A completion for a watch that has since been unsubscribed. Its sink is already silenced and its
        // handle closed, so there is nobody left to tell.
        if (w == nullptr)
            continue;

        // Fire either way. `bytes == 0` means the buffer overflowed and the OS dropped the details, and a
        // read that failed outright means the directory went out from under us — both are still "look
        // again", and revision() is what will say what actually happened.
        w->slot->fire();

        // A failed read leaves nothing to re-arm: the directory is gone. The scan the sink above just asked
        // for is what notices, so stop here rather than spin on a handle that will never work again.
        if (!ok)
            continue;

        _state.lock(
            [&](state& s)
            {
                // Under the lock, so a concurrent unwatch cannot close the handle between the check and the
                // call. If it already has, the watch is simply no longer here.
                if (s.watches.get_ptr(u64(key)) != nullptr)
                    (void)queue_read(*w);
            });
    }
}
} // namespace

std::unique_ptr<slib::impl::watch_backend> slib::impl::create_watch_backend()
{
    HANDLE const iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (iocp == nullptr)
        return nullptr;

    return std::make_unique<iocp_watch_backend>(iocp);
}

#endif
