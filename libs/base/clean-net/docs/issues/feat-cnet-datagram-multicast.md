---
created: 2026-10-27
refs:
  - file: libs/base/clean-net/src/clean-net/transport/datagram.hh
---

# cnet: joining and leaving multicast groups on a datagram socket

Service discovery (mDNS and the like) joins a multicast group and receives what is sent to it.
That is a join/leave API with interface selection (`IP_ADD_MEMBERSHIP`, `IPV6_JOIN_GROUP`), plus the outgoing interface and TTL, so it wants a small design of its own rather than a flag.

The virtual network would need a group registry to route to.
Left out of the first datagram change on purpose, as additive.
