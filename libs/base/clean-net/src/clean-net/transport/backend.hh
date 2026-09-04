#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/address/endpoint.hh>
#include <clean-net/common/cancel.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>
#include <clean-net/impl/async_glue.hh>

/// The seam a transport is reached through, so that "a socket" is not the only thing a connection can be.
///
/// Almost every serious networking bug is a timing or failure bug -- a connection that dies mid-body, a server that
/// stalls after the headers, a response arriving one byte at a time.
/// None of those happen on loopback, so a library tested only against a healthy local server is tested against the
/// one condition that never occurs in production.
/// The seam is what makes them reproducible in a unit test, with no proxy, no traffic shaper and no privileges.
///
/// It sits at the TRANSPORT rather than only at HTTP, because a reset after the headers and a body arriving one byte
/// at a time are transport events an HTTP backend has already smoothed over.
/// The cost is that it only exists where our transport does: on wasm the backend is the browser's `fetch`, and
/// simulation there stays at the HTTP layer.
///
/// `cnet::stream_connection` and `cnet::stream_listener` are handles over these interfaces rather than over a socket, so a
/// caller writes the same code whatever is underneath.

/// One end of an established connection, whatever is carrying it.
class cnet::connection_backend
{
public:
    /// Completes on the FIRST bytes that arrive, which may be far fewer than the buffer holds.
    [[nodiscard]] virtual cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d, cancel_token const& token)
        = 0;

    /// Completes only once every byte has been handed on.
    /// `bytes` must stay alive and unmodified until it completes.
    [[nodiscard]] virtual cc::shared_async<cc::unit> send(cc::span<byte const> bytes,
                                                          deadline d,
                                                          cancel_token const& token) = 0;

    /// Say that nothing more will be sent, while staying open for what the peer still has to say.
    ///
    /// The half-close a protocol needs to mean "that was the whole request" without also refusing the answer.
    /// Fails with `unsupported` on a backend that has no such notion.
    virtual cc::result<cc::unit, error> shutdown_send() = 0;

    [[nodiscard]] virtual endpoint local() const = 0;
    [[nodiscard]] virtual endpoint peer() const = 0;

    /// The application protocol a TLS handshake settled on, empty for a connection that never had one.
    ///
    /// Defaulted rather than pure: only the TLS wrapper has an answer, and a backend that is not one should not have
    /// to say so.
    [[nodiscard]] virtual cc::string_view negotiated_alpn() const { return {}; }

    [[nodiscard]] virtual bool is_open() const = 0;

    /// Stop using this connection.
    ///
    /// Whatever is underneath goes away once no operation still refers to it, which is usually at once and is not
    /// promised.
    virtual void close() = 0;

    connection_backend() = default;
    connection_backend(connection_backend const&) = delete;
    connection_backend& operator=(connection_backend const&) = delete;
    virtual ~connection_backend() = default;
};

/// What inbound connections arrive on.
class cnet::listener_backend
{
public:
    [[nodiscard]] virtual cc::shared_async<cc::shared_ptr<stream_connection>> accept(deadline d,
                                                                                     cancel_token const& token) = 0;

    /// What the listener actually bound to, port included.
    [[nodiscard]] virtual endpoint local() const = 0;

    listener_backend() = default;
    listener_backend(listener_backend const&) = delete;
    listener_backend& operator=(listener_backend const&) = delete;
    virtual ~listener_backend() = default;
};

/// Where connections and listeners come from.
///
/// One transport is the platform's own sockets; others answer without a network at all, or misbehave on the way
/// through to another one.
/// No method here takes a default argument: a default on a virtual binds to the static type of the call, so the
/// defaults live on `cnet::tcp_connect` and `cnet::stream_listener::try_create` instead.
class cnet::transport
{
public:
    /// Whether this transport can do anything here at all.
    /// False for the native one on wasm, where the browser offers no socket of any kind.
    [[nodiscard]] virtual bool is_supported() const = 0;

    [[nodiscard]] virtual cc::shared_async<cc::shared_ptr<stream_connection>> connect(endpoint const& where,
                                                                                      deadline d,
                                                                                      tcp_options const& options,
                                                                                      cancel_token const& token) = 0;

    /// The io_system this transport submits to, and on whose clock its deadlines are measured.
    ///
    /// Named here because a connection is not the only thing built on a transport that needs one: a keepalive is a
    /// timer, and a timer belongs to the reactor rather than to the socket it is about.
    [[nodiscard]] virtual io_system& io() const = 0;

    /// Binding and listening never waits, so there is nothing here for a token to cancel.
    [[nodiscard]] virtual cc::result<cc::unique_ptr<stream_listener>, error> listen(endpoint const& where,
                                                                                    tcp_listen_options const& options)
        = 0;

    transport() = default;
    transport(transport const&) = delete;
    transport& operator=(transport const&) = delete;
    virtual ~transport() = default;
};
