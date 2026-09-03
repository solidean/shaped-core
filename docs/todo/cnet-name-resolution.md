# cnet: turning a hostname into an address

**Status:** decided, not built.

## The problem

`getaddrinfo` is the portable way, and it is **blocking, with no timeout and no cancellation**.
It can take thirty seconds on a bad network and there is no standard way to abort it.

There is no portable asynchronous replacement.
`getaddrinfo_a` is glibc-only and implemented with threads anyway.
Windows has `GetAddrInfoExW` with an overlapped form, Darwin has its own, Android routes through the framework, and
wasm cannot resolve at all.

**Resolution is also not just DNS.**
`getaddrinfo` consults the hosts file, mDNS for `.local`, NetBIOS on Windows, VPN split-DNS policy, and whatever the
corporate network configured.
A resolver that speaks DNS over UDP directly gets none of that.

## The decision

**Thread-offloaded `getaddrinfo`, behind our own cache, with happy eyeballs above it.**

The correctness argument decides it.
Every case a hand-rolled resolver gets wrong — `.local` names, VPN-only names, hosts-file overrides, search-domain
completion — is a case that works on our machines and fails on someone else's, and **none of them is detectable in
CI**.
Paying a parked thread for a few hundred milliseconds is affordable; being wrong about `.local` on a customer network
is not.

A custom DNS client was considered and rejected for exactly that.
It would also mean reading `/etc/resolv.conf`, `GetAdaptersAddresses` and the Darwin equivalents to find out which
servers to ask — per-platform work the blocking call already does.

The platform async resolver (`GetAddrInfoExW` on Windows) stays available as a later internal optimization behind the
same interface.

## Happy eyeballs, from the first version

RFC 8305: race an IPv6 and an IPv4 connection attempt so a machine with broken IPv6 routing loses milliseconds rather
than a connect timeout.

Not exotic — it is the difference between "the internet is slow here" and "it works".
It is in the first version because **retrofitting it changes what a connect attempt *is***, from one socket to a
small race, and every caller of `tcp_connect` inherits that shape.

It also needs [cnet-cancellation.md](cnet-cancellation.md): the losing attempt has to be abandoned, and today a
deadline is the only way an operation ends early.

## The shape

```cpp
namespace cnet
{
struct resolve_options
{
    /// Prefer IPv6, IPv4, or race both (RFC 8305).
    /// Racing is the default, because a broken IPv6 route is common and a per-family preference cannot detect one.
    address_family_preference family = address_family_preference::race;
    i32 timeout_ms = 5000;
};

/// Resolve a hostname.
/// A cancelled resolve may still occupy its worker until the underlying blocking call returns.
[[nodiscard]] cc::shared_async<cc::vector<ip_address>> resolve(cc::string_view host, resolve_options const& o = {});

/// Absent on wasm, where the browser resolves inside fetch and a hostname never becomes an address here.
[[nodiscard]] bool resolve_is_supported();
}
```

A **TTL cache is in the first version**, and it is what confines the cost below to first contact with a host.

## The honest caveat, agreed and to be documented

Under `SC_THREADS=OFF` there is no worker, so a resolve is **the one operation that can stall the process**: a
blocking `getaddrinfo` inside a pump holds the only thread there is.

This is accepted rather than mechanised away, on two grounds.
Threads-off native builds are a debugging configuration, and wasm — the configuration that actually ships
single-threaded — never resolves at all, because the browser does it inside `fetch`.

Write it as one sentence in the docs rather than building a mechanism for it.

The multi-second cases come from mDNS, incidentally: a `.local` lookup on a machine with no responder waits for the
mDNS timeout, while ordinary DNS fails fast.
That is what makes accepting the stall reasonable.
