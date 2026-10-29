---
created: 2026-10-27
refs:
  - file: libs/base/clean-net/src/clean-net/impl/native_socket.cc
---

# cnet: Windows Registered I/O for batched datagrams

On Windows every datagram costs one `recvfrom` or `sendto` syscall; Linux batches up to 64 in one `recvmmsg` / `sendmmsg`.
Windows' batched path is Registered I/O (`RIOReceive`, `RIOSend` over registered buffers, with completion queues), a different API that would need the caller's records registered with the OS.

Not measured: no workload here has needed more than a few hundred datagrams a second per socket, where one syscall each is negligible.
Worth doing once a server with many peers shows datagram syscalls in a profile.
