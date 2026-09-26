#pragma once

#include "framing.hh"
#include "fwd.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread_pump.hh>

/// The native host: a server over this process's stdin and stdout, framed by `Content-Length`.
///
/// **Opening it takes stdout over for good.**
/// The real stdout becomes a descriptor only the protocol writes to, and fd 1 is pointed at stderr, so a stray
/// `cc::print` anywhere in the process lands on stderr as a line in the editor's log instead of corrupting a frame.
/// Both ends are in binary mode, since Windows' text mode would turn the header's `\r\n` into `\r\r\n`.
///
/// Input is read by an actor: with threads, its thread blocks on stdin; without them, its pump checks stdin without
/// blocking and waits a millisecond when there is nothing, so an idle unthreaded server does not spin.
class lsp::stdio_host
{
public:
    [[nodiscard]] static cc::unique_ptr<stdio_host> open();

    ~stdio_host();
    stdio_host(stdio_host&&) = delete;
    stdio_host& operator=(stdio_host&&) = delete;

    /// Moves messages between the process's streams and `s` until the client sends `exit` or closes stdin.
    /// Resolves to the process's exit code; `s` must outlive the returned async.
    [[nodiscard]] cc::shared_async<int> serve(server& s);

    // implementation, public for the reader actor
public:
    struct input
    {
        cc::vector<cc::string> messages;
        bool is_closed = false;
    };

    stdio_host();

private:
    bool impl_pump();
    void impl_write(cc::string_view bytes);

    struct reader;
    cc::unique_ptr<reader> _reader;
    cc::mutex<input> _input;
    int _output_fd = -1;
    server* _server = nullptr;
    cc::shared_async<int> _done;
    cc::thread_pump_registration _pump;
};
