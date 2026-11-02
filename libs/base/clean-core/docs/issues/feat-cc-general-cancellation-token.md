---
created: 2026-10-17
tags: [async]
refs:
  - file: libs/base/clean-net/src/clean-net/common/cancel.hh
  - symbol: cnet::cancel_token
  - file: libs/base/clean-core/docs/systems/async.md
---

# No general cancellation token

`cc::async` can fail with a `cancelled` error, but nothing lets a caller *ask* running work to stop.
`cnet::cancel_token` is the one token in the repo — copyable, thread-safe, hierarchical — and it is scoped to clean-net's socket operations.
The first user outside clean-net is `sgl lsp`, which stops a stale test run with a bare `cc::atomic<bool>`, read between tests and by SGL's interpreter.

Generalising cnet's token into clean-core is the candidate.
[async.md](../systems/async.md) lists cancellation propagation through a graph as a follow-up beside it.
