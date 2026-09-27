#pragma once

#include "documents.hh"
#include "fwd.hh"
#include "json.hh"
#include "types.hh"

#include <babel-data/data/json.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread_pump.hh>

/// A language server's protocol core: JSON-RPC, the LSP lifecycle and the open documents, with no I/O of its own.
///
/// Whole messages go in through `receive` and whole messages come out of `take_outgoing`; a host moves them over a
/// transport — stdin/stdout framed by `Content-Length`, or a browser worker's `postMessage` with no framing at all.
///
/// **Everything here is touched by one thread at a time, the one driving it**: `receive`, `poll`, handlers and
/// `when_ready` callbacks all run there.
/// A handler hands its heavy work to `cc::compute_scheduler()` as an async and returns it; `poll` notices completions
/// and answers.
/// Document changes are applied inside `receive`, before any later request is answered, as LSP requires.

namespace lsp
{
/// What a request answers: its result, or an error response.
template <class R>
using answer = cc::result<R, response_error>;

} // namespace lsp

/// The flag `$/cancelRequest` raises, which a long handler reads between steps.
struct lsp::cancel_flag
{
    cc::atomic<bool> is_raised = false;
};

namespace lsp
{

namespace impl
{
/// A request's answer as the text of its response members, never failing, so a watcher can depend on it.
struct reply
{
    bool is_error = false;
    i32 error_code = 0;
    cc::string text;
};

template <class R>
cc::shared_async<reply> reply_of(cc::shared_async<answer<R>> a, cc::shared_ptr<cancel_flag> flag)
{
    co_await cc::async_settled(a);
    if (auto const* v = a->try_value())
    {
        if (v->has_value())
            co_return reply{.text = json::to_text(v->value())};
        co_return reply{.is_error = true, .error_code = v->error().code, .text = v->error().message};
    }
    if (flag->is_raised.load() || a->try_error()->is_cancelled())
        co_return reply{.is_error = true, .error_code = error_code::request_cancelled, .text = "cancelled"};
    co_return reply{.is_error = true,
                    .error_code = error_code::internal_error,
                    .text = a->try_error()->underlying().to_string()};
}

/// Wakes the loop driving the server once `a` has settled, so a completion on another thread is never slept through.
template <class T>
cc::shared_async<int> notify_when_settled(cc::shared_async<T> a)
{
    co_await cc::async_settled(a);
    cc::thread_pump_notify();
    co_return 0;
}

struct pending_request
{
    /// The request's id as JSON text, echoed exactly: `7` or `"abc"`.
    cc::string id;
    cc::string method;
    cc::shared_async<reply> outcome;
    cc::shared_ptr<cancel_flag> flag;
};

struct watch
{
    cc::unique_function<bool()> is_ready;
    cc::unique_function<void()> fire;
};
} // namespace impl
} // namespace lsp

/// What a handler knows about the request it answers.
struct lsp::request_context
{
    cc::shared_ptr<cancel_flag> flag;

    /// True once the client cancelled the request.
    [[nodiscard]] bool is_cancelled() const { return flag->is_raised.load(); }
};

class lsp::server
{
public:
    struct options
    {
        cc::string name;
        cc::string version;
        /// Writes the server's own capabilities into the `capabilities` object of the `initialize` result.
        /// The position encoding and the document sync are the server core's, and written beside them.
        cc::unique_function<void(json::writer&)> write_capabilities;
    };

    explicit server(options opts);

    server(server&&) = delete;
    server& operator=(server&&) = delete;

    // transport
public:
    /// One whole JSON-RPC message, as the client sent it.
    void receive(cc::string_view message);

    /// Answers every request whose handler has finished and runs every `when_ready` callback whose async has; true
    /// when anything happened.
    bool poll();

    /// The messages to send, oldest first; the host frames and writes them.
    [[nodiscard]] cc::vector<cc::string> take_outgoing();

    /// True when no request is in flight, nothing is watched, and every wake-up for them has run.
    [[nodiscard]] bool is_idle() const { return _pending.empty() && _watches.empty() && _wakers.empty(); }

    // handlers
public:
    /// `handler(request_context, Params) -> cc::shared_async<answer<R>>`, called on the driving thread.
    /// Params that do not read are answered with InvalidParams before the handler is called.
    template <class Params, class F>
    void on_request(cc::string_view method, F handler);

    /// `handler(Params)`, called on the driving thread; params that do not read are logged and dropped.
    template <class Params, class F>
    void on_notification(cc::string_view method, F handler);

    /// Called after a document was opened, changed or closed, with its uri, on the driving thread.
    void on_document(cc::unique_function<void(cc::string_view uri)> handler) { _on_document = cc::move(handler); }

    /// Called once the client sent `initialized`.
    void on_initialized(cc::unique_function<void()> handler) { _on_initialized = cc::move(handler); }

    /// Runs `then(a)` on the driving thread once `a` is ready, value or error; starts `a` if it is cold.
    template <class T, class F>
    void when_ready(cc::shared_async<T> a, F then);

    // server to client
public:
    template <class Params>
    void notify(cc::string_view method, Params const& params);

    /// A request whose response is not waited for, such as `workspace/inlayHint/refresh`.
    template <class Params>
    void request(cc::string_view method, Params const& params);

    /// `window/logMessage`, safe to call from any thread; queued until the next `poll`, which it wakes the loop for.
    void post_log(message_type type, cc::string message);

    // state
public:
    [[nodiscard]] lsp::workspace const& workspace() const { return _workspace; }
    [[nodiscard]] position_encoding encoding() const { return _encoding; }

    /// True once `initialize` was answered; readable from any thread.
    [[nodiscard]] bool is_initialized() const { return _is_initialized.load(); }

    /// The `initialize` request's params, invalid before it arrived.
    [[nodiscard]] json::ref initialize_params() const;

    [[nodiscard]] bool has_exited() const { return _has_exited; }

    /// 0 after `shutdown` then `exit`, 1 after an `exit` with no `shutdown` before it, as LSP asks.
    [[nodiscard]] int exit_code() const { return _has_shut_down ? 0 : 1; }

    // implementation
private:
    using request_handler
        = cc::unique_function<cc::optional<cc::shared_async<impl::reply>>(request_context const&, json::ref)>;
    using notification_handler = cc::unique_function<void(json::ref)>;

    void impl_handle_request(cc::string id, cc::string_view method, json::ref params);
    void impl_handle_notification(cc::string_view method, json::ref params);
    void impl_initialize(cc::string const& id, json::ref params);
    void impl_respond(cc::string_view id, impl::reply const& r);
    void impl_send(cc::string text) { _outgoing.push_back(cc::move(text)); }

    options _options;
    lsp::workspace _workspace;
    position_encoding _encoding = position_encoding::utf16;
    cc::map<cc::string, request_handler> _requests;
    cc::map<cc::string, notification_handler> _notifications;
    cc::unique_function<void(cc::string_view)> _on_document;
    cc::unique_function<void()> _on_initialized;

    cc::vector<impl::pending_request> _pending;
    cc::vector<impl::watch> _watches;
    /// The nodes that wake the driving loop, kept until they ran, so nothing of the server outlives it detached.
    cc::vector<cc::shared_async<int>> _wakers;
    cc::vector<cc::string> _outgoing;
    cc::mutex<cc::vector<log_message_params>> _logs;
    cc::optional<babel::json::document> _initialize_document;
    cc::atomic<bool> _is_initialized = false;
    bool _has_shut_down = false;
    bool _has_exited = false;
    i64 _next_request_id = 0;
};

// implementation

template <class Params, class F>
void lsp::server::on_request(cc::string_view method, F handler)
{
    _requests[cc::string(method)]
        = [handler = cc::move(handler)](request_context const& ctx,
                                        json::ref in) mutable -> cc::optional<cc::shared_async<impl::reply>>
    {
        auto params = Params();
        if (!read(in, params))
            return cc::nullopt;
        return impl::reply_of(handler(ctx, cc::move(params)), ctx.flag);
    };
}

template <class Params, class F>
void lsp::server::on_notification(cc::string_view method, F handler)
{
    _notifications[cc::string(method)] = [handler = cc::move(handler), method = cc::string(method)](json::ref in) mutable
    {
        auto params = Params();
        if (!read(in, params))
        {
            CC_LOG_WARNING("{}: params that do not read, dropped", method);
            return;
        }
        handler(cc::move(params));
    };
}

template <class T, class F>
void lsp::server::when_ready(cc::shared_async<T> a, F then)
{
    cc::async_start(a);
    _wakers.push_back(cc::async_start(impl::notify_when_settled(a)));
    _watches.push_back({
        .is_ready = [a] { return a->is_ready(); },
        .fire = [a, then = cc::move(then)]() mutable { then(*a); },
    });
}

template <class Params>
void lsp::server::notify(cc::string_view method, Params const& params)
{
    auto w = babel::json::string_writer();
    auto& j = w.underlying();
    j.begin_object();
    j.write("jsonrpc", "2.0");
    j.write("method", method);
    json::put(j, "params", params);
    j.end_object();
    if (auto text = w.finish(); text.has_value())
        impl_send(cc::move(text.value()));
}

template <class Params>
void lsp::server::request(cc::string_view method, Params const& params)
{
    auto w = babel::json::string_writer();
    auto& j = w.underlying();
    j.begin_object();
    j.write("jsonrpc", "2.0");
    j.write("id", _next_request_id++);
    j.write("method", method);
    json::put(j, "params", params);
    j.end_object();
    if (auto text = w.finish(); text.has_value())
        impl_send(cc::move(text.value()));
}
