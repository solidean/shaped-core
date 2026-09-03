#include "tcp.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-net/fwd.hh>
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

    /// Kept so the socket cannot be closed -- or its handle reused by a different socket -- while the reactor is
    /// still watching it.
    cc::shared_ptr<impl::socket_holder> socket_owner;

    /// What a connect or accept produces, held here until it is pushed.
    cc::shared_ptr<tcp_connection> pending;

    void on_complete(cc::optional<error> failure) override
    {
        // Take ownership of ourselves first: everything below may run arbitrary continuations, and the last thing
        // this function does must be to die.
        auto const keep_alive_until_return = cc::move(self);

        if (failure.has_value())
        {
            promise->push_error(cc::async_error::make_error(cc::any_error(cc::move(failure.value()))));
            return;
        }
        static_cast<Self*>(this)->deliver();
    }
};

/// The absolute reading a deadline turns into, or 0 for none.
[[nodiscard]] i64 deadline_to_ns(io_system& io, deadline d)
{
    if (!d.is_finite())
        return 0;
    return io.time_source().now_ns() + d.timeout_ms * 1000 * 1000;
}

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
        promise->push_value(cc::make_shared<tcp_connection>(*io, cc::make_shared<impl::socket_holder>(s),
                                                            local.has_value() ? local.value() : endpoint(),
                                                            peer.has_value() ? peer.value() : endpoint()));
    }
};

/// Build the operation, wire its promise, and hand it to the reactor.
template <class Op, class T>
[[nodiscard]] cc::shared_async<T> launch(io_system& io, cc::unique_ptr<Op> op)
{
    auto promise = cc::make_async_manual<T>();
    op->promise = promise;

    auto* const raw = op.get();
    raw->self = cc::move(op);
    io.submit(raw);
    return promise;
}

/// A promise that is already broken, for a failure discovered before anything reached the reactor.
template <class T>
[[nodiscard]] cc::shared_async<T> failed_async(error e)
{
    auto promise = cc::make_async_manual<T>();
    promise->push_error(cc::async_error::make_error(cc::any_error(cc::move(e))));
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
} // namespace

// ---- connection ----------------------------------------------------------------------------------------

tcp_connection::tcp_connection(io_system& io,
                               cc::shared_ptr<impl::socket_holder> s,
                               endpoint local_endpoint,
                               endpoint peer_endpoint)
  : _io(io), _socket(cc::move(s)), _local(local_endpoint), _peer(peer_endpoint)
{
}

tcp_connection::~tcp_connection() = default;

bool tcp_connection::is_open() const
{
    return _socket.is_valid();
}

void tcp_connection::close()
{
    // Dropping the reference rather than closing the handle: an operation the reactor is still watching holds one
    // too, and the socket goes away once the last of them is done with it.
    _socket = {};
}

cc::shared_async<isize> tcp_connection::receive(cc::span<byte> buffer, deadline d)
{
    if (!is_open())
        return failed_async<isize>(
            {.code = error_code::connection_closed, .native_code = 0, .message = cc::string("the connection is closed")});

    auto op = cc::make_unique<receive_operation>();
    op->kind = impl::io_op_kind::receive;
    op->socket = _socket->handle;
    op->buffer = buffer.data();
    op->buffer_size = buffer.size();
    op->deadline_ns = deadline_to_ns(_io, d);

    // The reactor watches this socket by handle, so the operation keeps it alive for as long as it runs.
    op->socket_owner = _socket;

    return launch<receive_operation, isize>(_io, cc::move(op));
}

cc::shared_async<cc::unit> tcp_connection::send(cc::span<byte const> bytes, deadline d)
{
    if (!is_open())
        return failed_async<cc::unit>(
            {.code = error_code::connection_closed, .native_code = 0, .message = cc::string("the connection is closed")});

    auto op = cc::make_unique<send_operation>();
    op->kind = impl::io_op_kind::send;
    op->socket = _socket->handle;
    op->buffer = const_cast<byte*>(bytes.data());
    op->buffer_size = bytes.size();
    op->deadline_ns = deadline_to_ns(_io, d);
    op->socket_owner = _socket;

    return launch<send_operation, cc::unit>(_io, cc::move(op));
}

// ---- listener ------------------------------------------------------------------------------------------

tcp_listener::tcp_listener(io_system& io, cc::shared_ptr<impl::socket_holder> s, endpoint local_endpoint)
  : _io(io), _socket(cc::move(s)), _local(local_endpoint)
{
}

tcp_listener::~tcp_listener() = default;

cc::result<cc::unique_ptr<tcp_listener>, error> tcp_listener::try_create(io_system& io,
                                                                         endpoint const& where,
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

    auto listener = cc::make_unique<tcp_listener>(io, cc::make_shared<impl::socket_holder>(s), local.value());
    listener->_options = options.socket;

    CC_LOG_TRACE("listening on {}", local.value());
    return listener;
}

cc::unique_ptr<tcp_listener> tcp_listener::create(io_system& io, endpoint const& where, tcp_listen_options const& options)
{
    return try_create(io, where, options).or_throw();
}

cc::shared_async<cc::shared_ptr<tcp_connection>> tcp_listener::accept(deadline d)
{
    auto op = cc::make_unique<accept_operation>();
    op->kind = impl::io_op_kind::accept;
    op->socket = _socket->handle;
    op->deadline_ns = deadline_to_ns(_io, d);
    op->io = &_io;
    op->options = _options;
    op->socket_owner = _socket;

    return launch<accept_operation, cc::shared_ptr<tcp_connection>>(_io, cc::move(op));
}

// ---- connect -------------------------------------------------------------------------------------------

cc::shared_async<cc::shared_ptr<tcp_connection>> tcp_connect(io_system& io,
                                                             endpoint const& where,
                                                             deadline d,
                                                             tcp_options const& options)
{
    using handle = cc::shared_ptr<tcp_connection>;

    if (!impl::sockets_are_supported())
        return failed_async<handle>(unsupported_here("connecting"));
    if (!where.address.is_valid())
        return failed_async<handle>({.code = error_code::invalid_argument,
                                     .native_code = 0,
                                     .message = cc::string("connect: the endpoint has no address")});

    auto created = impl::create_tcp_socket(where.address.family());
    if (created.has_error())
        return failed_async<handle>(cc::move(created).error());

    auto const s = created.value();
    apply_options(s, where.address.family(), options);

    auto holder = cc::make_shared<impl::socket_holder>(s);

    auto op = cc::make_unique<connect_operation>();
    op->kind = impl::io_op_kind::connect;
    op->socket = s;
    op->peer = where;
    op->deadline_ns = deadline_to_ns(io, d);
    op->socket_owner = holder;

    // The connection object exists before the connection does, so a failure closes the socket by dropping this
    // rather than by remembering to.
    op->pending = cc::make_shared<tcp_connection>(io, holder, endpoint(), where);

    return launch<connect_operation, handle>(io, cc::move(op));
}
} // namespace cnet
