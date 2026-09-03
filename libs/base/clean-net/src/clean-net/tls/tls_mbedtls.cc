#include "tls.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-net/fwd.hh>
#include <clean-net/impl/async_glue.hh>
#include <clean-net/impl/trust_store.hh>

#include <memory> // std::unique_ptr, to own a backend through its interface -- see .shaped-lint.yml
#include <mutex>  // std::mutex, as the mutex Mbed TLS is given -- see .shaped-lint.yml

// TLS over whatever connection it was handed, which is the whole reason the transport seam exists.
//
// THE SHAPE OF THE PROBLEM.
// Mbed TLS is a synchronous state machine that reads and writes through two callbacks and answers WANT_READ or
// WANT_WRITE when they cannot proceed.
// Nothing here may block, so those callbacks never touch a socket: they move bytes to and from two buffers, and a
// pump drives the state machine and the real connection alternately until something finishes.
//
// THE PUMP IS THE WHOLE DESIGN.
// One step drives the record layer as far as it goes, collects the bytes it produced, and only then -- outside the
// lock -- starts the underlying operations and completes the promises.
// Both matter: an underlying operation can complete INLINE (a virtual connection does), so a continuation re-enters
// the pump while the first call is still on the stack, and a promise's continuation can do anything at all.
// The pumping/again pair turns that re-entrancy into a loop instead of recursion, and means a completion arriving
// from the reactor thread while another thread is pumping is never dropped.
//
// ONE CONTEXT, ONE THREAD AT A TIME.
// An mbedtls_ssl_context is a single-threaded state machine, and the lock is what makes that true here.
// A read and a write may both be outstanding -- the pump drives them in turn, which is exactly what the record layer
// supports and all it supports.

#ifndef CNET_HAS_TLS
#define CNET_HAS_TLS 0
#endif

#if CNET_HAS_TLS

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>

namespace cnet
{
namespace
{
/// How much of the socket is read in one go.
/// A TLS record is at most 16 KB plus its overhead, so this is one record's worth and the size mbedtls itself uses.
constexpr isize k_read_chunk = 16 * 1024 + 512;

// ---- making the record layer safe to use from two threads ----------------------------------------------

/// The mutex operations Mbed TLS is configured to call.
///
/// Its PSA layer keeps process-wide state that every TLS 1.3 handshake goes through, so a program handshaking on two
/// threads at once needs these -- without them the damage shows up as an occasional handshake failing for no visible
/// reason, which is the worst way for a bug like this to present.
void mbedtls_mutex_init(mbedtls_threading_mutex_t* mutex)
{
    mutex->opaque = new std::mutex();
}

void mbedtls_mutex_free(mbedtls_threading_mutex_t* mutex)
{
    delete static_cast<std::mutex*>(mutex->opaque);
    mutex->opaque = nullptr;
}

int mbedtls_mutex_lock(mbedtls_threading_mutex_t* mutex)
{
    if (mutex->opaque == nullptr)
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    static_cast<std::mutex*>(mutex->opaque)->lock();
    return 0;
}

int mbedtls_mutex_unlock(mbedtls_threading_mutex_t* mutex)
{
    if (mutex->opaque == nullptr)
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    static_cast<std::mutex*>(mutex->opaque)->unlock();
    return 0;
}

/// Install them, exactly once and before anything else touches Mbed TLS.
///
/// A function-local static is what makes "exactly once" true across threads; `mbedtls_threading_set_alt` itself is
/// not safe to call twice or concurrently, and it must run before the first context is initialized because it is
/// what brings upstream's own global mutexes to life.
void ensure_threading_installed()
{
    static auto const installed = []
    {
        mbedtls_threading_set_alt(mbedtls_mutex_init, mbedtls_mutex_free, mbedtls_mutex_lock, mbedtls_mutex_unlock);
        return true;
    }();
    (void)installed;
}

/// What the state machine is being driven towards.
enum class tls_phase : u8
{
    handshaking,
    established,
    dead,
};

[[nodiscard]] error tls_error(error_code code, cc::string_view what, i32 native)
{
    // Mbed TLS spells its own codes as negative numbers, and its message table is the only place they mean anything.
    char detail[128] = {};
    mbedtls_strerror(native, detail, sizeof(detail));
    return {.code = code, .native_code = native, .message = cc::format("{}: {}", what, cc::string_view(detail))};
}

/// Everything one TLS connection owns, and the only thing its callbacks touch.
struct tls_data
{
    // ---- the record layer ----
    mbedtls_ssl_context ssl = {};
    mbedtls_ssl_config conf = {};
    mbedtls_entropy_context entropy = {};
    mbedtls_ctr_drbg_context drbg = {};
    mbedtls_x509_crt roots = {};
    mbedtls_x509_crt own_cert = {};
    mbedtls_pk_context own_key = {};
    bool contexts_live = false;

    /// Kept because mbedtls stores the pointers rather than the strings.
    cc::vector<cc::string> alpn_storage;
    cc::vector<char const*> alpn_pointers;
    cc::string hostname;
    cc::string negotiated_alpn;

    // ---- the buffers the callbacks move bytes through ----
    cc::vector<byte> inbound;
    isize inbound_pos = 0;
    cc::vector<byte> outbound;

    /// The bytes a socket send is currently carrying; they must outlive it.
    cc::vector<byte> outbound_in_flight;

    cc::vector<byte> read_scratch;

    // ---- what is going on ----
    tls_phase phase = tls_phase::handshaking;
    bool socket_reading = false;
    bool socket_writing = false;
    bool peer_eof = false;
    bool closed = false;
    cc::optional<error> fatal;

    // ---- the operations in flight, at most one of each ----
    cc::shared_async<cc::shared_ptr<stream_connection>> handshake_promise;

    /// The wrapper the handshake will hand back, built before the connection exists so that a failure closes it by
    /// dropping this rather than by remembering to.
    /// Cleared the moment it is pushed: the state is reachable from it, and holding both ways is a cycle.
    cc::shared_ptr<stream_connection> pending_wrapper;

    /// What the underlying reads and writes are given.
    /// Whatever is driving the record layer owns the budget -- the handshake while handshaking, then each read or
    /// write in turn.
    deadline io_deadline = deadline::after_secs(30);

    cc::shared_async<isize> read_promise;
    byte* read_buffer = nullptr;
    isize read_size = 0;

    cc::shared_async<cc::unit> write_promise;
    byte const* write_buffer = nullptr;
    isize write_size = 0;
    isize write_done = 0;

    ~tls_data()
    {
        if (!contexts_live)
            return;

        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&roots);
        mbedtls_x509_crt_free(&own_cert);
        mbedtls_pk_free(&own_key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

struct tls_state
{
    cc::shared_ptr<stream_connection> under;
    cancel_token token;
    deadline handshake_deadline;

    cc::mutex<tls_data> data;

    /// The re-entrancy pair, outside the lock on purpose: a completion that arrives while another thread is inside
    /// the pump sets `again` rather than waiting, and the thread already pumping picks it up.
    cc::atomic<bool> pumping = false;
    cc::atomic<bool> again = false;

    tls_state(cc::shared_ptr<stream_connection> u, cancel_token t, deadline d)
      : under(cc::move(u)), token(cc::move(t)), handshake_deadline(d)
    {
    }
};

void pump(cc::shared_ptr<tls_state> const& state);

// ---- the two callbacks the record layer reaches the world through ---------------------------------------

int bio_send(void* ctx, unsigned char const* buf, size_t len)
{
    // Always accepted: the bytes go into a buffer the pump flushes, so the record layer never has to wait to write.
    auto* const d = static_cast<tls_data*>(ctx);
    for (size_t i = 0; i < len; ++i)
        d->outbound.push_back(byte(buf[i]));
    return int(len);
}

int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    auto* const d = static_cast<tls_data*>(ctx);

    auto const available = d->inbound.size() - d->inbound_pos;
    if (available <= 0)
    {
        // Zero means end of stream to the record layer, which is what a peer that hung up mid-handshake looks like.
        if (d->peer_eof)
            return 0;
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    auto const n = available < isize(len) ? available : isize(len);
    for (isize i = 0; i < n; ++i)
        buf[i] = static_cast<unsigned char>(d->inbound[d->inbound_pos + i]);

    d->inbound_pos += n;
    if (d->inbound_pos == d->inbound.size())
    {
        d->inbound.clear();
        d->inbound_pos = 0;
    }
    return int(n);
}

// ---- driving the state machine -------------------------------------------------------------------------

/// What one pump step decided to do, once the lock is gone.
struct pump_plan
{
    bool start_send = false;
    bool start_receive = false;

    cc::optional<cc::shared_async<cc::shared_ptr<stream_connection>>> finish_handshake;
    cc::optional<error> handshake_failure;
    cc::shared_ptr<stream_connection> handshake_value;

    cc::optional<cc::shared_async<isize>> finish_read;
    isize read_bytes = 0;
    cc::optional<error> read_failure;

    cc::optional<cc::shared_async<cc::unit>> finish_write;
    cc::optional<error> write_failure;
};

/// Turn a failed mbedtls call into the failure a caller branches on.
[[nodiscard]] error handshake_error(tls_data& d, int ret)
{
    // A chain that was built and refused is a decision; everything else is a breakdown.
    // Only the first is worth a caller distinguishing, and only it can be answered by trusting something else.
    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || mbedtls_ssl_get_verify_result(&d.ssl) != 0)
    {
        char reason[256] = {};
        auto const flags = mbedtls_ssl_get_verify_result(&d.ssl);
        mbedtls_x509_crt_verify_info(reason, sizeof(reason), "", flags);
        return {.code = error_code::certificate_rejected,
                .native_code = i32(ret),
                .message = cc::format("the certificate was rejected: {}", cc::string_view(reason))};
    }

    return tls_error(error_code::tls_handshake_failed, "the TLS handshake failed", i32(ret));
}

/// Fail everything outstanding, once the connection is beyond saving.
void collect_fatal(tls_data& d, pump_plan& plan)
{
    if (d.handshake_promise.is_valid())
    {
        plan.finish_handshake = d.handshake_promise;
        plan.handshake_failure = d.fatal;
        d.handshake_promise = {};
        d.pending_wrapper = {};
    }
    if (d.read_promise.is_valid())
    {
        plan.finish_read = d.read_promise;
        plan.read_failure = d.fatal;
        d.read_promise = {};
    }
    if (d.write_promise.is_valid())
    {
        plan.finish_write = d.write_promise;
        plan.write_failure = d.fatal;
        d.write_promise = {};
    }
}

void drive_handshake(tls_data& d, pump_plan& plan)
{
    auto const ret = mbedtls_ssl_handshake(&d.ssl);
    if (ret == 0)
    {
        d.phase = tls_phase::established;

        if (auto const* const chosen = mbedtls_ssl_get_alpn_protocol(&d.ssl); chosen != nullptr)
            d.negotiated_alpn = cc::string(cc::string_view(chosen));

        plan.finish_handshake = d.handshake_promise;
        plan.handshake_value = cc::move(d.pending_wrapper);
        d.handshake_promise = {};
        d.pending_wrapper = {};
        return;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return;

    d.phase = tls_phase::dead;
    d.fatal = handshake_error(d, ret);
    collect_fatal(d, plan);
}

void drive_write(tls_data& d, pump_plan& plan)
{
    while (d.write_done < d.write_size)
    {
        auto const remaining = d.write_size - d.write_done;
        auto const ret = mbedtls_ssl_write(
            &d.ssl, reinterpret_cast<unsigned char const*>(d.write_buffer + d.write_done), size_t(remaining));
        if (ret > 0)
        {
            d.write_done += isize(ret);
            continue;
        }

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            return;

        d.phase = tls_phase::dead;
        d.fatal = tls_error(error_code::connection_reset, "the TLS connection failed while sending", i32(ret));
        collect_fatal(d, plan);
        return;
    }

    plan.finish_write = d.write_promise;
    d.write_promise = {};
}

void drive_read(tls_data& d, pump_plan& plan)
{
    auto const ret = mbedtls_ssl_read(&d.ssl, reinterpret_cast<unsigned char*>(d.read_buffer), size_t(d.read_size));
    if (ret > 0)
    {
        plan.finish_read = d.read_promise;
        plan.read_bytes = isize(ret);
        d.read_promise = {};
        return;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return;

    // A close_notify is the peer saying it is done, which is a clean end rather than a failure of the connection.
    d.phase = tls_phase::dead;
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0)
        d.fatal = error{.code = error_code::connection_closed,
                        .native_code = 0,
                        .message = cc::string("the peer closed the TLS connection")};
    else
        d.fatal = tls_error(error_code::connection_reset, "the TLS connection failed while receiving", i32(ret));

    collect_fatal(d, plan);
}

[[nodiscard]] pump_plan pump_step(tls_data& d)
{
    auto plan = pump_plan();

    if (d.fatal.has_value())
    {
        collect_fatal(d, plan);
        return plan;
    }

    if (d.phase == tls_phase::handshaking && d.handshake_promise.is_valid())
        drive_handshake(d, plan);

    if (d.phase == tls_phase::established)
    {
        // Writing first: a write that produces records is what a peer is usually waiting for before it answers.
        if (d.write_promise.is_valid())
            drive_write(d, plan);
        if (d.read_promise.is_valid())
            drive_read(d, plan);
    }

    // Whatever the record layer produced goes out, and only one socket send may be in flight at a time.
    if (!d.outbound.empty() && !d.socket_writing)
    {
        d.outbound_in_flight = cc::move(d.outbound);
        d.outbound = {};
        d.socket_writing = true;
        plan.start_send = true;
    }

    // And it is fed only when it asked to be, which is what keeps a finished connection from holding a read open.
    auto const wants_input = d.handshake_promise.is_valid() || d.read_promise.is_valid();
    if (wants_input && !d.socket_reading && !d.peer_eof && !d.closed)
    {
        d.socket_reading = true;
        plan.start_receive = true;
    }

    return plan;
}

void pump_once(cc::shared_ptr<tls_state> const& state)
{
    auto plan = state->data.lock([](tls_data& d) { return pump_step(d); });

    // Everything below happens OUTSIDE the lock: an underlying operation can complete inline, and a promise's
    // continuation can do anything -- including calling back into this connection.
    if (plan.start_send)
    {
        auto const bytes = state->data.lock(
            [](tls_data& d) { return cc::span<byte const>(d.outbound_in_flight.data(), d.outbound_in_flight.size()); });

        auto const send_deadline = state->data.lock([](tls_data const& d) { return d.io_deadline; });
        impl::when_ready(state->under->send(bytes, send_deadline, state->token),
                         [state](cc::shared_async<cc::unit> const& sent)
                         {
                             state->data.lock(
                                 [&](tls_data& d)
                                 {
                                     d.socket_writing = false;
                                     d.outbound_in_flight.clear();
                                     if (sent->has_error() && !d.fatal.has_value())
                                     {
                                         d.phase = tls_phase::dead;
                                         d.fatal = error{.code = error_code::connection_reset,
                                                         .native_code = 0,
                                                         .message = cc::string("the connection under TLS failed while "
                                                                               "sending")};
                                     }
                                 });
                             pump(state);
                         });
    }

    if (plan.start_receive)
    {
        auto const buffer
            = state->data.lock([](tls_data& d) { return cc::span<byte>(d.read_scratch.data(), d.read_scratch.size()); });

        auto const receive_deadline = state->data.lock([](tls_data const& d) { return d.io_deadline; });
        impl::when_ready(state->under->receive(buffer, receive_deadline, state->token),
                         [state](cc::shared_async<isize> const& received)
                         {
                             state->data.lock(
                                 [&](tls_data& d)
                                 {
                                     d.socket_reading = false;

                                     if (received->has_error())
                                     {
                                         // Everything the connection underneath can report ends the input side: a
                                         // close, a reset and a timeout are all "no more bytes are coming".
                                         d.peer_eof = true;
                                         return;
                                     }

                                     auto const n = received->value();
                                     for (isize i = 0; i < n; ++i)
                                         d.inbound.push_back(d.read_scratch[i]);
                                 });
                             pump(state);
                         });
    }

    if (plan.finish_handshake.has_value())
    {
        if (plan.handshake_failure.has_value())
        {
            // A half-negotiated stream is not a stream anybody can use, so it does not go back to the caller.
            state->under->close();
            plan.finish_handshake.value()->push_error(to_async_error(cc::move(plan.handshake_failure.value())));
        }
        else
        {
            plan.finish_handshake.value()->push_value(cc::move(plan.handshake_value));
        }
    }

    if (plan.finish_read.has_value())
    {
        if (plan.read_failure.has_value())
            plan.finish_read.value()->push_error(to_async_error(cc::move(plan.read_failure.value())));
        else
            plan.finish_read.value()->push_value(plan.read_bytes);
    }

    if (plan.finish_write.has_value())
    {
        if (plan.write_failure.has_value())
            plan.finish_write.value()->push_error(to_async_error(cc::move(plan.write_failure.value())));
        else
            plan.finish_write.value()->push_value(cc::unit{});
    }
}

void pump(cc::shared_ptr<tls_state> const& state)
{
    // Announce the work before claiming the pump, so a thread that finds one running knows its work will be seen.
    state->again.store(true);

    for (;;)
    {
        if (state->pumping.exchange(true))
            return;

        while (state->again.exchange(false))
            pump_once(state);

        state->pumping.store(false);

        // A completion that arrived between the last exchange and the release would otherwise sit there forever.
        if (!state->again.load())
            return;
    }
}

// ---- the connection a caller is handed -----------------------------------------------------------------

/// A connection whose bytes go through the record layer.
class tls_connection final : public connection_backend
{
public:
    explicit tls_connection(cc::shared_ptr<tls_state> state) : _state(cc::move(state)) {}

    [[nodiscard]] cc::shared_async<isize> receive(cc::span<byte> buffer, deadline d, cancel_token const& token) override
    {
        auto promise = cc::make_async_manual<isize>();

        auto const refused = _state->data.lock(
            [&](tls_data& data) -> cc::optional<error>
            {
                if (data.closed)
                    return error{.code = error_code::connection_closed,
                                 .native_code = 0,
                                 .message = cc::string("the connection is closed")};
                if (token.is_cancelled())
                    return error{.code = error_code::cancelled,
                                 .native_code = 0,
                                 .message = cc::string("the operation was cancelled")};

                CC_ASSERT(!data.read_promise.is_valid(), "two reads at once on one connection: the second would take "
                                                         "bytes the first was promised");

                data.io_deadline = d;
                data.read_promise = promise;
                data.read_buffer = buffer.data();
                data.read_size = buffer.size();
                return {};
            });

        if (refused.has_value())
            return impl::failed_async<isize>(cc::move(refused.value()));

        pump(_state);
        return promise;
    }

    [[nodiscard]] cc::shared_async<cc::unit> send(cc::span<byte const> bytes, deadline d, cancel_token const& token) override
    {
        auto promise = cc::make_async_manual<cc::unit>();

        auto const refused = _state->data.lock(
            [&](tls_data& data) -> cc::optional<error>
            {
                if (data.closed)
                    return error{.code = error_code::connection_closed,
                                 .native_code = 0,
                                 .message = cc::string("the connection is closed")};
                if (token.is_cancelled())
                    return error{.code = error_code::cancelled,
                                 .native_code = 0,
                                 .message = cc::string("the operation was cancelled")};

                CC_ASSERT(!data.write_promise.is_valid(), "two writes at once on one connection: the record layer "
                                                          "would interleave them");

                data.io_deadline = d;
                data.write_promise = promise;
                data.write_buffer = bytes.data();
                data.write_size = bytes.size();
                data.write_done = 0;
                return {};
            });

        if (refused.has_value())
            return impl::failed_async<cc::unit>(cc::move(refused.value()));

        pump(_state);
        return promise;
    }

    cc::result<cc::unit, error> shutdown_send() override
    {
        // close_notify first: ending the stream without it is a truncation attack from the peer's point of view, and
        // it cannot tell that one from an attacker cutting the connection.
        _state->data.lock(
            [](tls_data& d)
            {
                if (d.phase == tls_phase::established)
                    (void)mbedtls_ssl_close_notify(&d.ssl);
            });

        pump(_state);
        return _state->under->shutdown_send();
    }

    [[nodiscard]] endpoint local() const override { return _state->under->local(); }
    [[nodiscard]] endpoint peer() const override { return _state->under->peer(); }

    [[nodiscard]] bool is_open() const override
    {
        auto const closed = _state->data.lock([](tls_data const& d) { return d.closed || d.phase == tls_phase::dead; });
        return !closed && _state->under->is_open();
    }

    void close() override
    {
        _state->data.lock([](tls_data& d) { d.closed = true; });
        _state->under->close();
    }

    [[nodiscard]] cc::string_view negotiated_alpn() const override
    {
        // Safe to hand out: it is written once, while the handshake completes, and never changes afterwards.
        return _state->data.lock([](tls_data const& d) { return cc::string_view(d.negotiated_alpn); });
    }

private:
    cc::shared_ptr<tls_state> _state;
};

// ---- setting the record layer up -----------------------------------------------------------------------

/// Parse a PEM certificate into `chain`.
///
/// Mbed TLS wants the terminating NUL counted in the length for PEM, which is the one detail that turns a valid
/// certificate into a parse error.
[[nodiscard]] cc::optional<error> parse_pem_certificate(mbedtls_x509_crt& chain, cc::string const& pem)
{
    auto text = cc::string(pem);
    auto const* const bytes = text.c_str_materialize();

    auto const ret = mbedtls_x509_crt_parse(&chain, reinterpret_cast<unsigned char const*>(bytes), text.size() + 1);
    if (ret != 0)
        return tls_error(error_code::invalid_argument, "could not parse a certificate", i32(ret));
    return {};
}

[[nodiscard]] cc::optional<error> apply_identity(tls_data& d, tls_identity const& identity)
{
    if (auto failure = parse_pem_certificate(d.own_cert, identity.certificate_chain_pem); failure.has_value())
        return failure;

    auto key = cc::string(identity.private_key_pem);
    auto const* const key_bytes = key.c_str_materialize();

    auto ret = mbedtls_pk_parse_key(&d.own_key, reinterpret_cast<unsigned char const*>(key_bytes), key.size() + 1,
                                    nullptr, 0, mbedtls_ctr_drbg_random, &d.drbg);
    if (ret != 0)
        return tls_error(error_code::invalid_argument, "could not parse a private key", i32(ret));

    ret = mbedtls_ssl_conf_own_cert(&d.conf, &d.own_cert, &d.own_key);
    if (ret != 0)
        return tls_error(error_code::invalid_argument, "the certificate and key do not go together", i32(ret));

    return {};
}

void apply_alpn(tls_data& d, cc::vector<cc::string> const& protocols)
{
    if (protocols.empty())
        return;

    // Mbed TLS keeps the pointers rather than the strings, and wants the array NUL-terminated -- so both the strings
    // and the array live in the state for as long as the configuration does.
    for (auto const& p : protocols)
    {
        auto copy = cc::string(p);
        (void)copy.c_str_materialize();
        d.alpn_storage.push_back(cc::move(copy));
    }

    for (auto& stored : d.alpn_storage)
        d.alpn_pointers.push_back(stored.c_str_materialize());
    d.alpn_pointers.push_back(nullptr);

    (void)mbedtls_ssl_conf_alpn_protocols(&d.conf, d.alpn_pointers.data());
}

/// The parts every handshake needs, whichever end it is.
[[nodiscard]] cc::optional<error> setup_common(tls_data& d, bool is_server)
{
    ensure_threading_installed();

    mbedtls_ssl_init(&d.ssl);
    mbedtls_ssl_config_init(&d.conf);
    mbedtls_entropy_init(&d.entropy);
    mbedtls_ctr_drbg_init(&d.drbg);
    mbedtls_x509_crt_init(&d.roots);
    mbedtls_x509_crt_init(&d.own_cert);
    mbedtls_pk_init(&d.own_key);
    d.contexts_live = true;

    d.read_scratch.resize_to_defaulted(k_read_chunk);

    auto const personalization = cc::string_view("shaped-core clean-net");
    auto ret = mbedtls_ctr_drbg_seed(&d.drbg, mbedtls_entropy_func, &d.entropy,
                                     reinterpret_cast<unsigned char const*>(personalization.data()),
                                     size_t(personalization.size()));
    if (ret != 0)
        return tls_error(error_code::unknown, "the random generator could not be seeded", i32(ret));

    ret = mbedtls_ssl_config_defaults(&d.conf, is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        return tls_error(error_code::unknown, "the TLS configuration could not be built", i32(ret));

    mbedtls_ssl_conf_rng(&d.conf, mbedtls_ctr_drbg_random, &d.drbg);
    return {};
}

[[nodiscard]] cc::optional<error> setup_client(tls_data& d, cc::string_view hostname, tls_options const& options)
{
    if (auto failure = setup_common(d, false); failure.has_value())
        return failure;

    auto roots_loaded = 0;

    if (options.trust.use_system_roots)
    {
        auto system_roots = impl::system_root_certificates();
        if (system_roots.has_error() && !options.trust.allow_any_certificate && options.trust.additional_roots_pem.empty())
        {
            // Nothing to verify against and nothing said otherwise: failing here is the honest answer, because the
            // alternative is a connection that looks verified and is not.
            return cc::move(system_roots).error();
        }

        if (system_roots.has_value())
            for (auto const& der : system_roots.value())
                if (mbedtls_x509_crt_parse_der(&d.roots, reinterpret_cast<unsigned char const*>(der.data()),
                                               size_t(der.size()))
                    == 0)
                    ++roots_loaded;
    }

    for (auto const& pem : options.trust.additional_roots_pem)
    {
        if (auto failure = parse_pem_certificate(d.roots, pem); failure.has_value())
            return failure;
        ++roots_loaded;
    }

    // VERIFY_NONE only where a caller said so in code: this is the one setting that turns TLS into obfuscation.
    mbedtls_ssl_conf_authmode(
        &d.conf, options.trust.allow_any_certificate ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&d.conf, &d.roots, nullptr);

    if (options.client_identity.has_value())
        if (auto failure = apply_identity(d, options.client_identity.value()); failure.has_value())
            return failure;

    apply_alpn(d, options.alpn);

    auto ret = mbedtls_ssl_setup(&d.ssl, &d.conf);
    if (ret != 0)
        return tls_error(error_code::unknown, "the TLS context could not be set up", i32(ret));

    // The name, not the address: SNI and the certificate's own subject are both about the name the caller asked for.
    d.hostname = cc::string(hostname);
    ret = mbedtls_ssl_set_hostname(&d.ssl, d.hostname.c_str_materialize());
    if (ret != 0)
        return tls_error(error_code::invalid_argument, "the hostname could not be set", i32(ret));

    CC_LOG_TRACE("TLS client handshake to {}, {} roots", hostname, roots_loaded);
    return {};
}

[[nodiscard]] cc::optional<error> setup_server(tls_data& d, tls_server_options const& options)
{
    if (auto failure = setup_common(d, true); failure.has_value())
        return failure;

    if (auto failure = apply_identity(d, options.identity); failure.has_value())
        return failure;

    apply_alpn(d, options.alpn);

    auto const ret = mbedtls_ssl_setup(&d.ssl, &d.conf);
    if (ret != 0)
        return tls_error(error_code::unknown, "the TLS context could not be set up", i32(ret));

    return {};
}

/// Build the state, wire the record layer to its buffers, and start the handshake.
template <class F>
[[nodiscard]] cc::shared_async<cc::shared_ptr<stream_connection>> start_handshake(cc::shared_ptr<stream_connection> under,
                                                                                  deadline d,
                                                                                  cancel_token const& token,
                                                                                  F setup)
{
    using handle = cc::shared_ptr<stream_connection>;

    if (!under.is_valid() || !under->is_open())
        return impl::failed_async<handle>({.code = error_code::connection_closed,
                                           .native_code = 0,
                                           .message = cc::string("the connection under TLS is closed")});

    auto state = cc::make_shared<tls_state>(cc::move(under), token, d);

    auto promise = cc::make_async_manual<handle>();

    auto failure = state->data.lock(
        [&](tls_data& data) -> cc::optional<error>
        {
            if (auto problem = setup(data); problem.has_value())
                return problem;

            data.io_deadline = d;
            data.handshake_promise = promise;

            // The context pointer is the value inside the mutex, which never moves and is only ever reached from
            // inside a lock -- the callbacks run from pump_step and nowhere else.
            mbedtls_ssl_set_bio(&data.ssl, &data, bio_send, bio_recv, nullptr);
            return {};
        });

    if (failure.has_value())
    {
        state->under->close();
        return impl::failed_async<handle>(cc::move(failure.value()));
    }

    // The wrapper exists before the connection does, so a failed handshake closes what it was given by dropping this.
    auto wrapper = cc::make_shared<stream_connection>(std::make_unique<tls_connection>(state));
    state->data.lock([&](tls_data& data) { data.pending_wrapper = wrapper; });

    pump(state);
    return promise;
}
} // namespace

bool tls_is_supported()
{
    return true;
}

cc::shared_async<cc::shared_ptr<stream_connection>> tls_connect(cc::shared_ptr<stream_connection> connection,
                                                                cc::string_view hostname,
                                                                tls_options const& options,
                                                                deadline d,
                                                                cancel_token const& token)
{
    return start_handshake(cc::move(connection), d, token,
                           [&](tls_data& data) { return setup_client(data, hostname, options); });
}

cc::shared_async<cc::shared_ptr<stream_connection>> tls_accept(cc::shared_ptr<stream_connection> connection,
                                                               tls_server_options const& options,
                                                               deadline d,
                                                               cancel_token const& token)
{
    return start_handshake(cc::move(connection), d, token, [&](tls_data& data) { return setup_server(data, options); });
}

cc::result<tls_identity, error> tls_make_self_signed(cc::string_view hostname)
{
    // Everything here is torn down on the way out, in reverse order, whichever branch is taken -- which is what the
    // scope guard below is for, since mbedtls is a C API with seven things to free.
    ensure_threading_installed();

    auto entropy = mbedtls_entropy_context();
    auto drbg = mbedtls_ctr_drbg_context();
    auto key = mbedtls_pk_context();
    auto crt = mbedtls_x509write_cert();

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    struct scope_guard
    {
        mbedtls_entropy_context* entropy;
        mbedtls_ctr_drbg_context* drbg;
        mbedtls_pk_context* key;
        mbedtls_x509write_cert* crt;

        ~scope_guard()
        {
            mbedtls_x509write_crt_free(crt);
            mbedtls_pk_free(key);
            mbedtls_ctr_drbg_free(drbg);
            mbedtls_entropy_free(entropy);
        }
    } const guard{&entropy, &drbg, &key, &crt};

    auto const personalization = cc::string_view("shaped-core self-signed");
    auto ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                     reinterpret_cast<unsigned char const*>(personalization.data()),
                                     size_t(personalization.size()));
    if (ret != 0)
        return cc::error(tls_error(error_code::unknown, "the random generator could not be seeded", i32(ret)));

    // P-256 rather than RSA: a key in about a millisecond instead of a second, and every peer we care about takes it.
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0)
        ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg);
    if (ret != 0)
        return cc::error(tls_error(error_code::unknown, "the key could not be generated", i32(ret)));

    auto subject = cc::format("CN={}", hostname);
    auto const* const subject_text = subject.c_str_materialize();

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, subject_text);
    if (ret == 0)
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject_text);
    if (ret != 0)
        return cc::error(
            tls_error(error_code::invalid_argument, "the hostname is not a usable certificate name", i32(ret)));

    unsigned char const serial[] = {1};
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, const_cast<unsigned char*>(serial), sizeof(serial));
    if (ret == 0)
        ret = mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20350101000000");
    if (ret == 0)
        ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0); // its own CA, so it can be handed over as a root
    if (ret != 0)
        return cc::error(tls_error(error_code::unknown, "the certificate could not be built", i32(ret)));

    // A subject alternative name, not just the common name: SAN is what a modern verifier looks at, and a
    // certificate with only a CN is one that works here and fails against anything stricter.
    auto host = cc::string(hostname);
    auto san = mbedtls_x509_san_list();
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(const_cast<char*>(host.c_str_materialize()));
    san.node.san.unstructured_name.len = size_t(host.size());
    san.next = nullptr;

    ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san);
    if (ret != 0)
        return cc::error(tls_error(error_code::unknown, "the subject alternative name could not be set", i32(ret)));

    // 4 KB holds a P-256 certificate and its key several times over; a fixed buffer keeps this a leaf function.
    char certificate_pem[4096] = {};
    char key_pem[4096] = {};

    ret = mbedtls_x509write_crt_pem(&crt, reinterpret_cast<unsigned char*>(certificate_pem), sizeof(certificate_pem),
                                    mbedtls_ctr_drbg_random, &drbg);
    if (ret == 0)
        ret = mbedtls_pk_write_key_pem(&key, reinterpret_cast<unsigned char*>(key_pem), sizeof(key_pem));
    if (ret != 0)
        return cc::error(tls_error(error_code::unknown, "the certificate could not be written", i32(ret)));

    return tls_identity{.certificate_chain_pem = cc::string(cc::string_view(certificate_pem)),
                        .private_key_pem = cc::string(cc::string_view(key_pem))};
}

cc::string_view tls_negotiated_alpn(stream_connection const& connection)
{
    return connection.negotiated_alpn();
}
} // namespace cnet

#else // CNET_HAS_TLS -- the browser holds the TLS here, and a program never sees a handshake

namespace cnet
{
bool tls_is_supported()
{
    return false;
}

cc::shared_async<cc::shared_ptr<stream_connection>> tls_connect(cc::shared_ptr<stream_connection> connection,
                                                                cc::string_view,
                                                                tls_options const&,
                                                                deadline,
                                                                cancel_token const&)
{
    if (connection.is_valid())
        connection->close();
    return impl::failed_async<cc::shared_ptr<stream_connection>>(unsupported_here("TLS"));
}

cc::shared_async<cc::shared_ptr<stream_connection>> tls_accept(cc::shared_ptr<stream_connection> connection,
                                                               tls_server_options const&,
                                                               deadline,
                                                               cancel_token const&)
{
    if (connection.is_valid())
        connection->close();
    return impl::failed_async<cc::shared_ptr<stream_connection>>(unsupported_here("TLS"));
}

cc::result<tls_identity, error> tls_make_self_signed(cc::string_view)
{
    return cc::error(unsupported_here("generating a certificate"));
}

cc::string_view tls_negotiated_alpn(stream_connection const&)
{
    return {};
}
} // namespace cnet

#endif // CNET_HAS_TLS
