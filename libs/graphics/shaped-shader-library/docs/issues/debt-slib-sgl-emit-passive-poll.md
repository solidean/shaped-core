---
created: 2026-10-17
blocked-by: [feat-cc-async-passive-timed-wait]
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/impl/sgl_emit_cache.cc
  - symbol: passive_get
  - symbol: cc::try_async_blocking_get_for
---

# The SGL emit cache polls its store by hand

`passive_get` waits for the persistent store's answer with a hand-rolled poll, standing in for a passive timed wait on a `cc::async` node, which clean-core lacks.
`cc::try_async_blocking_get_for` is the timed wait it has, and that one drives the ambient scheduler, which this must not.
A compile may run where its caller holds a lock or a reference that other work could invalidate.
Replace the loop with a passive timed wait once clean-core has one.
