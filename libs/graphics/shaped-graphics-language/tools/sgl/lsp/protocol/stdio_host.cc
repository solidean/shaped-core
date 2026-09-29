#include "stdio_host.hh"

#include "server.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/time.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>

#ifdef CC_OS_WINDOWS
#include <clean-core/platform/win32_sanitized.hh>
#include <fcntl.h>
#include <io.h>
#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

using namespace cc::primitive_defines;

namespace
{
/// The one message the reader takes, which exists only because an actor must accept one.
struct wake
{
};

/// What the host and the reader's thread share; the host owns it and outlives the thread.
struct reader_state
{
    cc::mutex<lsp::stdio_host::input>* input = nullptr;
    bool is_threaded = false;
    cc::atomic<bool> is_stopping = false;
    cc::atomic<bool> has_exited = false;
#ifdef CC_OS_WINDOWS
    /// The reader thread's own handle, null until the thread has made it.
    cc::atomic<HANDLE> thread = nullptr;
#endif
};

constexpr isize read_size = 64 * 1024;

/// Reads stdin in whatever pieces it arrives, and hands whole messages to the host.
struct reader_impl final : cc::threaded_actor_impl<wake>
{
    explicit reader_impl(reader_state* s) : state(s) {}

    reader_state* state;
    lsp::frame_reader frames;
    byte buffer[read_size];

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "lsp stdin"; }

    void on_thread_init() override
    {
#ifdef CC_OS_WINDOWS
        // a blocking ReadFile is only ever interrupted by CancelSynchronousIo, which needs this thread's handle
        if (state->is_threaded)
        {
            auto h = HANDLE(nullptr);
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                state->thread.store(h);
        }
#endif
    }

    void on_thread_shutdown() override { state->has_exited.store(true); }

    void on_message(wake) override {}

    bool on_process() override
    {
        if (state->is_stopping.load())
            return false;
        auto const n = state->is_threaded ? read_blocking() : read_available();
        if (n < 0)
        {
            close();
            return false;
        }
        if (n == 0)
            return state->is_threaded;
        // the messages framed before a broken header still reach the server
        auto messages = frames.feed(cc::span<byte const>(buffer, n));
        if (!messages.empty())
        {
            state->input->lock(
                [&](lsp::stdio_host::input& in)
                {
                    for (auto& m : messages)
                        in.messages.push_back(cc::move(m));
                });
            cc::thread_pump_notify();
        }
        if (frames.is_broken())
        {
            close();
            return false;
        }
        return true;
    }

    void close()
    {
        state->input->lock([](lsp::stdio_host::input& in) { in.is_closed = true; });
        cc::thread_pump_notify();
    }

    /// Bytes read, 0 when interrupted without any, -1 at end of input.
    [[nodiscard]] isize read_blocking()
    {
#ifdef CC_OS_WINDOWS
        auto read = DWORD(0);
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, DWORD(read_size), &read, nullptr))
            return GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
        return read == 0 ? -1 : isize(read);
#else
        // a timeout, so a stop request is seen without anything interrupting the read
        auto p = pollfd{.fd = 0, .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, 50) <= 0)
            return 0;
        return read_posix();
#endif
    }

    /// Bytes read without waiting, 0 when none are there, -1 at end of input.
    [[nodiscard]] isize read_available()
    {
#ifdef CC_OS_WINDOWS
        auto const h = GetStdHandle(STD_INPUT_HANDLE);
        auto const type = GetFileType(h);
        if (type == FILE_TYPE_PIPE)
        {
            auto available = DWORD(0);
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr))
                return -1;
            if (available == 0)
                return 0;
            auto read = DWORD(0);
            auto const want = available < DWORD(read_size) ? available : DWORD(read_size);
            if (!ReadFile(h, buffer, want, &read, nullptr))
                return -1;
            return isize(read);
        }
        // a console is signalled once it holds input, and a ReadFile before that would block
        if (type == FILE_TYPE_CHAR && WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
            return 0;
        // a file never blocks for long, so it is read as it is
        auto read = DWORD(0);
        if (!ReadFile(h, buffer, DWORD(read_size), &read, nullptr) || read == 0)
            return -1;
        return isize(read);
#else
        auto p = pollfd{.fd = 0, .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, 0) <= 0)
            return 0;
        return read_posix();
#endif
    }

#ifndef CC_OS_WINDOWS
    /// One read of stdin once poll said it is ready: bytes read, 0 when there were none after all, -1 at end of input.
    [[nodiscard]] isize read_posix()
    {
        auto n = ::read(0, buffer, read_size);
        while (n < 0 && errno == EINTR)
            n = ::read(0, buffer, read_size);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return n <= 0 ? -1 : isize(n);
    }
#endif
};
} // namespace

struct lsp::stdio_host::reader
{
    reader_state state;
    cc::unique_ptr<cc::threaded_actor<wake>> actor;
};

lsp::stdio_host::stdio_host(passkey)
{
}

cc::unique_ptr<lsp::stdio_host> lsp::stdio_host::open()
{
    auto host = cc::make_unique<stdio_host>(passkey());
    cc::flush();
#ifdef CC_OS_WINDOWS
    // 0, 1 and 2 are stdin, stdout and stderr on every platform
    _setmode(0, _O_BINARY);
    host->_output_fd = _dup(1);
    _setmode(host->_output_fd, _O_BINARY);
    _dup2(2, 1);
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#else
    // the process is the server's, and a client that closed its end must fail a write, not end the process
    ::signal(SIGPIPE, SIG_IGN);
    host->_output_fd = ::dup(1);
    ::dup2(2, 1);
#endif
    return host;
}

lsp::stdio_host::~stdio_host()
{
    _pump.reset();
    if (_reader == nullptr)
        return;
    _reader->state.is_stopping.store(true);
    _reader->actor->begin_shutdown();
#ifdef CC_OS_WINDOWS
    // The thread may be inside ReadFile, which only CancelSynchronousIo interrupts.
    // It may also be just about to enter it, or not yet have made its handle, so the cancel repeats until the thread is
    // seen gone, for a bounded time.
    if (_reader->state.is_threaded)
    {
        auto const deadline = cc::current_time_steady_secs() + 2.0;
        while (!_reader->state.has_exited.load() && cc::current_time_steady_secs() < deadline)
        {
            if (auto const h = _reader->state.thread.load(); h != nullptr)
                CancelSynchronousIo(h);
            Sleep(1);
        }
    }
#endif
    _reader->actor->shutdown();
#ifdef CC_OS_WINDOWS
    if (auto const h = _reader->state.thread.exchange(nullptr); h != nullptr)
        CloseHandle(h);
    if (_output_fd >= 0)
        _close(_output_fd);
#else
    if (_output_fd >= 0)
        ::close(_output_fd);
#endif
}

cc::shared_async<int> lsp::stdio_host::serve(server& s)
{
    CC_ASSERT(_server == nullptr, "a host serves one server once");
    _server = &s;
    _done = cc::make_async_manual<int>();

    _reader = cc::make_unique<reader>();
    _reader->state.input = &_input;
    _reader->state.is_threaded = CC_HAS_THREADS != 0;
    _reader->actor = cc::make_threaded_actor<reader_impl>(&_reader->state);
    _reader->actor->start(_reader->state.is_threaded ? cc::threaded_actor_mode::threaded_if_possible
                                                     : cc::threaded_actor_mode::unthreaded);

    _pump = cc::register_thread_pump([this] { return impl_pump(); });
    return _done;
}

bool lsp::stdio_host::impl_pump()
{
    if (_done->is_ready())
        return false;

    auto in = _input.lock(
        [](input& i)
        {
            auto taken = input{.messages = cc::move(i.messages), .is_closed = i.is_closed};
            i.messages.clear();
            return taken;
        });
    auto progress = !in.messages.empty();
    for (auto const& m : in.messages)
        _server->receive(m);
    progress = _server->poll() || progress;
    for (auto const& m : _server->take_outgoing())
    {
        impl_write(frame(m));
        progress = true;
    }

    if (_server->has_exited())
    {
        _done->push_value(_server->exit_code());
        return true;
    }
    if (in.is_closed && _server->is_idle())
    {
        CC_LOG_INFO("stdin closed without `exit`; the server stops");
        _done->push_value(1);
        return true;
    }

#if !CC_HAS_THREADS
    // Nothing parks a loop without threads, so an idle server would spin: wait a millisecond on the OS instead.
    // Only when idle, since a sweep with work pending must stay fast.
    if (!progress && _server->is_idle())
    {
#ifdef CC_OS_WINDOWS
        Sleep(1);
#else
        auto const pause = timespec{.tv_sec = 0, .tv_nsec = 1'000'000};
        nanosleep(&pause, nullptr);
#endif
    }
    // Stdin is known work no other pump drives: without threads this pump is the only thing that reads it, so it
    // reports work while it serves, or the loop would take the wait for input as a deadlock.
    return true;
#else
    return progress;
#endif
}

void lsp::stdio_host::impl_write(cc::string_view bytes)
{
    while (!bytes.empty() && !_is_output_dead)
    {
#ifdef CC_OS_WINDOWS
        auto const n = _write(_output_fd, bytes.data(), unsigned(bytes.size()));
#else
        auto const n = ::write(_output_fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
        {
            CC_LOG_ERROR("writing to stdout failed; the client is gone, and nothing more is written");
            _is_output_dead = true;
            return;
        }
        bytes.remove_prefix(isize(n));
    }
}
