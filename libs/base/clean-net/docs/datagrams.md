# Datagrams

Whole messages to and from any peer, with no connection, no ordering and no delivery promise — UDP, and the two transports that stand in for it.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## The shape: poll-and-batch, not an async per packet

A game sends and receives small packets 30 to 120 times a second per peer, and acts on them inside the same frame.
Modelling each packet as an asynchronous operation would give every one an allocation, a state machine and a completion callback, to carry sixty bytes.
So a packet is a record in a caller-owned array, and the two calls that move them are plain synchronous calls that never block and never allocate.

```cpp
auto sock = cnet::datagram_socket::create(io, cnet::endpoint(cnet::ip_address::any(cnet::ip_family::v4), 7777));

// Set up once, reused every frame: `buffer` is the room, and a receive never changes it.
auto storage = cc::vector<byte>::create_defaulted(64 * 2048);
auto records = cc::vector<cnet::datagram>::create_defaulted(64);
for (isize i = 0; i < 64; ++i)
    records[i].buffer = cc::span<byte>(storage).subspan(i * 2048).first_n(2048);

auto const n = sock->receive_batch(records).value();      // 0 when nothing was waiting
for (auto const& d : cc::span<cnet::datagram>(records).first_n(n))
    handle(d.peer, d.bytes(), d.received_at_ns);

auto const packet = cnet::outgoing_datagram{.peer = server, .bytes = payload};
sock->send_batch(cc::span<cnet::outgoing_datagram const>(&packet, 1));
```

**Each record carries its own room** rather than the call carving one buffer into equal slots, so slots can come from a pool, a ring or anything else.
The room (`buffer`) and the result (`size`, `bytes()`) are separate fields on purpose.
A record whose span the call shrank in place would read into the previous datagram's size on the next frame, and every caller would write a reset loop.

**Variable sizes do not need variable slots.**
Sizes vary all the time, but a receiver cannot know which datagram comes next or how large it is, so every slot has to fit the largest expected one anyway.
A datagram larger than its room is cut, and `truncated` says so; silently cut data is the one corruption a caller could not otherwise detect.

## What counts as a failure

**Loss, duplication and reordering are data, never errors.**
A packet that did not arrive produces nothing.

Both calls return `cc::result<isize, error>`, and an error means the socket is unusable or a datagram was refused outright: too large, no address, no route.
A closed socket that read as "0 datagrams" would leave a server polling forever with only a log saying why.

**A partial send is a count, and the next call says why it stopped** — the `writev` rule.
A full OS buffer stops the batch with no error, since dropping or retrying the rest is the same decision as an ordinary lost packet.
Any other failure is an error only when it hit the first packet; otherwise the count so far comes back, and the next call starting at that packet reports it.
So an error never hides how much was already sent.

**A peer that went away is silence.**
Windows otherwise fails the *next* receive with `WSAECONNRESET` once the ICMP "port unreachable" for an earlier send arrives, which reads as a broken socket.
The socket turns that off at creation (`SIO_UDP_CONNRESET`, and `SIO_UDP_NETRESET` for "TTL expired"), and the receive path skips both errors anyway.
Linux and macOS report nothing on an unconnected socket.

## Waiting: a level-triggered arrival callback

A game polls once per frame and needs nothing else.
A server, a network thread or a test has to sleep until something arrives, and a sleep loop either spins a core or adds its interval to every packet's latency.

`on_arrival(callback)` calls back whenever datagrams are waiting:

```cpp
sock->on_arrival([&](cnet::datagram_socket& s) {
    auto const n = s.receive_batch(records).value();
    hand_to_game_thread(cc::span<cnet::datagram>(records).first_n(n));
});
```

- **It is a readiness notification, not a delivery.**
  The callback calls `receive_batch` itself, so caller-owned records stay the one buffer contract whether a caller polls or is called.
- **Level-triggered**, like the OS readiness it reports: a callback that leaves datagrams unread is called again at once.
- **One call per wake, not per datagram.** A burst of fifty is one call that drains fifty.
- **It runs on the reactor thread**, or on whichever thread drives `cc::thread_pump_all()` when the io_system has none — so it must be quick.
- **Replacing, clearing and destroying wait** for a call already running on another thread; from inside the callback they do not, since waiting there would never end.
  After `io_system::stop()` it is never called again.

Underneath, it is a loop of one-shot readiness waits, `datagram_backend::wait_readable`: wait, call, wait again.
Natively that is a reactor `readable` operation, which completes when the socket has something to read and reads nothing.
**A backend's wait must never complete inline**, only from a later reactor wait, because the loop re-arms from its own completion and would otherwise recurse for as long as datagrams keep arriving.

**Polling and the callback can run at the same time.**
`receive_batch` may run on several threads at once and each datagram goes to exactly one of them, so a game thread may poll while the callback drains on the reactor thread.
A callback that finds the poller already took everything simply receives 0.

**What each costs**, measured by the `cnet - datagrams: polling against the arrival callback` benchmark in `tests/datagram-test.cc`.
Windows loopback, release build, a Ryzen 9 9950X3D at 13% load; a burst is 16 datagrams of 64 bytes, sent and then drained:

| | per iteration |
|---|---|
| a poll with nothing waiting | 0.32 µs |
| a burst, drained by polling | 58.7 µs |
| a burst, drained by the callback | 61.2 µs |

So a frame that finds nothing costs one failed syscall, and the callback adds about 2.4 µs per wake over polling: the reactor's wait, the completion, and four small allocations.
That is per burst, not per datagram, since one wake drains whatever has arrived.

There is no `wait_writable`.
It would let a bulk sender wait for buffer room instead of dropping, and nothing needs it yet; it is purely additive later.

## Arrival times

`received_at_ns` is on the io_system's clock, the same one every deadline and simulated delay is measured on, so a test with a `manual_clock` asserts exact arrival times.
Natively it is read right after the syscall that returned the datagram, so a datagram that waited in the socket buffer carries the time it was read.
A caller that cares drains promptly through `on_arrival`.

Kernel arrival stamps (`SO_TIMESTAMPNS` on Linux, `SO_TIMESTAMP` on macOS) would improve the accuracy without changing what the field means.
They are not built: they need a per-platform path and a wall-clock-to-monotonic conversion, and are meaningless under a manual clock.

## The syscalls

Linux moves a batch in one `recvmmsg` or `sendmmsg` call of up to 64 records.
Every other platform loops one datagram per syscall: `recvmsg` on POSIX, since only its message flags report a cut datagram, and `recvfrom` on Windows, which reports one as `WSAEMSGSIZE`.
The interface is the same either way.

**The Linux arm has not run on a developer machine yet**; CI builds it and the datagram tests exercise it there.
Windows' high-throughput path, Registered I/O, is a separate API and not built.

## The three transports

`datagram_socket` is a handle over `cnet::datagram_backend`, and `cnet::transport::bind_datagram` is where one comes from, beside `connect` and `listen`.
So one `simulated_transport` covers a test with a TCP lobby and a UDP game channel alike.

| | what it is |
|---|---|
| `native_transport` | the platform's sockets; `unsupported` on wasm |
| `virtual_network` | in-process: a send appends a copy to the inbox of whoever is bound at the destination, instantly |
| `simulated_transport` | loss, duplication, reordering, latency, jitter and bandwidth over either of the others |

The virtual network routes and binds the way a stack without address reuse does: an exact endpoint first, then a socket bound to every interface on that port.
A bind clashes with one on the same port and family when either is bound to every interface, and a bind to port 0 skips ports already taken.
A datagram to an endpoint nobody bound vanishes, and one larger than 65 507 bytes is refused, so a test cannot pass with a datagram no real socket would send.

Two known differences remain: `datagram_options` is not read, so a v6 socket bound to every interface never receives v4 even with `v6_only = false`, and the 65 507-byte cap applies to IPv6 as well.

**wasm has no datagram socket and never will**, so the native one reports `unsupported` there rather than disappearing.
The virtual and simulated ones need no socket and work there, so tests built on them run in every build.
What a browser game ships on is the message channel above this layer (`docs/todo/cnet-datagrams.md` at the repo root), whose weaker promise a WebRTC data channel can keep.

## Where a simulated link impairs a datagram: where it is received

Each arrival is decided once — dropped, duplicated, delayed — and then held until its release time, which `receive_batch` and the readiness wait both read.

- **No timer per datagram.** A timer exists only while a readiness wait is parked, and only for the earliest release.
- **In a test where every socket comes from one simulated transport, each datagram crosses exactly one impairment**, so a 10% loss setting means 10%.
  Impairing on both sides would make it 19%.
- **Over the native transport only inbound traffic is impaired**: a client simulated against a real server sees a bad downlink and a clean uplink.
  A send-side mode can be added behind an option without changing what this one means.
- **Jitter alone reorders**, since datagrams are released in the order their delays end, as on a real network.
- **Reordering is one datagram taking a slower path** on top of that: an extra delay of one to two times its own, at least a millisecond, so later ones overtake it.
- **Bandwidth is a delay by size, not a ceiling**: datagrams in flight together are not queued behind one another.
- **The arrival time is the release time**, exactly.

## Not built yet

Each is additive and changes nothing above:

- broadcast (`SO_BROADCAST`), which `virtual_network` cannot represent without a subnet model;
- multicast groups, a join/leave API of its own;
- connected sockets, which filter to one peer and report ICMP errors;
- kernel arrival timestamps, and Windows Registered I/O;
- `wait_writable`, and a send-side impairment mode.
