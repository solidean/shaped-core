#pragma once

#include <clean-core/memory/unique_ptr.hh>
#include <clean-net/transport/tcp.hh>

/// A transport with no network at all: requests are answered in this process, over no socket and no loopback.
///
/// What it is for is testing the layers ABOVE the transport -- a downloader, an asset pipeline, a dev server, an
/// HTTP client -- without a port, a firewall prompt or a machine-dependent timing.
/// A test builds one of these, listens on any endpoint it likes, connects to it, and gets the same
/// `cnet::tcp_listener` and `cnet::tcp_connection` a real socket would have handed back.
///
/// **The addresses are make-believe.** A `virtual_network` answers for whatever endpoint something listened on, and
/// refuses every other one with `connection_refused`; nothing is checked against the machine's real interfaces.
/// A port of 0 is still assigned one, so the "bind to 0 and ask which port" pattern works here too.
///
/// Deadlines are real, because they are the io_system's: a read with nothing to read fails with `timed_out` exactly
/// when the injected clock says so.
class cnet::virtual_network final : public cnet::transport
{
public:
    /// The registry behind the network, named here only because the listeners it hands out share it.
    struct state;

    explicit virtual_network(io_system& io);

    /// Always true: this is the one transport that needs nothing from the platform.
    [[nodiscard]] bool is_supported() const override { return true; }

    [[nodiscard]] cc::shared_async<cc::shared_ptr<tcp_connection>> connect(endpoint const& where,
                                                                           deadline d,
                                                                           tcp_options const& options,
                                                                           cancel_token const& token) override;

    [[nodiscard]] cc::result<cc::unique_ptr<tcp_listener>, error> listen(endpoint const& where,
                                                                         tcp_listen_options const& options) override;

    virtual_network(virtual_network const&) = delete;
    virtual_network& operator=(virtual_network const&) = delete;
    ~virtual_network();

private:
    /// Shared with every listener it hands out, because a listener outliving its network would otherwise deregister
    /// into freed memory.
    cc::shared_ptr<state> _state;
};
