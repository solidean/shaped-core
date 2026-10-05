# blob-cache TODO

## Open

- **Test binaries that compile shaders open the real default cache, and keep it past the run.**
  The dxc and msl shader caches reach for `bcache::default_cache()` on their first miss, and no such test binary installs a `scoped_default_cache`.
  So shaped-rendering's, shaped-viewer's, slib's, ssc-dxc's and sg-vulkan's tests read and write the developer's own cache file, which `default_cache.hh` says a test must not.
  The lazily opened default is a function-local static, so it closes at exit, after nexus's end-of-run check.
  On a singlethreaded build its actor is unthreaded and registers a pump, which is the "1 thread pump(s) still registered after the run" warning those binaries print.
  `SC_BLOB_CACHE=off` silences it, and the persistent tier keeps its own tests with explicit caches.
  Two fixes were weighed: dev.py running tests with the cache off, and a nexus hook before its end-of-run checks that a test main closes a temp cache in.
  A hang at exit of shaped-rendering-test, seen once beside it, did not reproduce in 15 repeated runs; joining the actor during static destruction is the suspect if it returns.
