#include "metal_driver.hh"

#include <clean-core/string/format.hh>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace
{
using namespace cc::primitive_defines;

/// One end of a pipe, closed exactly once however the function leaves.
struct fd_guard
{
    int fd = -1;

    fd_guard() = default;
    explicit fd_guard(int f) : fd(f) {}
    fd_guard(fd_guard const&) = delete;
    fd_guard& operator=(fd_guard const&) = delete;
    ~fd_guard() { close_now(); }

    void close_now()
    {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
};

/// Reads whatever is available and hands it to `append`; false once the writer has closed its end.
/// `append` takes the bytes rather than a container, because `cc::vector` and `cc::string` name no element type.
template <class Append>
bool drain(int fd, Append&& append)
{
    char buffer[4096];
    auto const n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0)
    {
        append(buffer, isize(n));
        return true;
    }
    // EAGAIN means the pipe is merely empty; 0 is the writer's end closed, and any other error ends it too.
    return n < 0 && errno == EAGAIN;
}
} // namespace

cc::result<ssc::msl::impl::process_result> ssc::msl::impl::run_process(cc::string_view executable,
                                                                       cc::span<cc::string const> arguments,
                                                                       cc::string_view input)
{
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0)
        return cc::error(cc::format("run_process: could not create a pipe for '{}' (errno {})", executable, errno));

    // Every pipe end must be close-on-exec, and this is a correctness requirement rather than hygiene.
    // Two threads compiling at once each spawn a child, and without it one child inherits the OTHER's write end —
    // so that pipe never reaches end-of-file, and both compiles hang until the unrelated child exits.
    // The dup2 below re-creates 0/1/2 in the child without the flag, which is what keeps the child's own streams open.
    for (auto const fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    auto in_read = fd_guard(in_pipe[0]);
    auto in_write = fd_guard(in_pipe[1]);
    auto out_read = fd_guard(out_pipe[0]);
    auto out_write = fd_guard(out_pipe[1]);
    auto err_read = fd_guard(err_pipe[0]);
    auto err_write = fd_guard(err_pipe[1]);

    // The child keeps one end of each pipe and inherits nothing else of ours.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_read.fd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_write.fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_write.fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_write.fd);
    posix_spawn_file_actions_addclose(&actions, out_read.fd);
    posix_spawn_file_actions_addclose(&actions, err_read.fd);

    // cc::string::data() is not null-terminated and argv must be, so each argument gets a terminated buffer.
    auto argv_storage = cc::vector<cc::vector<char>>();
    auto const terminated = [&argv_storage](cc::string_view s)
    {
        auto buffer = cc::vector<char>();
        for (auto const c : s)
            buffer.push_back(c);
        buffer.push_back('\0');
        argv_storage.push_back(cc::move(buffer));
    };
    terminated(executable);
    for (auto const& a : arguments)
        terminated(a);

    auto argv = cc::vector<char*>();
    for (auto& a : argv_storage)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    auto const spawned = ::posix_spawn(&pid, argv_storage[0].data(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return cc::error(cc::format("run_process: could not start '{}' ({})", executable, ::strerror(spawned)));

    // Our copies of the child's ends must go, or reading stdout would never see end-of-file.
    in_read.close_now();
    out_write.close_now();
    err_write.close_now();

    auto result = process_result();
    auto written = isize(0);
    // Non-blocking throughout: `poll` says a stream is ready, never how much it will take, so a blocking write of a
    // large shader would stall inside write() with the child's output still unread.
    ::fcntl(out_read.fd, F_SETFL, O_NONBLOCK);
    ::fcntl(err_read.fd, F_SETFL, O_NONBLOCK);
    ::fcntl(in_write.fd, F_SETFL, O_NONBLOCK);

    auto out_open = true;
    auto err_open = true;
    while (out_open || err_open || in_write.fd >= 0)
    {
        pollfd fds[3] = {};
        auto count = 0;
        auto const out_slot = out_open ? count++ : -1;
        if (out_open)
            fds[out_slot] = {.fd = out_read.fd, .events = POLLIN, .revents = 0};
        auto const err_slot = err_open ? count++ : -1;
        if (err_open)
            fds[err_slot] = {.fd = err_read.fd, .events = POLLIN, .revents = 0};
        auto const in_slot = in_write.fd >= 0 ? count++ : -1;
        if (in_write.fd >= 0)
            fds[in_slot] = {.fd = in_write.fd, .events = POLLOUT, .revents = 0};

        if (count == 0)
            break;
        if (::poll(fds, nfds_t(count), -1) < 0)
        {
            if (errno == EINTR)
                continue;
            return cc::error(cc::format("run_process: poll failed for '{}' (errno {})", executable, errno));
        }

        if (out_open && (fds[out_slot].revents & (POLLIN | POLLHUP)) != 0)
            out_open = drain(out_read.fd,
                             [&result](char const* data, isize count)
                             {
                                 for (auto i = isize(0); i < count; ++i)
                                     result.out.push_back(byte(data[i]));
                             });
        if (err_open && (fds[err_slot].revents & (POLLIN | POLLHUP)) != 0)
            err_open = drain(err_read.fd,
                             [&result](char const* data, isize count)
                             {
                                 for (auto i = isize(0); i < count; ++i)
                                     result.err += data[i];
                             });

        if (in_slot >= 0 && (fds[in_slot].revents & (POLLOUT | POLLERR | POLLHUP)) != 0)
        {
            auto const remaining = input.size() - written;
            auto const n = remaining > 0 ? ::write(in_write.fd, input.data() + written, size_t(remaining)) : isize(-1);
            if (n > 0)
                written += isize(n);

            auto const would_block = n < 0 && (errno == EAGAIN || errno == EINTR);
            // Closing stdin is what tells the compiler its input ended; anything but a partial write ends it.
            if (!would_block && (n <= 0 || written == input.size()))
                in_write.close_now();
        }
    }

    auto status = 0;
    while (::waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
            return cc::error(cc::format("run_process: could not wait for '{}' (errno {})", executable, errno));
    }

    if (WIFSIGNALED(status))
        return cc::error(cc::format("run_process: '{}' was killed by signal {}", executable, WTERMSIG(status)));

    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

cc::string ssc::msl::impl::resolve_metal_driver()
{
    auto const args = cc::vector<cc::string>{"-sdk", "macosx", "-f", "metal"};
    auto r = run_process("/usr/bin/xcrun", args, "");
    if (r.has_error() || r.value().exit_code != 0)
        return {};

    auto path = cc::string();
    for (auto const b : r.value().out)
    {
        auto const c = char(b);
        if (c == '\n' || c == '\r')
            break;
        path += c;
    }

    return path;
}

cc::string ssc::msl::impl::query_driver_version(cc::string_view driver_path)
{
    if (driver_path.empty())
        return {};

    auto const args = cc::vector<cc::string>{"--version"};
    auto r = run_process(driver_path, args, "");
    if (r.has_error())
        return {};

    // The driver reports on either stream depending on the release, so both are searched.
    auto out_text = cc::string();
    for (auto const b : r.value().out)
        out_text += char(b);

    // "Apple metal version 32023.883 (metalfe-32023.883)" — what a cache key wants is the word after "version".
    auto const extract = [](cc::string_view text) -> cc::string
    {
        auto const marker = cc::string_view("version ");
        auto const at = text.find(marker);
        if (at < 0)
            return {};

        auto version = cc::string();
        for (auto i = at + marker.size(); i < text.size(); ++i)
        {
            auto const c = text[i];
            if (c == ' ' || c == '\n' || c == '\r')
                break;
            version += c;
        }
        return version;
    };

    auto version = extract(out_text);
    return version.empty() ? extract(r.value().err) : version;
}
