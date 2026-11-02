---
created: 2026-10-27
refs:
  - file: libs/base/clean-net/src/clean-net/transport/simulated_transport.hh
---

# cnet: a simulated link whose bandwidth is a queue, not a per-operation delay

`link_conditions::bandwidth_bytes_per_sec` turns into a delay proportional to each operation's own size, with nothing queued behind it.
So ten datagrams sent at once all arrive one datagram's delay later, and aggregate throughput is unbounded; the stream half behaves the same per read and write.

A real ceiling is a serialization queue per direction: each operation starts transmitting when the previous one finished, and its delay is the queue's backlog plus its own size.
That is what a test of congestion behaviour (a sender that must back off) needs, and what nothing here needs yet.
The doc on the field says what it does today.
