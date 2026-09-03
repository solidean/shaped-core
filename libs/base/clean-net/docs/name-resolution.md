# Turning a hostname into an address

`cnet::resolver`, and the race above it.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## Why the blocking call

`getaddrinfo` is the portable way, and it is **blocking, with no timeout and no cancellation**.
It can take thirty seconds on a bad network and there is no standard way to abort it.

There is no portable asynchronous replacement.
`getaddrinfo_a` is glibc-only and implemented with threads anyway; Windows has `GetAddrInfoExW`, Darwin has its own,
Android routes through the framework, and wasm cannot resolve at all.

**Resolution is also not just DNS.**
`getaddrinfo` consults the hosts file, mDNS for `.local`, NetBIOS on Windows, VPN split-DNS policy, and whatever the
corporate network configured.
A resolver speaking DNS over UDP directly gets none of that.

So the decision is thread-offloaded `getaddrinfo`, and the correctness argument is what decides it.
Every case a hand-rolled resolver gets wrong — `.local` names, VPN-only names, hosts-file overrides, search-domain
completion — is a case that works on our machines and fails on someone else's, and **none of them is detectable in
CI**.
Paying a parked thread for a few hundred milliseconds is affordable; being wrong about `.local` on a customer network
is not.

The platform async resolvers stay available as a later internal optimization behind the same interface.

## The three pieces

A blocking call cannot be cancelled, so the answer has to come back without the waiting side depending on it.

- The **worker** is a `cc::threaded_actor` that runs the lookup.
- The **slot** is where it leaves its answer, and the only thing the two threads share.
- The **operation** is a `manual` reactor operation, so a resolve carries the same deadline and the same cancellation
  as everything else rather than a second timeout mechanism.

The slot exists because an operation frees itself once it completes, and a resolve can time out while the worker is
still inside a call nobody can abort.
The worker therefore touches the operation only under the slot's lock, and the operation clears itself out of the slot
before it dies.

**The timeout bounds the wait rather than the work.** A resolve that times out still occupies its worker until the OS
returns.

## The threads-off caveat

**A resolve is the one operation that can stall a threads-off process.**
With `SC_THREADS=OFF` there is no worker, so the lookup runs inside `cc::thread_pump_all()` and holds the only thread
there is for as long as the network takes.

That is accepted rather than mechanised away.
Threads-off native builds are a debugging configuration, and wasm — the configuration that actually ships
single-threaded — never resolves at all, because the browser does it inside `fetch`.
The cache is what keeps it to first contact with a host.

The multi-second cases come from mDNS, incidentally: a `.local` lookup on a machine with no responder waits for the
mDNS timeout, while ordinary DNS fails fast.
That is what makes accepting the stall reasonable.

## The cache

A TTL cache is in the first version, and it is not an optimization: it is what confines the cost of a blocking lookup
to first contact with a host.

The whole answer is cached whatever the caller asked for, so a v4-only caller warms the cache for a v6-only one.
A failure is never cached, because a name that failed once may resolve a second later.
The DNS record's own TTL is ignored, since `getaddrinfo` does not hand it back.

## Happy eyeballs

`cnet::connect_to_host` resolves and connects as one operation, racing the addresses (RFC 8305).

**A machine with broken IPv6 routing loses milliseconds rather than a connect timeout.**
That is not exotic — it is the difference between "the internet is slow here" and "it works".
No per-family preference can detect a route that is advertised and does not work; only trying both can.

It is here from the first version rather than added later, because retrofitting it changes what a connect attempt
*is*, from one socket to a small race, and every caller inherits that shape.

How it runs:

- The addresses are **interleaved by family**, so the first two attempts are one of each, IPv6 first.
  Within a family the order is the OS's, which already reflects RFC 6724 sorting.
- Attempt zero starts at once; a `timer` starts the next after `attempt_delay_ms` (RFC 8305's Connection Attempt
  Delay, 250 ms).
  The earlier attempt keeps running — this is a stagger, not a timeout.
- A failure starts the next attempt immediately, so the delay is a floor on the spacing rather than a ceiling on how
  many run at once.
- **One budget covers the whole thing**: the resolve and every attempt spend the same deadline, because a per-step
  timeout would let a four-address host take four times what the caller asked for.

**Every attempt gets a child of the caller's token.**
Cancelling the caller's token cancels the race; settling the race cancels its own attempts and nothing above them.
That is what `cnet::cancel_token::create_child` is for — see [cancellation.md](cancellation.md).

If every attempt fails, the failure reported is the **first** one, because it is the one about the address the OS
thought best.
