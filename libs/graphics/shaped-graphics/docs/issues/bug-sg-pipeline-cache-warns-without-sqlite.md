---
created: 2026-11-05
tags: [webgpu, wasm]
refs:
  - symbol: sg::pipeline_cache::resolve_blob_cache
  - file: libs/data/blob-cache/src/blob-cache/impl/cache_actor.cc
---

# The pipeline cache opens the default blob cache on wasm, which warns that it has no SQLite

On `emscripten-webgpu-release` under node, the first cached pipeline makes `bcache` log `cache degraded to always-miss: error: no SQLite backend was compiled in`.
nexus counts it as an undeclared warning, so an example that ran correctly is reported as failed: seen with `shaped-graphics/ray-throughput` on 2026-11-05.
slib's SGL emit cache already declines its store under `__EMSCRIPTEN__` (`slib::impl::sgl_emit_store`); `pipeline_cache::resolve_blob_cache` does not.
Either the pipeline cache declines the default store where no SQLite backend exists, or `bcache` reports that case below a warning.
