#include "http_client.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/http/impl/http1.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/transport/connect.hh>

// One request, from a URL to a parsed response.
//
// THE WHOLE THING IS ONE STATE MACHINE, held by shared_ptr and stepped by completions.
// connect -> maybe TLS -> write the head -> write the body -> read until the message ends -> maybe start again at a
// redirect.
// Nothing blocks and nothing polls: each step hands the next one to `when_ready`, which fires on the reactor thread.
//
// ONE BUDGET FOR ALL OF IT.
// The deadline becomes an absolute reading once, and every step is given what is left of it -- so a request to a
// slow host cannot spend its timeout four times over by spending it per phase.

namespace cnet
{
namespace
{
/// How much is read from the connection at a time.
constexpr isize k_read_chunk = 16 * 1024;

/// The default body cap, which is a platform question rather than a preference.
[[nodiscard]] i64 default_max_body_bytes()
{
    // A 32-bit address space cannot hold what a 64-bit one can, and a browser fails well below its own 4 GiB limit.
    // The number matters less than the fact that exceeding it is a clean error rather than an abort.
    if constexpr (sizeof(void*) >= 8)
        return i64(3) * 1024 * 1024 * 1024;
    else
        return i64(256) * 1024 * 1024;
}

/// What `request_options::max_body_bytes` means, once: a cap, the platform default, or no cap at all.
[[nodiscard]] i64 resolve_body_cap(i64 requested)
{
    if (requested == request_options::unlimited)
        return i64(0x7FFF'FFFF'FFFF'FFFF);
    return requested > 0 ? requested : default_max_body_bytes();
}

} // namespace

/// What a `cnet::resume_body` holds.
///
/// Shared with the exchange rather than owned by it, so a sink that kept its `resume_body` past the end of the
/// request finds a gate that has been disarmed rather than a freed exchange.
struct impl::body_gate
{
    cc::atomic<i32> references = 1;

    struct data
    {
        /// Null once the request has settled, which is what makes a late `resume()` harmless.
        io_system* io = nullptr;

        /// The manual operation the paused request waits on, or null when it is not paused.
        impl::io_operation* waiting = nullptr;

        /// A resume that arrived before the pause had published its operation.
        ///
        /// The sink returns on the reactor thread and may hand its `resume_body` to a consumer on another one, so
        /// the two genuinely race -- and a signal posted ahead of the operation it means to wake is dropped.
        /// The pause re-reads this after publishing, which is the same shape `cancel_registration::attach` uses.
        bool resume_pending = false;
    };

    cc::mutex<data> state;
};

namespace impl
{
void body_gate_retain(body_gate* g)
{
    if (g != nullptr)
        g->references.fetch_add(1);
}

void body_gate_release(body_gate* g)
{
    if (g != nullptr && g->references.fetch_sub(1) == 1)
        delete g;
}

void body_gate_resume(body_gate* g)
{
    if (g == nullptr)
        return;

    struct wake
    {
        io_system* io = nullptr;
        io_operation* op = nullptr;
    };

    auto const w = g->state.lock(
        [](body_gate::data& d) -> wake
        {
            if (d.io == nullptr)
                return {}; // the request is over; a sink holding this past the end is not an error

            if (d.waiting == nullptr)
            {
                // The pause has not published its operation yet, so leave the answer where it will look.
                d.resume_pending = true;
                return {};
            }

            auto const out = wake{.io = d.io, .op = d.waiting};
            d.waiting = nullptr;
            return out;
        });

    if (w.op != nullptr)
        w.io->signal(w.op);
}
} // namespace impl

namespace
{
/// Everything one request needs, from the first connect to the last byte.
struct exchange
{
    transport& t;
    resolver& r;

    http_request request;
    request_options options;
    cancel_token token;
    body_sink sink;

    cc::shared_async<http_response_head> promise;

    /// The whole request's budget, as one absolute reading.
    i64 deadline_ns = 0;
    i32 redirects_left = 0;
    i64 max_body_bytes = 0;

    connection_pool* pool = nullptr;
    cc::shared_ptr<stream_connection> connection;

    /// True while this attempt is running on a connection that was already open.
    ///
    /// It is the one thing that decides whether a failure is worth retrying: a pooled connection can be dead
    /// without anybody knowing, and a fresh one that fails failed for a real reason.
    bool connection_was_pooled = false;
    bool retried_stale = false;

    impl::http1_parser parser;

    /// The serialized head, which must outlive the send that carries it.
    cc::string head_bytes;

    cc::vector<byte> read_buffer;

    /// Bytes that arrived and have not been parsed yet, because the parser stopped at the end of the head.
    cc::vector<byte> unparsed;

    /// True while a redirect's body is being read and thrown away.
    bool discarding_body = false;

    /// Shared with every `resume_body` this request handed to its sink; released in the destructor.
    impl::body_gate* gate = nullptr;

    /// True while the sink has taken nothing and nothing more is being read.
    bool paused = false;

    bool settled = false;

    exchange(transport& transport_ref, resolver& resolver_ref) : t(transport_ref), r(resolver_ref) {}
    exchange(exchange const&) = delete;
    exchange& operator=(exchange const&) = delete;
    ~exchange() { impl::body_gate_release(gate); }

    /// What is left of the budget, as a deadline the next step can be given.
    [[nodiscard]] deadline remaining() const
    {
        if (deadline_ns <= 0)
            return deadline::never();

        auto const left_ms = (deadline_ns - r.io().time_source().now_ns()) / (1000 * 1000);
        return deadline::after_ms(left_ms > 0 ? left_ms : 0);
    }
};

void start_attempt(cc::shared_ptr<exchange> const& ex);
void release_connection(cc::shared_ptr<exchange> const& ex, bool reusable);
void write_head(cc::shared_ptr<exchange> const& ex);

void close_gate(cc::shared_ptr<exchange> const& ex);

void fail(cc::shared_ptr<exchange> const& ex, error e)
{
    if (ex->settled)
        return;
    ex->settled = true;

    close_gate(ex);
    release_connection(ex, false);
    ex->promise->push_error(to_async_error(cc::move(e)));
}

/// Whether this failure is the one pooling creates, and can be answered by trying again.
///
/// Only before a single response byte has arrived: after that the request reached the server, and repeating it would
/// be a second request rather than a retry.
[[nodiscard]] bool can_retry_stale(cc::shared_ptr<exchange> const& ex)
{
    // Not while the io_system is stopping: a retry starts a fresh connect on `ex->t`, which is the caller's
    // transport and may already be gone by the time teardown settles this request.
    return ex->connection_was_pooled && !ex->retried_stale && !ex->parser.head_complete()
        && ex->parser.body_bytes() == 0 && !ex->token.is_cancelled() && !ex->t.io().is_stopping();
}

void retry_on_fresh_connection(cc::shared_ptr<exchange> const& ex)
{
    CC_LOG_TRACE("the pooled connection to {} was dead; trying a fresh one", ex->request.target.origin());

    ex->retried_stale = true;
    release_connection(ex, false);
    ex->unparsed.clear();
    ex->discarding_body = false;
    start_attempt(ex);
}

/// Fail with whatever a step underneath reported, keeping its message and its cancellation.
template <class T>
void fail_from(cc::shared_ptr<exchange> const& ex, cc::shared_async<T> const& failed)
{
    if (ex->settled)
        return;

    // A pooled connection that failed before the response started is the case pooling itself created, so it is the
    // one this answers by trying again rather than by failing.
    if (can_retry_stale(ex))
    {
        retry_on_fresh_connection(ex);
        return;
    }

    ex->settled = true;
    close_gate(ex);
    release_connection(ex, false);
    ex->promise->push_error(failed->propagate_error());
}

void release_connection(cc::shared_ptr<exchange> const& ex, bool reusable)
{
    if (!ex->connection.is_valid())
        return;

    auto connection = cc::move(ex->connection);
    ex->connection = {};

    if (ex->pool == nullptr)
    {
        connection->close();
        return;
    }

    ex->pool->give_back(ex->request.target.origin(), cc::move(connection), reusable);
}

void succeed(cc::shared_ptr<exchange> const& ex)
{
    if (ex->settled)
        return;
    ex->settled = true;

    close_gate(ex);

    // Leftover bytes mean the server said something this client did not ask for, and a stream nobody understands is
    // not one to hand to the next request.
    auto const clean = ex->parser.can_reuse_connection() && ex->unparsed.empty();
    release_connection(ex, clean);

    ex->promise->push_value(cc::move(ex->parser.response()));
}

/// What a redirect turns this request into, or nothing if it is not one to follow.
[[nodiscard]] cc::optional<http_target> redirect_target(cc::shared_ptr<exchange> const& ex)
{
    if (!ex->options.follow_redirects || ex->redirects_left <= 0)
        return {};

    auto const& head = ex->parser.response();
    if (!head.is_redirect() || head.status == 304)
        return {};

    auto const location = head.headers.get("Location");
    if (!location.has_value() || location.value().empty())
        return {};

    // Relative or absolute, resolved against the URL that was actually requested -- which is why the target keeps
    // its cc::uri rather than only the pieces a request line needs.
    auto resolved = ex->request.target.url.resolve(location.value());
    if (!resolved.has_value())
        return {};

    auto next = http_target::from_uri(cc::move(resolved.value()));
    if (next.has_error())
        return {};

    return cc::move(next).value();
}

void follow_redirect(cc::shared_ptr<exchange> const& ex, http_target next)
{
    auto const status = ex->parser.response().status;
    auto const same_origin = next.origin() == ex->request.target.origin();

    // 301, 302 and 303 turn everything but HEAD into a GET without a body, which is what every client does and what
    // servers assume; 307 and 308 exist precisely to say "keep the method and the body".
    if (status == 301 || status == 302 || status == 303)
        if (ex->request.method != http_method::head)
        {
            ex->request.method = http_method::get;
            ex->request.body = {};
            ex->request.headers.remove("Content-Length");
            ex->request.headers.remove("Content-Type");
        }

    // Credentials do not follow a redirect off the origin they were given to, which is how a token ends up in
    // somebody else's logs.
    if (!same_origin)
    {
        ex->request.headers.remove("Authorization");
        ex->request.headers.remove("Cookie");
    }

    CC_LOG_TRACE("redirect {} to {}", status, next.origin());

    ex->request.target = cc::move(next);
    --ex->redirects_left;

    // The redirect's own response finished cleanly, so its connection is worth keeping -- even though the next
    // request may well go somewhere else.
    release_connection(ex, ex->parser.can_reuse_connection() && ex->unparsed.empty());
    ex->unparsed.clear();
    ex->discarding_body = false;
    ex->retried_stale = false;

    start_attempt(ex);
}

/// Called when the parser says the message is done.
void on_message_complete(cc::shared_ptr<exchange> const& ex)
{
    if (auto next = redirect_target(ex); next.has_value())
    {
        follow_redirect(ex, cc::move(next.value()));
        return;
    }
    succeed(ex);
}

void read_more(cc::shared_ptr<exchange> const& ex);

/// Feed everything that has arrived, and act on what the parser says.
void pause_for_sink(cc::shared_ptr<exchange> const& ex);

void parse_available(cc::shared_ptr<exchange> const& ex)
{
    auto cursor = isize(0);
    auto over_cap = false;

    while (cursor < ex->unparsed.size() && !ex->parser.message_complete())
    {
        auto const before_head = !ex->parser.head_complete();

        auto const fed
            = ex->parser.feed(cc::span<byte const>(ex->unparsed.data() + cursor, ex->unparsed.size() - cursor),
                              [&ex, &over_cap](cc::span<byte const> chunk) -> isize
                              {
                                  if (ex->discarding_body)
                                      return chunk.size();

                                  // The parser's count is what ACTUALLY reached the sink; counting what was offered
                                  // charges the same bytes again every time a pushing-back sink is re-offered them.
                                  if (ex->parser.body_bytes() + chunk.size() > ex->max_body_bytes)
                                  {
                                      over_cap = true;
                                      return 0; // refused below, where it can fail the request rather than stall it
                                  }

                                  return ex->sink(chunk, resume_body(ex->gate));
                              });

        if (fed.has_error())
        {
            fail(ex, cc::move(fed).error());
            return;
        }

        cursor += fed.value();

        // The parser stops when the head is complete, which is where a redirect is decided -- and where the body
        // cap has to be checked before anything is buffered.
        if (before_head && ex->parser.head_complete())
        {
            ex->discarding_body = redirect_target(ex).has_value();

            if (auto const declared = ex->parser.response().content_length();
                declared.has_value() && !ex->discarding_body && declared.value() > ex->max_body_bytes)
            {
                fail(ex, {.code = error_code::body_too_large,
                          .native_code = 0,
                          .message = cc::format("the response declares {} bytes, over the {} this request allows",
                                                declared.value(), ex->max_body_bytes)});
                return;
            }
            continue;
        }

        if (fed.value() == 0)
        {
            // The sink took nothing and the parser made no progress: either the body is over the cap, or the sink is
            // pushing back and this request stops reading until it says otherwise.
            if (over_cap)
            {
                fail(ex, {.code = error_code::body_too_large,
                          .native_code = 0,
                          .message = cc::format("the response body is over the {} bytes this request allows",
                                                ex->max_body_bytes)});
                return;
            }
            break;
        }
    }

    // Keep whatever was not consumed; it is the start of the next parse.
    if (cursor > 0)
    {
        auto rest = cc::vector<byte>();
        for (auto i = cursor; i < ex->unparsed.size(); ++i)
            rest.push_back(ex->unparsed[i]);
        ex->unparsed = cc::move(rest);
    }

    if (ex->parser.message_complete())
    {
        on_message_complete(ex);
        return;
    }

    // A sink that took nothing leaves those bytes here, and reading more would put the pressure in this vector
    // instead of in TCP's receive window -- which is the whole point of the return value.
    // So the request stops until `resume_body::resume` says the sink can take them.
    if (!ex->unparsed.empty() && ex->parser.head_complete() && !ex->discarding_body && !ex->paused)
    {
        pause_for_sink(ex);
        return;
    }

    read_more(ex);
}

/// Stop reading until the sink asks for the rest, and fail on the request's own deadline if it never does.
///
/// A `manual` operation rather than a flag: the request's budget then bounds a stalled consumer exactly as it bounds
/// a slow server, and the caller's token ends it the same way.
void pause_for_sink(cc::shared_ptr<exchange> const& ex)
{
    struct resume_op final : impl::io_operation
    {
        cc::unique_ptr<resume_op> self;
        impl::cancel_registration registration;
        cc::shared_ptr<exchange> ex;

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
            ex->paused = false;

            if (failure.has_value())
            {
                fail(ex, cc::move(failure.value()));
                return;
            }

            parse_available(ex);
        }
    };

    auto op = cc::make_unique<resume_op>();
    op->kind = impl::io_op_kind::manual;
    op->deadline_ns = ex->deadline_ns;
    op->ex = ex;

    auto* const raw = op.get();
    raw->self = cc::move(op);
    ex->paused = true;

    // Submitted BEFORE the operation is published, so a resume can never be posted ahead of what it wakes.
    ex->r.io().submit(raw);

    auto const already_resumed = ex->gate->state.lock(
        [raw](impl::body_gate::data& d)
        {
            d.waiting = raw;
            auto const pending = d.resume_pending;
            d.resume_pending = false;
            return pending;
        });

    if (already_resumed)
        ex->r.io().signal(raw);

    raw->registration.attach(ex->token, ex->r.io(), raw);
}

void read_more(cc::shared_ptr<exchange> const& ex)
{
    auto buffer = cc::span<byte>(ex->read_buffer.data(), ex->read_buffer.size());

    impl::when_ready(ex->connection->receive(buffer, ex->remaining(), ex->token),
                     [ex](cc::shared_async<isize> const& received)
                     {
                         if (received->has_error())
                         {
                             // A close is the end of the message for the one framing that has no other end, and a
                             // truncation for every other -- the parser is what knows which.
                             if (auto ended = ex->parser.notify_end_of_stream(); ended.has_value())
                             {
                                 on_message_complete(ex);
                                 return;
                             }

                             // A pooled connection the server had already closed looks exactly like this, and is
                             // the reason a first read is allowed to fail without failing the request.
                             if (can_retry_stale(ex))
                             {
                                 retry_on_fresh_connection(ex);
                                 return;
                             }
                             fail_from(ex, received);
                             return;
                         }

                         auto const n = received->value();
                         for (isize i = 0; i < n; ++i)
                             ex->unparsed.push_back(ex->read_buffer[i]);

                         parse_available(ex);
                     });
}

void write_body(cc::shared_ptr<exchange> const& ex)
{
    if (ex->request.body.empty())
    {
        read_more(ex);
        return;
    }

    impl::when_ready(ex->connection->send(ex->request.body, ex->remaining(), ex->token),
                     [ex](cc::shared_async<cc::unit> const& sent)
                     {
                         if (sent->has_error())
                         {
                             fail_from(ex, sent);
                             return;
                         }
                         read_more(ex);
                     });
}

void write_head(cc::shared_ptr<exchange> const& ex)
{
    // Keep-alive when the connection may be reused, and `Connection: close` when it may not -- which is what keeps
    // a server from holding a connection open for a second request that is never coming.
    auto head = impl::write_request_head(ex->request, ex->pool != nullptr);
    if (head.has_error())
    {
        fail(ex, cc::move(head).error());
        return;
    }

    ex->head_bytes = cc::move(head).value();

    auto const bytes = cc::span<byte const>(reinterpret_cast<byte const*>(ex->head_bytes.data()), ex->head_bytes.size());

    impl::when_ready(ex->connection->send(bytes, ex->remaining(), ex->token),
                     [ex](cc::shared_async<cc::unit> const& sent)
                     {
                         if (sent->has_error())
                         {
                             fail_from(ex, sent);
                             return;
                         }
                         write_body(ex);
                     });
}

void start_attempt(cc::shared_ptr<exchange> const& ex)
{
    auto const& target = ex->request.target;

    ex->parser.start_response(ex->request.method);
    ex->connection_was_pooled = false;

    // A pooled connection is already past the connect and the handshake, which is the whole point of keeping it.
    if (ex->pool != nullptr)
        if (auto pooled = ex->pool->try_take(target.origin()); pooled.is_valid())
        {
            ex->connection = cc::move(pooled);
            ex->connection_was_pooled = true;
            write_head(ex);
            return;
        }

    impl::when_ready(
        connect_to_host(
            ex->t, ex->r, target.host, target.port,
            {.timeout = ex->remaining(), .family = ex->options.family, .attempt_delay_ms = ex->options.attempt_delay_ms},
            ex->token),
        [ex](cc::shared_async<cc::shared_ptr<stream_connection>> const& connected)
        {
            if (connected->has_error())
            {
                fail_from(ex, connected);
                return;
            }

            ex->connection = connected->value();

            if (!ex->request.target.secure)
            {
                write_head(ex);
                return;
            }

            // The NAME, not the address: the certificate is checked against what the caller asked for.
            impl::when_ready(
                tls_connect(ex->connection, ex->request.target.host, ex->options.tls, ex->remaining(), ex->token),
                [ex](cc::shared_async<cc::shared_ptr<stream_connection>> const& secured)
                {
                    if (secured->has_error())
                    {
                        // tls_connect already closed what it was given.
                        ex->connection = {};
                        fail_from(ex, secured);
                        return;
                    }

                    ex->connection = secured->value();
                    write_head(ex);
                });
        });
}
/// Disarm the gate, so a `resume_body` the sink kept can never reach a request that is over.
void close_gate(cc::shared_ptr<exchange> const& ex)
{
    if (ex->gate == nullptr)
        return;

    ex->gate->state.lock(
        [](impl::body_gate::data& d)
        {
            d.io = nullptr;
            d.waiting = nullptr;
            d.resume_pending = false;
        });
}
} // namespace

resume_body& resume_body::operator=(resume_body const& o)
{
    if (this == &o)
        return *this;

    impl::body_gate_retain(o._gate);
    impl::body_gate_release(_gate);
    _gate = o._gate;
    return *this;
}

resume_body& resume_body::operator=(resume_body&& o) noexcept
{
    if (this == &o)
        return *this;

    impl::body_gate_release(_gate);
    _gate = o._gate;
    o._gate = nullptr;
    return *this;
}

cc::shared_async<http_response_head> native_http_client::send_streaming(http_request request,
                                                                        body_sink sink,
                                                                        request_options const& options,
                                                                        cancel_token const& token)
{
    if (!request.target.url.view().is_absolute())
        return impl::failed_async<http_response_head>(
            {.code = error_code::invalid_argument, .native_code = 0, .message = cc::string("the request has no URL")});

    auto ex = cc::make_shared<exchange>(_transport, _resolver);
    ex->request = cc::move(request);
    ex->options = options;
    ex->token = token;
    ex->sink = cc::move(sink);
    ex->promise = cc::make_async_manual<http_response_head>();
    ex->deadline_ns = deadline_to_absolute(_resolver.io(), options.timeout);
    ex->redirects_left = options.max_redirects;
    // 0 is the platform default and `request_options::unlimited` is the one value that means no cap; anything else
    // is the cap itself.
    ex->max_body_bytes = resolve_body_cap(options.max_body_bytes);
    ex->gate = new impl::body_gate();
    ex->gate->state.lock([&](impl::body_gate::data& d) { d.io = &_resolver.io(); });
    ex->pool = options.reuse_connections ? &_pool : nullptr;
    ex->read_buffer.resize_to_defaulted(k_read_chunk);

    start_attempt(ex);
    return ex->promise;
}

owned_http_client::owned_http_client(io_system& io, cc::unique_ptr<resolver> r)
  : _transport(io), _resolver(cc::move(r)), _client(_transport, *_resolver)
{
}

http_level owned_http_client::level() const
{
    return _client.level();
}

cc::shared_async<http_response_head> owned_http_client::send_streaming(http_request request,
                                                                       body_sink sink,
                                                                       request_options const& options,
                                                                       cancel_token const& token)
{
    return _client.send_streaming(cc::move(request), cc::move(sink), options, token);
}

cc::result<cc::unique_ptr<owned_http_client>, error> make_http_client(io_system& io)
{
    auto r = resolver::try_create(io);
    if (r.has_error())
        return cc::error(cc::move(r).error());

    return cc::make_unique<owned_http_client>(io, cc::move(r).value());
}

cc::shared_async<http_response> http_send(http_client& client,
                                          http_request request,
                                          request_options const& options,
                                          cancel_token const& token)
{
    // The buffered response IS a sink: this is the whole of "buffered is written over streaming".
    auto body = cc::make_shared<cc::vector<byte>>();

    auto head = client.send_streaming(
        cc::move(request),
        [body](cc::span<byte const> chunk, resume_body const&) -> isize
        {
            // Buffering takes everything every time, so it never pushes back and never needs the flow.
            for (auto const b : chunk)
                body->push_back(b);
            return chunk.size();
        },
        options, token);

    auto promise = cc::make_async_manual<http_response>();

    impl::when_ready(head,
                     [promise, body](cc::shared_async<http_response_head> const& settled)
                     {
                         if (settled->has_error())
                         {
                             promise->push_error(settled->propagate_error());
                             return;
                         }

                         promise->push_value({.head = settled->take_value(), .body = cc::move(*body)});
                     });

    return promise;
}

cc::shared_async<http_response> http_get(http_client& client,
                                         cc::string_view url,
                                         request_options const& options,
                                         cancel_token const& token)
{
    auto target = http_target::parse(url);
    if (target.has_error())
        return impl::failed_async<http_response>(cc::move(target).error());

    return http_send(client, {.method = http_method::get, .target = cc::move(target).value()}, options, token);
}
} // namespace cnet
