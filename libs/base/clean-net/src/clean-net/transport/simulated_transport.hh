#pragma once

#include <clean-net/transport/stream.hh>

/// What a link does to the traffic passing through it.
///
/// **Every field is off by default**, so a simulated transport nobody configured is indistinguishable from the one
/// underneath.
/// That is not a convenience: without it, a passing test against a simulated link proves nothing, because nobody
/// knows what the link was doing.
struct cnet::link_conditions
{
    /// A fixed delay on every operation, in milliseconds.
    f32 latency_ms = 0;

    /// How much that delay varies, plus or minus, per operation.
    f32 jitter_ms = 0;

    /// Chance that an operation is lost.
    ///
    /// A datagram would be dropped; a stream has no such thing, so this becomes a `connection_reset` -- which is what
    /// loss actually looks like to a caller one layer up.
    f32 loss_probability = 0;

    /// Datagram paths only, and ignored on a stream, which cannot deliver a byte twice.
    f32 duplicate_probability = 0;

    /// Datagram paths only, and ignored on a stream, whose whole promise is that order is kept.
    f32 reorder_probability = 0;

    /// A throughput ceiling, which becomes a delay proportional to the bytes moved.
    /// 0 means no limit.
    f32 bandwidth_bytes_per_sec = 0;

    /// Cut a connection once this many bytes have been received on it; 0 means never.
    ///
    /// The condition worth having above all the others: a connection that dies mid-body is the bug that never
    /// reproduces, because it never happens on loopback.
    isize reset_after_bytes = 0;

    /// Everything above is drawn from this seed, so a failing run replays from two numbers.
    u64 seed = 0;
};

/// A transport that misbehaves on the way through to another one.
///
/// It composes over whatever it is given: over `cnet::virtual_network` for a fast deterministic test, and over
/// `cnet::native_transport` to watch the real client behave on a bad link.
///
/// **Delays are measured on the io_system's clock**, so a test with a `cnet::manual_clock` proves a 30-second stall
/// in microseconds rather than sleeping through it.
/// The seed is logged when the transport is built, which is the other half of a failure that replays.
class cnet::simulated_transport final : public cnet::transport
{
public:
    /// The conditions and the seeded stream behind them, named here only because every connection shares it.
    struct state;

    simulated_transport(io_system& io, transport& under, link_conditions const& conditions = {});

    /// Whatever the transport underneath says: simulating a link cannot conjure one.
    [[nodiscard]] bool is_supported() const override;

    [[nodiscard]] io_system& io() const override;

    [[nodiscard]] cc::shared_async<cc::shared_ptr<stream_connection>> connect(endpoint const& where,
                                                                              deadline d,
                                                                              tcp_options const& options,
                                                                              cancel_token const& token) override;

    [[nodiscard]] cc::result<cc::unique_ptr<stream_listener>, error> listen(endpoint const& where,
                                                                            tcp_listen_options const& options) override;

    simulated_transport(simulated_transport const&) = delete;
    simulated_transport& operator=(simulated_transport const&) = delete;
    ~simulated_transport();

private:
    /// Shared with every connection and listener it hands out, since each of them keeps drawing from the same seeded
    /// stream after this object is gone.
    cc::shared_ptr<state> _state;
};
