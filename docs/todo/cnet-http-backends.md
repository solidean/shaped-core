# cnet: the HTTP backends, and the order they are chosen in

**Status:** decided, not built.

## Why there is more than one

Our own HTTP/1.1 over our own transport cannot exist on wasm, where there are no sockets.
A browser's `fetch` cannot take over a connection.
A system libcurl has HTTP/2 and proxy handling we are not going to write.

So the client is an interface, and which implementation answers depends on the platform and on what the caller asked
for.

## The automatic order

This is the maintainer's call and it **overrides the recommendation that was made** — worth recording, because the
reasoning behind the override is what a reader needs.

1. **An explicit override.** Never falls back: it fails when that backend is unavailable.
2. **The platform system backend**, where the platform has one that is always present — wasm `fetch`, later WinHTTP
   and NSURLSession.
3. **A system libcurl**, loaded at runtime, preferred on Linux whenever present.
4. **A vendored curl**, only in a build that opted into fetching one.
5. **Our native HTTP/1.1 — last.**

The native backend is deliberately the **baseline fallback**: it is there to guarantee that something always works,
not to be the usual answer.
Where a better-maintained stack is present, that stack runs.

**The consequence to watch, and it is the important one:** on a machine with a system curl, our own HTTP/1.1 code
rarely runs.
So the **test suite must pin backends explicitly rather than using `automatic`**, and the HTTP conformance tests
should sweep *every* available backend.
This is the single easiest way for the native backend to rot unnoticed.

## Runtime discovery means `dlopen`, not linking

Linking `-lcurl` makes the library refuse to start where it is absent, which is exactly the fragility being avoided.
The pattern is loading `libcurl.so.4` (or `libcurl.4.dylib`) by name at first use and resolving the dozen symbols we
need, with any failure meaning "this backend is unavailable" rather than an error.
`libcurl.so.4` has been ABI-stable since 2006.

**This would be the repo's first by-name dynamic load.**
Nothing in shaped-core calls `LoadLibrary` or `dlopen` today; the two `GetProcAddress` sites both resolve out of an
already-loaded module.
The nearest shape to copy is the cached-once, null-on-failure resolver in
[system_metrics.cc](../../libs/base/clean-core/src/clean-core/platform/system_metrics.cc).

**Expect LeakSanitizer noise** with no frame of ours to attribute, exactly as the Vulkan ICD already produces.
The existing answer is a narrowly-keyed entry in [lsan-suppressions.txt](../../tools/cmake/lsan-suppressions.txt),
not a `cc::leak_scope`.

## A system curl is not a known quantity

It is whatever the distribution shipped: any version, built against any TLS backend, possibly with HTTP/2 compiled
out, possibly carrying a distribution patch.
So preferring it makes our behaviour depend on the machine, and a bug report becomes "which curl do you have".

Two things make that acceptable rather than reckless, and both are required:

- **Probe rather than assume.** `curl_version_info` says what this copy actually supports.
- **Log the chosen backend and its version through `cc::rec` at client creation.**
  That is what turns "it is slow on my machine" into a five-second diagnosis.

## The API

```cpp
namespace cnet
{
enum class http_backend
{
    automatic,      // the documented order above; logs which one it chose
    native,         // our HTTP/1.1 over our transport. Always present off wasm
    system,         // WinHTTP, NSURLSession, or the browser
    system_curl,    // a system libcurl, loaded at runtime. Never linked, never shipped
    vendored_curl,  // a curl we fetched and built. Off by default
};

struct http_client_description
{
    /// `automatic` is the default.
    /// An explicit value is an override, and it FAILS rather than falling back: a test that asks for a backend gets
    /// it or gets an error.
    http_backend backend = http_backend::automatic;
};

/// What this build and this machine can actually provide, in the automatic order.
[[nodiscard]] cc::vector<http_backend> available_http_backends();
}
```

**An explicit override must not fall back.**
A test that asks for the native backend and silently gets curl is a test that proves nothing, and that is the single
most valuable property of the override.

## In scope now, and not

**In:** the wasm `fetch` backend, and the `dlopen`ed system curl.

The `fetch` backend should be written **early, as a stub that compiles and fails at runtime**, and filled in later.
That is what keeps the seam enforced by the type system from day one rather than assumed — it is the whole reason the
layering is shaped this way, and a seam nothing exercises is a seam that quietly stops fitting.

It also has to **register a pump**, so `cc::thread_pump_all()` stays the single drive point on every platform rather
than a threads-off special case.

**Not in:** WinHTTP and NSURLSession.
The seam exists and `available_http_backends()` simply will not list them.
They become worth writing when iOS and Android become real tiers — and on iOS a raw-socket HTTP client is a fight
with the platform, so that is when.

## The one thing levels cannot promise away

CORS.
A level-0 backend can be told to make a request that a *server* then refuses, and no amount of level checking
predicts that.
It is a documentation problem: name the forbidden-header and CORS restrictions in the library's own docs, or they
will read as our bugs.
