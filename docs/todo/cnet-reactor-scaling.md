# cnet: IOCP and epoll, and what would make them worth it

**Status:** deliberately deferred.
The interface that makes it a contained swap is already built.

## What exists

One shared readiness poller behind a completion-shaped interface:
`select` on Windows, `poll` everywhere else — [reactor_poll.cc](../../libs/base/clean-net/src/clean-net/impl/reactor_poll.cc).

The interface is completion-shaped precisely so the poller can be replaced without touching a line above it.
A caller submits an operation and is told when it *finished*, never when a socket became ready.

## Why the shared poller was chosen over IOCP and epoll

The plan called for IOCP and epoll written together against one seam.
That was changed during implementation, and the reasoning is the part worth keeping:

**IOCP and epoll share no code.** "Both together" therefore means writing two implementations and verifying one,
since only one platform is ever in front of you.
The unverified half then ships and is discovered later, on someone else's machine.

One shared path means Linux and macOS run the logic Windows has actually been run against, and the entire
platform-specific surface is one function.

## Why `select` and not `WSAPoll` on Windows

This one is a correctness choice rather than a scale one, and it stands regardless of what replaces the poller.

**WSAPoll does not report a failed connection.**
A refused connect appears in neither the readable nor the error set, so the operation hangs until its deadline
instead of failing with `connection_refused`.
It is a known, unfixed defect that curl documents and works around.

`select` reports it in its exception set, which is what the connect path reads.
There is a test asserting `connection_refused` that would fail under WSAPoll.

## What this costs, written down rather than discovered

- Both pollers are **O(n) in pending operations per wait**.
- `select` on Windows watches at most `FD_SETSIZE` sockets — raised to 1024 here, since the default of 64 is far too
  small.
- The POSIX path caps at 4096 watched descriptors per wait; the rest wait for the next round.

That is fine for a dev server and an HTTP client with a connection pool.
It is not fine for ten thousand connections.

## What would make the swap worth doing

Any one of these, and not before:

- A **connection count in the thousands** — a server that is no longer a dev server, or a multiplayer host.
- A **profile showing the poll sweep costing real time**, rather than an assumption that it must.
- **Datagram throughput** at a rate where per-wait rebuilding of the descriptor set shows up, since the datagram path
  ([cnet-datagrams.md](cnet-datagrams.md)) is the one with a packet rate rather than a request rate.

## What the swap involves

Nothing above `impl::reactor` changes.
Its public shape — `submit`, `cancel`, `wait`, `wake`, `pending_count` — is what the io_system actor uses, and none
of it is readiness-flavoured.

The work is per-platform and does not overlap:

- **Windows, IOCP.** Association per socket, plus `ConnectEx` and `AcceptEx` obtained through `WSAIoctl`, and an
  `OVERLAPPED` per operation.
  Note that `io_operation` currently has no platform scratch space; adding one is the first change.
- **Linux, epoll.** Readiness edges converted to completions by doing the read or write inside the reactor, which is
  what the current poller already does — so this is the smaller of the two.
- **macOS, kqueue.** Same shape as epoll.

Follow the repo pattern already used here and by `sr::window_system`: **one concrete class, implementation swapped by
compiling a different source file**, rather than an interface with subclasses.
`cc::unique_ptr` deliberately has no upcast, so polymorphic ownership is not available anyway.

Keep the shared poller as the fallback for platforms without a native mechanism.
