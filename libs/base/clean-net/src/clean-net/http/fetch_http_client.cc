#include "fetch_http_client.hh"

#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/http/impl/body_flow.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/impl/reactor.hh>

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/threading.h>
#endif
#endif

// One request, as a state machine stepped by the reactor.
//
// THE JAVASCRIPT SIDE RUNS ON THE MAIN RUNTIME THREAD, AND THE STATE MACHINE ON WHATEVER THREAD PUMPS THE REACTOR.
// Every call into JavaScript is proxied there, and every answer comes back as a signal: the answer is parked in the
// exchange, and the request's gate wakes the manual operation it waits on.
// The gate is the one `resume_body` hands out too, so a JavaScript answer and a sink's resume are the same wake-up --
// and a wake-up with nothing behind it just waits again.
//
// ONE JAVASCRIPT CALL IS OUTSTANDING AT A TIME: the fetch until its head, then one read until its chunk.
// That is what lets the answer be a single slot, and it is also the backpressure: nothing is read while the sink
// still holds bytes it has not taken.

namespace cnet
{
namespace
{
#if defined(__EMSCRIPTEN__)

enum class fetch_answer_kind : u8
{
    none,
    head,
    chunk,
    done,
    failed,
};

/// What the JavaScript side last answered, waiting for the reactor to step on it.
struct fetch_answer
{
    fetch_answer_kind kind = fetch_answer_kind::none;
    http_response_head head;
    cc::vector<byte> bytes;
    cc::string failure;
};

struct fetch_exchange
{
    io_system& io;
    http_request request;
    body_sink sink;
    cancel_token token;
    cc::shared_async<http_response_head> promise;

    i64 deadline_ns = 0;
    i64 max_body_bytes = 0;
    i64 body_bytes = 0;

    /// Woken by every JavaScript answer and by the sink's resume alike.
    impl::body_gate* gate = nullptr;

    /// Written on the main runtime thread, taken on the reactor's.
    cc::mutex<fetch_answer> answer;

    http_response_head head;

    /// A chunk the sink has not taken all of yet, and how much of it it has.
    cc::vector<byte> unconsumed;
    isize consumed = 0;

    bool settled = false;

    /// What the JavaScript side knows this request by: a strong reference of its own, alive while its table holds
    /// the request and freed when the table lets go of it -- after a final answer, or in `forget_on_main`.
    /// **Main runtime thread only**, like `forgotten`.
    cc::shared_ptr<fetch_exchange>* js_handle = nullptr;

    /// Set once `forget_on_main` ran, so a start or a read posted before it and run after it does nothing.
    bool forgotten = false;

    explicit fetch_exchange(io_system& io_ref) : io(io_ref) {}
    fetch_exchange(fetch_exchange const&) = delete;
    fetch_exchange& operator=(fetch_exchange const&) = delete;
    ~fetch_exchange() { impl::body_gate_release(gate); }
};

[[nodiscard]] fetch_exchange& exchange_of(void* js_handle)
{
    return **static_cast<cc::shared_ptr<fetch_exchange>*>(js_handle);
}

void answer(void* js_handle, fetch_answer value)
{
    auto& ex = exchange_of(js_handle);
    ex.answer.lock([&](fetch_answer& a) { a = cc::move(value); });
    impl::body_gate_resume(ex.gate);
}

/// Drop the table's reference, once the table has let go of the request.
void release_js_handle(void* js_handle)
{
    auto* const handle = static_cast<cc::shared_ptr<fetch_exchange>*>(js_handle);
    (*handle)->js_handle = nullptr;
    delete handle;
}

[[nodiscard]] cc::string take_c_string(char* text)
{
    auto out = cc::string(text == nullptr ? "" : text);
    delete[] text;
    return out;
}
#endif
} // namespace
} // namespace cnet

#if defined(__EMSCRIPTEN__)

// The answers, called from JavaScript on the main runtime thread.
// Every string and chunk arrives in memory the JavaScript side allocated through `cnet_fetch_alloc`, and is freed here.
extern "C"
{
    EMSCRIPTEN_KEEPALIVE char* cnet_fetch_alloc(int size)
    {
        return new char[size > 0 ? size : 1];
    }

    EMSCRIPTEN_KEEPALIVE void cnet_fetch_on_head(void* handle, int status, char* reason, char* headers)
    {
        auto head = cnet::http_response_head{.status = status, .reason = cnet::take_c_string(reason)};

        // Name and value alternate, one per line: neither can hold a newline in a `fetch` response.
        auto const text = cnet::take_c_string(headers);
        auto rest = cc::string_view(text);
        while (!rest.empty())
        {
            auto const name_end = rest.find('\n');
            if (name_end < 0)
                break;
            auto const name = rest.subview({.offset = 0, .size = name_end});
            rest = rest.subview(name_end + 1);
            auto const value_end = rest.find('\n');
            if (value_end < 0)
                break;
            head.headers.add(name, rest.subview({.offset = 0, .size = value_end}));
            rest = rest.subview(value_end + 1);
        }

        cnet::answer(handle, {.kind = cnet::fetch_answer_kind::head, .head = cc::move(head)});
    }

    EMSCRIPTEN_KEEPALIVE void cnet_fetch_on_chunk(void* handle, cnet::byte* data, int size)
    {
        auto bytes = cc::vector<cnet::byte>();
        bytes.resize_to_defaulted(size);
        for (auto i = 0; i < size; ++i)
            bytes[i] = data[i];
        delete[] reinterpret_cast<char*>(data);

        cnet::answer(handle, {.kind = cnet::fetch_answer_kind::chunk, .bytes = cc::move(bytes)});
    }

    EMSCRIPTEN_KEEPALIVE void cnet_fetch_on_done(void* handle)
    {
        cnet::answer(handle, {.kind = cnet::fetch_answer_kind::done});
        cnet::release_js_handle(handle);
    }

    EMSCRIPTEN_KEEPALIVE void cnet_fetch_on_failed(void* handle, char* message)
    {
        cnet::answer(handle, {.kind = cnet::fetch_answer_kind::failed, .failure = cnet::take_c_string(message)});
        cnet::release_js_handle(handle);
    }
}

EM_JS_DEPS(cnet_fetch, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

// The live requests, by handle.
// A request `cnet_fetch_forget` removed answers nothing more, whatever its promises still do.
EM_JS(
    void,
    cnet_fetch_start,
    (void* handle, char const* method, char const* url, char const* headers, void const* body, int body_size),
    {
        var toC = function(s)
        {
            s = String(s);
            var n = lengthBytesUTF8(s) + 1;
            var p = _cnet_fetch_alloc(n);
            stringToUTF8(s, p, n);
            return p;
        };
        var table = Module["cnetFetches"] || (Module["cnetFetches"] = new Map());
        var entry = {controller : new AbortController(), reader : null};
        table.set(handle, entry);
        var fail = function(e)
        {
            if (table.get(handle) != entry)
                return;
            table.delete(handle);
            _cnet_fetch_on_failed(handle, toC(String((e && e.message) || e)));
        };
        try
        {
            var init
                = {method : UTF8ToString(method), redirect : "follow", signal : entry.controller.signal, headers : []};
            var lines = UTF8ToString(headers).split("\n");
            for (var i = 0; i + 1 < lines.length; i += 2)
                init.headers.push([ lines[i], lines[i + 1] ]);
            if (body_size > 0)
                init.body = HEAPU8.slice(body, body + body_size);
            fetch(UTF8ToString(url), init)
                .then(
                    function(response) {
                        if (table.get(handle) != entry)
                            return;
                        entry.reader = response.body ? response.body.getReader() : null;
                        var h = "";
                        response.headers.forEach(function(value, name) { h += name + "\n" + value + "\n"; });
                        _cnet_fetch_on_head(handle, response.status, toC(response.statusText), toC(h));
                    },
                    fail);
        }
        catch (e)
        {
            fail(e);
        }
    });

EM_JS(void, cnet_fetch_read, (void* handle), {
    var toC = function(s)
    {
        s = String(s);
        var n = lengthBytesUTF8(s) + 1;
        var p = _cnet_fetch_alloc(n);
        stringToUTF8(s, p, n);
        return p;
    };
    var table = Module["cnetFetches"];
    var entry = table && table.get(handle);
    if (!entry)
        return;
    if (!entry.reader)
    {
        table.delete(handle);
        _cnet_fetch_on_done(handle);
        return;
    }
    entry.reader.read().then(
        function(r) {
            if (table.get(handle) != entry)
                return;
            if (r.done)
            {
                table.delete(handle);
                _cnet_fetch_on_done(handle);
                return;
            }
            var size = r.value.length;
            var data = _cnet_fetch_alloc(size);
            HEAPU8.set(r.value, data);
            _cnet_fetch_on_chunk(handle, data, size);
        },
        function(e) {
            if (table.get(handle) != entry)
                return;
            table.delete(handle);
            _cnet_fetch_on_failed(handle, toC(String((e && e.message) || e)));
        });
});

EM_JS(int, cnet_fetch_forget, (void* handle), {
    var table = Module["cnetFetches"];
    var entry = table && table.get(handle);
    if (!entry)
        return 0;
    table.delete(handle);
    try
    {
        entry.controller.abort();
    }
    catch (e)
    {
    }
    return 1;
});

EM_JS(char*, cnet_document_url, (), {
    var toC = function(s)
    {
        s = String(s);
        var n = lengthBytesUTF8(s) + 1;
        var p = _cnet_fetch_alloc(n);
        stringToUTF8(s, p, n);
        return p;
    };
    if (typeof location == "undefined" || !location.href)
        return 0;
    return toC(String(location.href));
});

#endif

namespace cnet
{
namespace
{
#if defined(__EMSCRIPTEN__)

using main_call = void (*)(cc::shared_ptr<fetch_exchange> const&);

/// Run `fn` on the main runtime thread: in place there, posted from anywhere else.
/// The posted call holds a reference of its own, so the exchange outlives it whatever settles meanwhile.
void on_main(main_call fn, cc::shared_ptr<fetch_exchange> const& ex)
{
    struct call
    {
        main_call fn = nullptr;
        cc::shared_ptr<fetch_exchange> ex;
    };

    auto* const posted = new call{.fn = fn, .ex = ex};
    auto* const run = +[](void* arg)
    {
        auto* const c = static_cast<call*>(arg);
        c->fn(c->ex);
        delete c;
    };

#if defined(__EMSCRIPTEN_PTHREADS__)
    if (!emscripten_is_main_runtime_thread())
    {
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, run, posted);
        return;
    }
#endif
    run(posted);
}

void start_on_main(cc::shared_ptr<fetch_exchange> const& keeper)
{
    auto& ex = *keeper;
    if (ex.forgotten)
        return;

    auto headers = cc::string();
    for (auto const& h : ex.request.headers.entries())
    {
        headers += h.name;
        headers += '\n';
        headers += h.value;
        headers += '\n';
    }

    auto method = cc::string(to_string(ex.request.method));
    auto url = cc::string(ex.request.target.url.text());
    auto const& body = ex.request.body;

    ex.js_handle = new cc::shared_ptr<fetch_exchange>(keeper);
    cnet_fetch_start(ex.js_handle, method.c_str_materialize(), url.c_str_materialize(), headers.c_str_materialize(),
                     body.data(), int(body.size()));
}

void read_on_main(cc::shared_ptr<fetch_exchange> const& keeper)
{
    if (!keeper->forgotten && keeper->js_handle != nullptr)
        cnet_fetch_read(keeper->js_handle);
}

void forget_on_main(cc::shared_ptr<fetch_exchange> const& keeper)
{
    keeper->forgotten = true;
    if (keeper->js_handle != nullptr && cnet_fetch_forget(keeper->js_handle) != 0)
        release_js_handle(keeper->js_handle);
}

void step(cc::shared_ptr<fetch_exchange> const& ex);

void settle(cc::shared_ptr<fetch_exchange> const& ex, cc::optional<error> failure)
{
    if (ex->settled)
        return;
    ex->settled = true;

    ex->gate->state.lock(
        [](impl::body_gate::data& d)
        {
            d.io = nullptr;
            d.waiting = nullptr;
            d.resume_pending = false;
        });
    on_main(forget_on_main, ex);

    if (failure.has_value())
        ex->promise->push_error(to_async_error(cc::move(failure.value())));
    else
        ex->promise->push_value(cc::move(ex->head));
}

/// Wait for the next wake-up of the gate, bounded by the request's budget and ended by its token.
void wait(cc::shared_ptr<fetch_exchange> const& ex)
{
    struct wake_op final : impl::io_operation
    {
        cc::unique_ptr<wake_op> self;
        impl::cancel_registration registration;
        cc::shared_ptr<fetch_exchange> ex;

        void on_complete(cc::optional<error> failure) override
        {
            auto const keep_alive_until_return = cc::move(self);
            registration.detach();

            ex->gate->state.lock(
                [this](impl::body_gate::data& d)
                {
                    if (d.waiting == this)
                        d.waiting = nullptr;
                });

            if (failure.has_value())
            {
                settle(ex, cc::move(failure));
                return;
            }

            step(ex);
        }
    };

    auto op = cc::make_unique<wake_op>();
    op->kind = impl::io_op_kind::manual;
    op->deadline_ns = ex->deadline_ns;
    op->ex = ex;

    auto* const raw = op.get();
    raw->self = cc::move(op);

    // Submitted BEFORE the operation is published, so a wake-up can never be posted ahead of what it wakes.
    auto in_flight = ex->io.submit(raw);
    if (in_flight.io() == nullptr)
        return; // the io_system was stopping, and `submit` already answered the operation

    auto const already_woken = ex->gate->state.lock(
        [raw](impl::body_gate::data& d)
        {
            d.waiting = raw;
            auto const pending = d.resume_pending;
            d.resume_pending = false;
            return pending;
        });

    if (already_woken)
        ex->io.signal(raw);

    raw->registration.attach(cc::move(in_flight), ex->token);
}

void read_next(cc::shared_ptr<fetch_exchange> const& ex)
{
    wait(ex);
    on_main(read_on_main, ex);
}

/// Offer the sink what it has not taken yet, and read on once it has taken all of it.
void offer(cc::shared_ptr<fetch_exchange> const& ex)
{
    auto const rest = cc::span<byte const>(ex->unconsumed).subspan(ex->consumed);
    auto const taken = rest.empty() ? isize(0) : ex->sink(rest, resume_body(ex->gate));
    CC_ASSERT(taken >= 0 && taken <= rest.size(), "a body sink takes between none and all of what it is offered");
    ex->consumed += taken;

    if (ex->consumed < ex->unconsumed.size())
    {
        wait(ex); // the sink's resume is what wakes this
        return;
    }

    ex->unconsumed.clear();
    ex->consumed = 0;
    read_next(ex);
}

void step(cc::shared_ptr<fetch_exchange> const& ex)
{
    if (ex->settled)
        return;

    if (!ex->unconsumed.empty())
    {
        offer(ex);
        return;
    }

    auto a = ex->answer.lock([](fetch_answer& slot) { return cc::exchange(slot, fetch_answer{}); });
    switch (a.kind)
    {
    case fetch_answer_kind::none:
        wait(ex); // a resume the sink sent while nothing was paused
        return;

    case fetch_answer_kind::head:
    {
        ex->head = cc::move(a.head);
        auto const declared = ex->head.content_length();
        if (declared.has_value() && declared.value() > ex->max_body_bytes)
        {
            settle(ex, error{.code = error_code::body_too_large,
                             .native_code = 0,
                             .message = cc::format("the response declares {} body bytes, more than the {} allowed",
                                                   declared.value(), ex->max_body_bytes)});
            return;
        }
        read_next(ex);
        return;
    }

    case fetch_answer_kind::chunk:
        ex->body_bytes += a.bytes.size();
        if (ex->body_bytes > ex->max_body_bytes)
        {
            settle(ex,
                   error{.code = error_code::body_too_large,
                         .native_code = 0,
                         .message = cc::format("the response body exceeds the {} bytes allowed", ex->max_body_bytes)});
            return;
        }
        ex->unconsumed = cc::move(a.bytes);
        ex->consumed = 0;
        offer(ex);
        return;

    case fetch_answer_kind::done:
        settle(ex, {});
        return;

    case fetch_answer_kind::failed:
        // A browser says nothing more about a failed fetch than that it failed: a refused connection, a CORS
        // refusal and a name that does not resolve are one TypeError, on purpose.
        settle(
            ex,
            error{.code = error_code::unknown, .native_code = 0, .message = cc::format("fetch failed: {}", a.failure)});
        return;
    }
}
#endif
} // namespace

bool fetch_http_client::is_supported()
{
#if defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}

cc::optional<cc::string> fetch_http_client::document_url()
{
#if defined(__EMSCRIPTEN__)
    auto* const text = cnet_document_url();
    if (text == nullptr)
        return {};
    return take_c_string(text);
#else
    return {};
#endif
}

cc::shared_async<http_response_head> fetch_http_client::send_streaming(http_request request,
                                                                       body_sink sink,
                                                                       request_options const& options,
                                                                       cancel_token const& token)
{
#if defined(__EMSCRIPTEN__)
    if (!request.target.url.view().is_absolute())
        return impl::failed_async<http_response_head>(
            {.code = error_code::invalid_argument, .native_code = 0, .message = cc::string("the request has no URL")});

    if (!options.follow_redirects)
        return impl::failed_async<http_response_head>({.code = error_code::level_not_supported,
                                                       .native_code = 0,
                                                       .message = cc::string("a browser follows redirects itself: "
                                                                             "follow_redirects = false needs "
                                                                             "http_level::client")});

    // The headers cross to JavaScript one per line, and a newline in one is refused where it would do damage anyway.
    for (auto const& h : request.headers.entries())
        if (cc::string_view(h.name).contains('\n') || cc::string_view(h.name).contains('\r')
            || cc::string_view(h.value).contains('\n') || cc::string_view(h.value).contains('\r'))
            return impl::failed_async<http_response_head>(
                {.code = error_code::invalid_argument,
                 .native_code = 0,
                 .message = cc::format("header '{}' holds a line break", h.name)});

    if (token.is_cancelled())
        return impl::failed_async<http_response_head>(
            {.code = error_code::cancelled, .native_code = 0, .message = cc::string("the request was cancelled")});

    auto ex = cc::make_shared<fetch_exchange>(_io);
    ex->request = cc::move(request);
    ex->sink = cc::move(sink);
    ex->token = token;
    ex->promise = cc::make_async_manual<http_response_head>();
    ex->deadline_ns = deadline_to_absolute(_io, options.timeout);
    ex->max_body_bytes = impl::resolve_body_cap(options.max_body_bytes);
    ex->gate = new impl::body_gate();
    ex->gate->state.lock([&](impl::body_gate::data& d) { d.io = &_io; });

    auto promise = ex->promise;
    wait(ex);
    on_main(start_on_main, ex);
    return promise;
#else
    (void)request;
    (void)sink;
    (void)options;
    (void)token;
    return impl::failed_async<http_response_head>(
        {.code = error_code::unsupported, .native_code = 0, .message = cc::string("fetch exists only on wasm")});
#endif
}
} // namespace cnet
