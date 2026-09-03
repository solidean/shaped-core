#include "tcp.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/native_socket.hh>
#include <clean-net/impl/reactor.hh>

namespace cnet
{
namespace
{
/// An operation that owns itself until it completes.
///
/// The reactor holds a bare pointer and has already forgotten the operation by the time `on_complete` runs, so
/// something has to free it, and only the operation knows when it is done.
/// `_self` is that: the handle is moved into a local at the top of `finish`, so the object outlives the body and
/// dies at its end -- the `delete this` pattern, spelled so the lifetime is visible.
/// CRTP rather than a plain base, because cc::unique_ptr has no upcast: the self-handle has to be typed as the
/// concrete operation, which only the derived type can name.
template <class Self, class T>
struct async_operation : impl::io_operation
{
    cc::shared_async<T> promise;
    cc::unique_ptr<Self> self;

    /// Taken off the token before anything else, so a cancel racing this completion finds a registration that is
    /// still alive rather than an operation that has already freed itself.
    impl::cancel_registration cancellation;

    /// Kept so the socket cannot be closed -- or its handle reused by a different socket -- while the reactor is
    /// still watching it.
    cc::shared_ptr<impl::socket_holder> socket_owner;

    /// What a connect produces, held here until it is pushed.
    cc::shared_ptr<tcp_connection> pending;

    void on_complete(cc::optional<error> failure) override
    {
        // Take ownership of ourselves first: everything below may run arbitrary continuations, and the last thing
        // this function does must be to die.
        auto const keep_alive_until_return = cc::move(self);
        cancellation.detach();

        if (failure.has_value())
        {
            promise->push_error(to_async_error(cc::move(failure.value())));
            return;
        }
        static_cast<Self*>(this)->deliver();
    }
};

/// Build the operation, wire its promise, and hand it to the reactor.
///
/// The token is attached AFTER the submit: a cancel arriving in between would otherwise be posted ahead of the
/// operation it means to cancel, and the reactor would have nothing to match it against.
template <class Op, class T>
[[nodiscard]] cc::shared_async<T> launch(io_system& io, cc::unique_ptr<Op> op, cancel_token const& token)
{
    auto promise = cc::make_async_manual<T>();
    op->promise = promise;

    auto* const raw = op.get();
    raw->self = cc::move(op);
    io.submit(raw);
    raw->cancellation.attach(token, io, raw);
    return promise;
}

/// The socket options that are worth failing a connection over, and the ones that are not.
void apply_options(impl::native_socket s, ip_family family, tcp_options const& options)
{
    // A failure here is not worth losing a working connection over: the socket is usable either way, and the cost of
    // the option not applying is latency rather than correctness.
    if (family == ip_family::v6)
        (void)impl::set_v6_only(s, options.v6_only);
    (void)impl::set_tcp_no_delay(s, options.no_delay);
}

// ---- the native backend --------------------------------------------------------------------------------

/// A connection over one of the platform's own sockets.
class native_connection final : public connection_backend
{
public:
    native_connection(io_system& io, cc::shared_ptr<impl::socket_holder> s, endpoint local_endpoint, endpoint peer_endpoint)
      : _io(io), _socket(cc::move(s)), _local(local_endpoint), _peer(peer_endpoint)
    {
    }

    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d, cancel_token const& token) override;
    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes,
                                                  deadline d,
                                                  cancel_token const& token) override;

    cc::result<cc::unit, error> shutdown_send() override
    {
        if (!is_open())
            return cc::error(error{.code = error_code::connection_closed,
                                   .native_code = 0,
                                   .message = cc::string("the connection is closed")});
        return impl::shutdown_socket_send(_socket->handle);
    }

    [[nodiscard]] endpoint local() const override { return _local; }
    [[nodiscard]] endpoint peer() const override { return _peer; }
    [[nodiscard]] bool is_open() const override { return _socket.is_valid(); }

    void close() override
    {
        // Dropping the reference rather than closing the handle: an operation the reactor is still watching holds one
        // too, and the socket goes away once the last of them is done with it.
        _socket = {};
    }

private:
    io_system& _io;
    cc::shared_ptr<impl::socket_holder> _socket;
    endpoint _local;
    endpoint _peer;
};

/// A listening socket of the platform's own.
class native_listener final : public listener_backend
{
public:
    native_listener(io_system& io, cc::shared_ptr<impl::socket_holder> s, endpoint local_endpoint, tcp_options options)
      : _io(io), _socket(cc::move(s)), _local(local_endpoint), _options(options)
    {
    }

    [[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> accept(deadline d, cancel_token const& token) override;
    [[nodiscard]] endpoint local() const override { return _local; }

private:
    io_system& _io;
    cc::shared_ptr<impl::socket_holder> _socket;
    endpoint _local;
    tcp_options _options;
};

struct receive_operation final : async_operation<receive_operation, isize>
{
    void deliver() { promise->push_value(transferred); }
};

struct send_operation final : async_operation<send_operation, cc::unit>
{
    void deliver() { promise->push_value(cc::unit{}); }
};

struct connect_operation final : async_operation<connect_operation, cc::shared_ptr<tcp_connection>>
{
    void deliver() { promise->push_value(cc::move(pending)); }
};

struct accept_operation final : async_operation<accept_operation, cc::shared_ptr<tcp_connection>>
{
    io_system* io = nullptr;
    tcp_options options;

    void deliver()
    {
        // The socket only exists once the accept succeeded, so the connection is built here rather than up front.
        auto const s = accepted;
        (void)impl::set_tcp_no_delay(s, options.no_delay);

        auto local = impl::local_endpoint(s);
        auto peer = impl::remote_endpoint(s);
        auto backend = std::make_unique<native_connection>(*io, cc::make_shared<impl::socket_holder>(s),
                                                           local.has_value() ? local.value() : endpoint(),
                                                           peer.has_value() ? peer.value() : endpoint());
        promise->push_value(cc::make_shared<tcp_connection>(cc::move(backend)));
    }
};

cc::shared_async<isize> native_connection::receive(cc::span<byte> buffer, deadline d, cancel_token const& token)
{
    if (!is_open())
        return impl::failed_async<isize>(
            {.code = error_code::connection_closed, .native_code = 0, .message = cc::string("the connection is closed")});

    auto op = cc::make_unique<receive_operation>();
    op->kind = impl::io_op_kind::receive;
    op->socket = _socket->handle;
    op->buffer = buffer.data();
    op->buffer_size = buffer.size();
    op->deadline_ns = deadline_to_absolute(_io, d);

    // The reactor watches this socket by handle, so the operation keeps it alive for as long as it runs.
    op->socket_owner = _socket;

    return launch<receive_operation, isize>(_io, cc::move(op), token);
}

cc::shared_async<cc::unit> native_connection::send(cc::span<byte const> bytes, deadline d, cancel_token const& token)
{
    if (!is_open())
        return impl::failed_async<cc::unit>(
            {.code = error_code::connection_closed, .native_code = 0, .message = cc::string("the connection is closed")});

    auto op = cc::make_unique<send_operation>();
    op->kind = impl::io_op_kind::send;
    op->socket = _socket->handle;
    op->buffer = const_cast<byte*>(bytes.data());
    op->buffer_size = bytes.size();
    op->deadline_ns = deadline_to_absolute(_io, d);
    op->socket_owner = _socket;

    return launch<send_operation, cc::unit>(_io, cc::move(op), token);
}

cc::shared_async<cc::shared_ptr<tcp_connection>> native_listener::accept(deadline d, cancel_token const& token)
{
    auto op = cc::make_unique<accept_operation>();
    op->kind = impl::io_op_kind::accept;
    op->socket = _socket->handle;
    op->deadline_ns = deadline_to_absolute(_io, d);
    op->io = &_io;
    op->options = _options;
    op->socket_owner = _socket;

    return launch<accept_operation, cc::shared_ptr<tcp_connection>>(_io, cc::move(op), token);
}
} // namespace

// ---- the handles ---------------------------------------------------------------------------------------

tcp_connection::tcp_connection(std::unique_ptr<connection_backend> backend) : _backend(cc::move(backend))
{
}

tcp_connection::~tcp_connection() = default;

cc::shared_async<isize> tcp_connection::receive(cc::span<byte> buffer, deadline d, cancel_token const& token)
{
    return _backend->receive(buffer, d, token);
}

cc::shared_async<cc::unit> tcp_connection::send(cc::span<byte const> bytes, deadline d, cancel_token const& token)
{
    return _backend->send(bytes, d, token);
}

cc::result<cc::unit, error> tcp_connection::shutdown_send()
{
    return _backend->shutdown_send();
}

endpoint tcp_connection::local() const
{
    return _backend->local();
}

endpoint tcp_connection::peer() const
{
    return _backend->peer();
}

cc::string_view tcp_connection::negotiated_alpn() const
{
    return _backend->negotiated_alpn();
}

bool tcp_connection::is_open() const
{
    return _backend->is_open();
}

void tcp_connection::close()
{
    _backend->close();
}

tcp_listener::tcp_listener(std::unique_ptr<listener_backend> backend) : _backend(cc::move(backend))
{
}

tcp_listener::~tcp_listener() = default;

cc::shared_async<cc::shared_ptr<tcp_connection>> tcp_listener::accept(deadline d, cancel_token const& token)
{
    return _backend->accept(d, token);
}

endpoint tcp_listener::local() const
{
    return _backend->local();
}

cc::result<cc::unique_ptr<tcp_listener>, error> tcp_listener::try_create(io_system& io,
                                                                         endpoint const& where,
                                                                         tcp_listen_options const& options)
{
    auto native = native_transport(io);
    return native.listen(where, options);
}

cc::result<cc::unique_ptr<tcp_listener>, error> tcp_listener::try_create(transport& t,
                                                                         endpoint const& where,
                                                                         tcp_listen_options const& options)
{
    return t.listen(where, options);
}

cc::unique_ptr<tcp_listener> tcp_listener::create(io_system& io, endpoint const& where, tcp_listen_options const& options)
{
    return try_create(io, where, options).or_throw();
}

// ---- the native transport ------------------------------------------------------------------------------

bool native_transport::is_supported() const
{
    return impl::sockets_are_supported();
}

cc::result<cc::unique_ptr<tcp_listener>, error> native_transport::listen(endpoint const& where,
                                                                         tcp_listen_options const& options)
{
    if (!impl::sockets_are_supported())
        return cc::error(unsupported_here("listening"));
    if (!where.address.is_valid())
        return cc::error(error{.code = error_code::invalid_argument,
                               .native_code = 0,
                               .message = cc::string("listen: the endpoint has no address")});

    auto created = impl::create_tcp_socket(where.address.family());
    if (created.has_error())
        return cc::error(cc::move(created).error());

    auto const s = created.value();
    apply_options(s, where.address.family(), options.socket);

    auto bound = impl::bind_socket(s, where, options.reuse_address);
    if (bound.has_error())
    {
        impl::close_socket(s);
        return cc::error(cc::move(bound).error());
    }

    auto listening = impl::listen_socket(s, options.backlog);
    if (listening.has_error())
    {
        impl::close_socket(s);
        return cc::error(cc::move(listening).error());
    }

    auto local = impl::local_endpoint(s);
    if (local.has_error())
    {
        impl::close_socket(s);
        return cc::error(cc::move(local).error());
    }

    auto backend
        = std::make_unique<native_listener>(_io, cc::make_shared<impl::socket_holder>(s), local.value(), options.socket);

    CC_LOG_TRACE("listening on {}", local.value());
    return cc::make_unique<tcp_listener>(cc::move(backend));
}

cc::shared_async<cc::shared_ptr<tcp_connection>> native_transport::connect(endpoint const& where,
                                                                           deadline d,
                                                                           tcp_options const& options,
                                                                           cancel_token const& token)
{
    using handle = cc::shared_ptr<tcp_connection>;

    if (!impl::sockets_are_supported())
        return impl::failed_async<handle>(unsupported_here("connecting"));
    if (!where.address.is_valid())
        return impl::failed_async<handle>({.code = error_code::invalid_argument,
                                           .native_code = 0,
                                           .message = cc::string("connect: the endpoint has no address")});

    auto created = impl::create_tcp_socket(where.address.family());
    if (created.has_error())
        return impl::failed_async<handle>(cc::move(created).error());

    auto const s = created.value();
    apply_options(s, where.address.family(), options);

    auto holder = cc::make_shared<impl::socket_holder>(s);

    auto op = cc::make_unique<connect_operation>();
    op->kind = impl::io_op_kind::connect;
    op->socket = s;
    op->peer = where;
    op->deadline_ns = deadline_to_absolute(_io, d);
    op->socket_owner = holder;

    // The connection object exists before the connection does, so a failure closes the socket by dropping this
    // rather than by remembering to.
    op->pending = cc::make_shared<tcp_connection>(std::make_unique<native_connection>(_io, holder, endpoint(), where));

    return launch<connect_operation, handle>(_io, cc::move(op), token);
}

// ---- connect -------------------------------------------------------------------------------------------

cc::shared_async<cc::shared_ptr<tcp_connection>> tcp_connect(io_system& io,
                                                             endpoint const& where,
                                                             deadline d,
                                                             tcp_options const& options,
                                                             cancel_token const& token)
{
    auto native = native_transport(io);
    return native.connect(where, d, options, token);
}

cc::shared_async<cc::shared_ptr<tcp_connection>> tcp_connect(transport& t,
                                                             endpoint const& where,
                                                             deadline d,
                                                             tcp_options const& options,
                                                             cancel_token const& token)
{
    return t.connect(where, d, options, token);
}
} // namespace cnet
