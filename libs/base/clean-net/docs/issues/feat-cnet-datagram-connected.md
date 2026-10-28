---
created: 2026-10-27
refs:
  - file: libs/base/clean-net/src/clean-net/transport/datagram.hh
---

# cnet: connected datagram sockets, filtered to one peer

A client with exactly one server benefits from `connect()` on its datagram socket.
The kernel then drops datagrams from anyone else, and reports ICMP errors such as "port unreachable" on the next call.

It is a second mode on every call: a send needs no endpoint, and a refusal becomes a reportable error rather than silence.
So it wants deciding how that surfaces on `cnet::datagram_socket` without muddying the unconnected contract, where loss is never an error.
Left out of the first datagram change on purpose, as additive.
