---
created: 2026-10-17
parent: epic-sg-webgpu-gaps
tags: [webgpu]
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_query.cc
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_query.hh
---

# Each webgpu query set's last slot is a discard target

emdawnwebgpu passes `WGPU_QUERY_SET_INDEX_UNDEFINED` to JS as 4294967295 rather than as an absent field.
Dawn tolerates that, and wgpu refuses it as out of bounds.
So every query set spends its last slot as a discard target until emdawnwebgpu is fixed.
The external bug is [webgpu-timestamp-write-index-sentinel](../../../../../docs/bugs-external/webgpu-timestamp-write-index-sentinel/readme.md).
