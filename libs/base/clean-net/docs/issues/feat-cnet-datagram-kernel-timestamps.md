---
created: 2026-10-27
refs:
  - file: libs/base/clean-net/src/clean-net/transport/datagram.hh
---

# cnet: kernel arrival timestamps for received datagrams

`cnet::datagram::received_at_ns` is read from the io_system's clock right after the receive syscall.
So a datagram that waited in the socket buffer carries the time it was read rather than the time it arrived.
A jitter buffer wants the arrival.

The kernel can stamp each datagram as it arrives: `SO_TIMESTAMPNS` on Linux (wall clock, nanoseconds) and `SO_TIMESTAMP` on macOS (microseconds).
Windows has only `SIO_TIMESTAMPING`, on recent versions with a capable NIC.
Each needs `recvmsg` control messages and a conversion from the wall clock into the io_system clock's timebase, which is approximate.

The field's meaning stays "io_system clock, as early as the platform allows", so this only improves accuracy.
Under a `manual_clock` the kernel stamp is meaningless, so the clock-read path stays for that case.
