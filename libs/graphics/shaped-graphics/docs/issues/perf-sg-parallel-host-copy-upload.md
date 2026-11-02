---
created: 2026-10-17
parent: epic-sg-transfer-gaps
---

# A parallel host copy for a large inline upload

Take a `cc::pinned_data`, copy it on worker threads, and block at submit rather than inside `bytes_to_buffer`.
