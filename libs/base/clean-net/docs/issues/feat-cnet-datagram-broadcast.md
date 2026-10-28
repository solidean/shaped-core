---
created: 2026-10-27
refs:
  - file: libs/base/clean-net/src/clean-net/transport/datagram.hh
---

# cnet: broadcast on datagram sockets (SO_BROADCAST)

LAN discovery ("find servers on this network") wants a datagram sent to the subnet's broadcast address, which the OS refuses unless `SO_BROADCAST` is set.
One flag on `cnet::datagram_options`, and the sending side is otherwise unchanged.

Known: `cnet::virtual_network` cannot represent it without a subnet model, so the virtual half needs a decision of its own — refuse it as `unsupported`, or deliver to every socket on the port.
Left out of the first datagram change on purpose, as additive.
