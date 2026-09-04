# TLS, and the trust store that is the actual problem

How a connection becomes an encrypted one, and where the roots come from.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## Two halves that get conflated

The **handshake and record layer** is the cryptography: key exchange, ciphers, framing.
Several good libraries do it.

The **trust decision** answers "is this certificate chain acceptable for this hostname", and needs the set of root
certificates the machine's owner has decided to trust.
Every operating system keeps its own, reachable only through its own API, and **none of them is a file a portable
library can read**.

The second is the harder half, and no library does it for you.

## Why not the system TLS

| platform | handshake API | trust store |
|---|---|---|
| Windows | Schannel — usable, ugly, stable | CryptoAPI, `CertGetCertificateChain` |
| macOS / iOS | Secure Transport is deprecated; Network.framework is connection-shaped, not stream-shaped | `SecTrustEvaluateWithError` |
| Linux | none — OpenSSL is a package, not a platform API | a directory of PEM files whose path varies by distro |
| Android | none in the NDK — TLS is Java-side | Java-side, through JNI |
| wasm | the browser does it, invisibly | the browser's |

**Three of the six have no system handshake API at all.**
So a system-only design means writing three backends *and* vendoring a library for the rest — strictly more work than
vendoring one library and using it everywhere except wasm.

Meanwhile the trust store must be per-platform whichever handshake library is used, because that is where the roots
live.

## Mbed TLS, vendored

Small, plain CMake, no external toolchain, and it builds unchanged on every target.
Build simplicity dominates here in a way it would not in a server product: this repo builds on eight
platform/compiler combinations in CI including wasm, Android and iOS, and BoringSSL's Go-and-Perl build is a recurring
cost across all of them.

What would overturn that choice, named so it is not silently revisited:

- **QUIC or HTTP/3.** Mbed TLS cannot do it; that is a BoringSSL-shaped requirement.
- **A throughput target in gigabits.** Then the record layer starts to matter, and the number should be measured
  before a second backend is added rather than argued from reputation.

[extern/mbedtls/](../../../../extern/mbedtls/) has the pin and the vendoring script, and
`shaped_mbedtls_config.h` there is the configuration — every line of it a subtraction from upstream's defaults.

**It is configured thread-safe**, which it is not by default.
Its PSA layer keeps process-wide state that every TLS 1.3 handshake goes through, so two handshakes at once corrupt
it — and the damage presents as one handshake in a few dozen failing for no visible reason, which is the worst way
for a bug to show up.
clean-net supplies the mutex implementation, since upstream ships a pthreads one and Windows has no pthreads.

## TLS is a wrapper, not a transport

`cnet::tls_connect` takes a connection and hands back a connection.

That falls out of the [transport seam](transport-seam.md), and it is what makes the handshake testable: over
`native_transport` it is HTTPS, over `virtual_network` it is a real handshake with no socket in sight, and over
`simulated_transport` it is a handshake on a link that drops records.
Every TLS test in this library runs over the virtual network, so none of them needs a port, a server, or a
certificate anybody has to remember to renew.

**No Mbed TLS type appears in a public header**, exactly as zstd and libspng are kept private.
Anything a caller could do through a native handle that *changes* behaviour is behaviour the other backends do not
have, and becomes a portability bug shaped like a feature.

## The pump

Mbed TLS is a synchronous state machine that reads and writes through two callbacks and answers WANT_READ or
WANT_WRITE when they cannot proceed.
Nothing in this library may block, so those callbacks never touch a connection: they move bytes to and from two
buffers, and a pump drives the state machine and the real connection alternately until something finishes.

One step drives the record layer as far as it goes, collects what it produced, and only then — outside the lock —
starts the underlying operations and completes the promises.
Both halves of that matter.
An underlying operation can complete **inline** (a virtual connection does), so a continuation re-enters the pump
while the first call is still on the stack; and a promise's continuation can do anything at all, including calling
back into the same connection.

## Trust

**The platform store, and no shipped root bundle.**
A bundled root set goes stale, ignores the enterprise roots a corporate MITM proxy needs, and ignores the user's own
decisions.

The store is read per handshake rather than cached: a machine's trust changes while a program runs — an enterprise
policy push, a developer trusting a proxy — and a cache would mean restarting the program before it believes what its
owner already decided.
A few hundred certificates cost about a millisecond against a handshake that costs tens.

An adapter that is not written yet reports `unsupported` rather than an empty list.
No roots and "I could not ask" are the same set and very different facts, and only the first should let a caller
believe a connection was verified.

| platform | trust store |
|---|---|
| Windows | `CertOpenSystemStore("ROOT")`, which is the union group policy resolves |
| macOS | `SecTrustSettingsCopyCertificates` over all three domains — system, admin and user |
| Linux | the distro CA bundle, probed rather than assumed: the path varies and there is no standard |
| iOS | none — see below |
| Android | none — the roots live behind JNI |

The three macOS domains are a union rather than a fallback: Apple ships one, an administrator adds to another, and a
user trusts things in the third — and a corporate proxy's certificate lives in one of the latter two.

**iOS and Android need a different seam, not a missing adapter.**
Neither can enumerate its anchors at all; the supported path on both is to hand the OS a built chain and let *it*
decide — `SecTrustEvaluateWithError`, or the Java trust manager.
That is a verify callback rather than a set of roots, and it is what those two will need when their turn comes.

Until then a caller there supplies its own roots through `tls_trust::additional_roots_pem`.

Roots arrive in two forms, because that is how the platforms keep them: Windows and Apple hand over parsed
certificates as DER, while a Linux trust store is a file of concatenated PEM.
The parser takes both, a distro bundle is parsed as one blob, and one bad certificate in it does not disqualify the
rest.

**`allow_any_certificate` is settable from code only** — never from a URL, an environment variable or a config file,
so it cannot be turned on in the field by anything but a recompile.
It exists for a test against a self-signed fixture, and the one test that uses it is the only place in this library
it appears.

## A certificate of your own

`cnet::tls_make_self_signed` generates a P-256 identity in about a millisecond, for a loopback dev server and for a
test.

Its validity window is fixed — 2020 through 2035 — rather than relative to now.
A certificate minted for "the next hour" is a test that fails on a machine whose clock is off, and this is not a
certificate anybody should be relying on for freshness.
