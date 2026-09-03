#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <clean-net/address/ip_address.hh>
#include <clean-net/common/cancel.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>

namespace cnet::impl
{
class resolver_worker;
}

/// Which family a caller wants back.
enum class cnet::address_family_preference : cnet::u8
{
    /// Both, in the order the OS offered them, for the caller above to race (RFC 8305).
    ///
    /// The default, because a broken IPv6 route is common and no per-family preference can detect one -- only trying
    /// both can.
    race,

    /// IPv6 only, and `name_not_resolved` if the host has none.
    v6_only,

    /// IPv4 only, and `name_not_resolved` if the host has none.
    v4_only,
};

/// What one resolve is asked for.
struct cnet::resolve_options
{
    address_family_preference family = address_family_preference::race;

    /// How long the caller will wait.
    ///
    /// The underlying call cannot be aborted, so this bounds the WAIT rather than the work: a resolve that times out
    /// still occupies its worker until the OS returns.
    deadline timeout = deadline::after_secs(5);
};

/// How a resolver is built.
struct cnet::resolver_description
{
    /// How long an answer stays good, in milliseconds.
    ///
    /// The cache is not an optimization here: it is what confines the cost of a blocking lookup -- and, in a
    /// threads-off build, the stall -- to first contact with a host.
    /// It deliberately ignores the DNS record's own TTL, which `getaddrinfo` does not hand back.
    i64 cache_ttl_ms = 60'000;

    /// Resolve with this instead of asking the OS.
    ///
    /// The seam a test uses, for the same reason `cnet::clock` exists: a test that depends on a real resolver depends
    /// on the machine it runs on, on a network, and on a name somebody else controls.
    /// Called on the worker, so it may block.
    cc::unique_function<cc::result<cc::vector<ip_address>, error>(cc::string_view host)> lookup;
};

/// Turns a hostname into addresses.
///
/// **The lookup is a blocking `getaddrinfo` on a worker thread**, and that is a correctness decision rather than a
/// shortcut.
/// A resolver that spoke DNS over UDP itself would be asynchronous and would silently get `.local` names, VPN-only
/// names, hosts-file overrides and search-domain completion wrong -- every one of which works on the machine that
/// wrote it and fails on a customer's, and none of which is detectable in CI.
///
/// **A resolve is the one operation that can stall a threads-off process.**
/// With `SC_THREADS=OFF` there is no worker, so the lookup runs inside `cc::thread_pump_all()` and holds the only
/// thread there is for as long as the network takes.
/// That is accepted rather than mechanised away: threads-off native builds are a debugging configuration, and wasm --
/// the configuration that actually ships single-threaded -- never resolves at all.
/// The cache is what keeps it to first contact with a host.
///
/// **Absent on wasm**, where `try_create` reports `unsupported`, because the browser resolves inside `fetch` and a
/// hostname never becomes an address here.
class cnet::resolver
{
public:
    /// Fails with `unsupported` where the platform cannot resolve, unless a `lookup` was supplied -- one that answers
    /// from a table needs nothing from the platform, which is what makes it testable everywhere.
    [[nodiscard]] static cc::result<cc::unique_ptr<resolver>, error> try_create(io_system& io,
                                                                                resolver_description desc = {});

    /// Throwing counterpart of try_create.
    [[nodiscard]] static cc::unique_ptr<resolver> create(io_system& io, resolver_description desc = {});

    /// Whether the platform can resolve at all.
    [[nodiscard]] static bool is_supported();

    /// Resolve `host`, or hand back the literal address it already is.
    ///
    /// An address in text -- `127.0.0.1`, `[::1]` without its brackets -- is answered at once, with no worker, no
    /// cache and no chance of failing: a caller should not have to know which kind of string it holds.
    /// Otherwise the answer is the OS's, in the OS's order, which already reflects the RFC 6724 sorting it knows more
    /// about than we do.
    [[nodiscard]] cc::shared_async<cc::vector<ip_address>> resolve(cc::string_view host,
                                                                   resolve_options const& options = {},
                                                                   cancel_token const& token = {});

    /// Forget every cached answer.
    /// For a test, and for a program that has just watched the network change under it.
    void clear_cache();

    /// How many hosts the cache is holding, expired entries included.
    [[nodiscard]] isize cached_host_count() const;

    explicit resolver(cc::unique_ptr<impl::resolver_worker> worker);
    resolver(resolver const&) = delete;
    resolver& operator=(resolver const&) = delete;
    ~resolver();

private:
    cc::unique_ptr<impl::resolver_worker> _worker;
};
