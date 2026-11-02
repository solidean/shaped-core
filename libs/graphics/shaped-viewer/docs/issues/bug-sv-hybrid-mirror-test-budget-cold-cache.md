---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-viewer/tests/hybrid-test.cc
  - test: shaped-viewer-test
---

# The hybrid mirror-floor test exceeds its 45 s budget in debug when its shaders compile cold

Measured on Windows, debug-nopch-clang, a dx12 hardware adapter, at 5b4ca3604:

- The test is "sv::hybrid_routine - a mirror floor near the origin, seen from far off it, reflects the sky the same forward and deferred".
  It failed its `current_time_steady_secs() - start < 45.0` check at 60.6 s, in a gate run with `SC_BLOB_CACHE=off`.
- Run alone right after, with the default cache, it failed again at 62.4 s; the merge before had changed what a shader's key holds.
- Every run after that passed in 1.2 to 2.2 s.

So the budget holds with a warm shader cache and not with a cold one, where debug compiles the routine's shaders inside the timed span.
A run with the cache off, which bug-sv-test-heap-corruption-with-real-cache suggests as a workaround, fails it every time.
Either the timed span starts after the shaders are built, or the budget is for a warm cache and says so.
