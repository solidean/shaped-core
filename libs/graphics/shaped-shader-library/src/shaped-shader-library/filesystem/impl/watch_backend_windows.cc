#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <shaped-shader-library/filesystem/impl/watch_backend.hh>

#if !CC_HAS_THREADS

// A backend has to wait on the OS somewhere, and there is no thread to wait on.
// Reporting "cannot notify" puts the caller back on polling, which is the one thing that still works here.
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

// ReadDirectoryChangesW over one I/O completion port, on one thread for every directory this filesystem watches — rather than a thread apiece.
// With real_filesystem.cc, this is the only part of slib allowed to reach the OS (see libs/graphics/shaped-shader-library/docs/coding-guidelines.md).
//
// It never has to say *what* changed.
// A buffer overflow, an editor saving via write-temp-then-rename, and a plain modify all collapse to one action: fire the sink.

namespace
{
using namespace cc::primitive_defines;

/// Completion key that means "stop", posted by the destructor.
/// Not a valid watch id, which start at 1.
constexpr ULONG_PTR k_quit_key = 0;

/// Big enough that an ordinary save never fills it.
/// If one does, the OS hands back zero bytes and we fire anyway — losing the details costs nothing when they were never used.
constexpr DWORD k_buffer_bytes = 16 * 1024;

constexpr DWORD k_notify_filter = FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE
                                | FILE_NOTIFY_CHANGE_CREATION;

/// One watched directory: its handle, the read in flight, and the sink it feeds.
///
/// `overlapped` and `buffer` belong to the kernel for as long as a read is in flight, so this object outlives
/// its subscription whenever one is: `cancelled` marks it dead to everyone, and the run loop is what finally
/// drops it, on the completion that proves the kernel is done writing.
struct dir_watch
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped = {};
    std::shared_ptr<slib::impl::watch_slot> slot;

    // Both are guarded by the backend's state mutex.
    bool read_in_flight = false;
    bool cancelled = false;

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

    /// Ends one watch.
    /// Called from the subscription's destructor, so by the time it returns the sink must be neither running nor callable again.
    void unwatch(u64 id);

private:
    void run();

    struct state
    {
        // Keyed by id, not by address: an id cannot be recycled into a different directory the way an address can.
        // A completion that arrives late is then simply not found.
        cc::map<u64, std::shared_ptr<dir_watch>> watches;
        u64 next_id = 1;
    };

    HANDLE _iocp;
    std::thread _thread;
    cc::mutex<state> _state;
};

/// Ends its watch on destruction.
/// The backend must outlive it — real_filesystem owns both.
struct dir_subscription final : slib::watch_subscription::impl_base
{
    dir_subscription(iocp_watch_backend* backend, u64 id) : backend(backend), id(id) {}
    ~dir_subscription() override { backend->unwatch(id); }

    iocp_watch_backend* backend;
    u64 id;
};

iocp_watch_backend::~iocp_watch_backend()
{
    // Cancel whatever is left — a subscription outliving its backend is a contract violation, but a watch whose
    // directory went away sits here with no read in flight and nobody to retire it.
    _state.lock(
        [](state& s)
        {
            cc::vector<u64> retired;

            // By value: cc::map hands back a proxy reference, and the shared_ptr copy still writes through to the watch.
            for (auto [id, w] : s.watches)
            {
                w->cancelled = true;

                // With a read in flight the run loop retires it, on the completion CancelIoEx is about to produce.
                if (w->read_in_flight)
                {
                    CancelIoEx(w->handle, &w->overlapped);
                    continue;
                }

                CloseHandle(w->handle);
                retired.push_back(id);
            }

            for (auto const id : retired)
                s.watches.erase(id);
        });

    // The run loop leaves only once every cancelled read has come back, so the quit packet is a request, not a cut.
    PostQueuedCompletionStatus(_iocp, 0, k_quit_key, nullptr);
    _thread.join();
    CloseHandle(_iocp);
}

cc::optional<slib::watch_subscription> iocp_watch_backend::watch_dir(cc::string_view native_dir, slib::watch_sink sink)
{
    auto wide = cc::utf8_to_utf16(native_dir);
    wide.push_back(u'\0'); // utf8_to_utf16 does not terminate

    // FILE_FLAG_BACKUP_SEMANTICS is what makes CreateFileW open a directory at all; the share flags let the editor we are watching for keep doing its job.
    HANDLE const handle = CreateFileW(reinterpret_cast<wchar_t const*>(wide.data()), FILE_LIST_DIRECTORY,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return cc::nullopt; // no such directory, most likely: a shipped build has no source tree

    auto w = std::make_shared<dir_watch>();
    w->handle = handle;
    w->slot = std::make_shared<slib::impl::watch_slot>(cc::move(sink));

    // Registered, bound and armed under one lock, so read_in_flight cannot be stale when the first completion arrives.
    // Both calls only *start* work — neither waits on the kernel, which is what makes holding the lock safe.
    bool armed = false;
    auto const id = _state.lock(
        [&](state& s)
        {
            auto const fresh = s.next_id++;
            s.watches[fresh] = w;

            if (CreateIoCompletionPort(handle, _iocp, ULONG_PTR(fresh), 0) != nullptr)
                w->read_in_flight = queue_read(*w);

            armed = w->read_in_flight; // read here: once the lock is gone, a completion may already have cleared it
            return fresh;
        });

    if (!armed)
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
            auto* const found = s.watches.get_ptr(id);
            if (found == nullptr)
                return;

            w = *found;
            w->cancelled = true; // dead to the run loop from here: it will neither fire this sink nor re-arm the read

            // A read in flight owns the OVERLAPPED and the buffer, and only the kernel can say when it is done with them.
            // So cancel it and leave the entry standing; the run loop retires it when that cancellation completes.
            if (w->read_in_flight)
            {
                CancelIoEx(w->handle, &w->overlapped);
                return;
            }

            CloseHandle(w->handle);
            s.watches.erase(id);
        });

    if (w == nullptr)
        return;

    // Outside the lock: cancel() blocks until an in-flight fire() has returned, and fire() runs unlocked on the run
    // loop — waiting for it while holding the state mutex would deadlock against the very thread we are waiting for.
    // Once it returns the sink is neither running nor callable again, which is the promise watch_subscription makes.
    w->slot->cancel();
}

void iocp_watch_backend::run()
{
    cc::set_current_thread_name("slib fs watch");

    // The quit packet is a request to leave once every cancelled read has reported back.
    // Leaving earlier would strand buffers the kernel still owns, which is what the destructor must not do.
    bool quitting = false;

    while (true)
    {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        BOOL const ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &overlapped, INFINITE);

        // A completion ends the read that produced it — the cancelled ones included, which is what makes this the
        // one place a dir_watch can be dropped without racing the kernel over its buffer.
        std::shared_ptr<dir_watch> w;
        auto const drained = _state.lock(
            [&](state& s)
            {
                if (key != k_quit_key)
                {
                    if (auto* const found = s.watches.get_ptr(u64(key)); found != nullptr)
                    {
                        (*found)->read_in_flight = false;

                        if ((*found)->cancelled)
                        {
                            CloseHandle((*found)->handle);
                            s.watches.erase(u64(key));
                        }
                        else
                            w = *found;
                    }
                }

                return s.watches.empty();
            });

        if (key == k_quit_key)
        {
            quitting = true;
            if (drained)
                break;
            continue;
        }

        // Either a watch that has since been unsubscribed — its sink is already silenced, so there is nobody left to
        // tell — or the last cancelled one, which the destructor is waiting on.
        if (w == nullptr)
        {
            if (quitting && drained)
                break;
            continue;
        }

        // Fire either way.
        // `bytes == 0` means the buffer overflowed and the OS dropped the details; a read that failed outright means the directory went out from under us.
        // Both are still "look again", and revision() is what says what actually happened.
        w->slot->fire();

        // A failed read leaves nothing to re-arm: the directory is gone.
        // The scan the sink above just asked for is what notices, so stop here rather than spin on a handle that will never work again.
        if (!ok)
            continue;

        _state.lock(
            [&](state& s)
            {
                // Under the lock, so a concurrent unwatch cannot cancel between the check and the call.
                // If it already has, the watch is either gone or marked, and re-arming it would resurrect a read nobody will collect.
                if (auto* const found = s.watches.get_ptr(u64(key)); found != nullptr && !(*found)->cancelled)
                    (*found)->read_in_flight = queue_read(*w);
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
