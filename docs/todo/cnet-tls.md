# cnet: TLS, and the trust store that is the actual problem

**Status:** decided, not built.
The largest single chunk before a download works.

## Two halves that get conflated

The **handshake and record layer** is the cryptography: key exchange, ciphers, framing.
Several good libraries do it.

The **trust decision** answers "is this certificate chain acceptable for this hostname", and needs a set of root
certificates the machine's owner has decided to trust.
Every operating system keeps its own, reachable only through its own API, and **none of them is a file a portable
library can read**.

The second is the harder half, and no library does it for you.

## Why "just use the system TLS" is not an answer

| platform | handshake API | trust store |
|---|---|---|
| Windows | Schannel — usable, ugly, stable | CryptoAPI, `CertGetCertificateChain` |
| macOS / iOS | Secure Transport is deprecated; Network.framework is connection-shaped, not stream-shaped | `SecTrustEvaluateWithError` |
| Linux | none — OpenSSL is a package, not a platform API, and its version varies by distro | a directory of PEM files whose path varies by distro |
| Android | none in the NDK — TLS is Java-side | Java-side, through JNI |
| wasm | the browser does it, invisibly | the browser's |

**Three of the six have no system handshake API at all.**
So a system-only design means writing three backends *and* vendoring a library for the rest — strictly more work than
vendoring one library and using it everywhere except wasm.

Meanwhile the trust store must be per-platform whichever handshake library is used, because that is where the roots
live.

## The decisions

**mbedTLS, vendored, everywhere except wasm.**
Small, plain CMake, no external toolchain, and it builds unchanged on every target.
Slower on bulk transfer than the alternatives, and no QUIC.

Build simplicity dominates here in a way it would not in a server product: this repo builds on eight
platform/compiler combinations in CI including wasm, Android and iOS, and BoringSSL's Go-and-Perl build is a
recurring cost across all of them.

**Vendored rather than fetched.**
A build that cannot do HTTPS is not a working build of this library, which is the same argument
[extern/CMakeLists.txt](../../extern/CMakeLists.txt) already makes for zstd and zlib.
The cost is that the pin moves on a security cadence rather than a convenience one — true either way, and at least a
vendored pin is visible in the tree.

**The seam exists from day one, with one implementation.**
TLS behind an interface costs one indirection per record batch and nothing measurable; adding it later means touching
every place a handshake or a record is driven.

**Platform trust stores only, and no shipped root bundle.**
A bundled root set goes stale, ignores the enterprise roots a corporate MITM proxy needs, and ignores the user's own
decisions.

**`allow_any_certificate` is settable from code only** — never from a URL, an environment variable or a config file,
so it cannot be turned on in the field.

## What would overturn the mbedTLS choice

Named now so the decision is not silently revisited:

- **QUIC or HTTP/3.** mbedTLS cannot do it; that is a BoringSSL-shaped requirement.
- **A throughput target in gigabits.** Then the record layer starts to matter.

On performance, **measure before adding a second backend**.
With AES-NI and the ARMv8 crypto extensions enabled, mbedTLS's bulk gap is far smaller than its reputation.
Verify the hardware path is actually compiled in, benchmark a large download against the loopback server, and record
the number in the library docs so the next person does not re-litigate it from intuition.

Note that `BENCHMARK` in nexus implies `main_thread` and there is no macro for a threaded or async benchmark yet, so
the throughput measurement has to be a plain `TEST` with `nx::config::benchmark` and no `main_thread`.

## The shape

```cpp
namespace cnet
{
/// What a connection is willing to trust. The platform store is the default and the only correct default.
struct tls_trust
{
    bool use_system_roots = true;
    cc::vector<cc::string> additional_roots_pem;  // a private CA, or a test fixture

    /// Never a default, and never settable from configuration -- this exists for a test against a self-signed fixture.
    bool allow_any_certificate = false;
};

struct tls_options
{
    tls_trust trust;
    cc::vector<cc::string> alpn;              // in preference order
    cc::optional<tls_identity> client_identity;
};
}
```

**No TLS library type in a public header**, exactly as zstd and libspng are kept private today.
The escape hatch is an optional header carrying a `kind` enum plus an opaque handle the caller casts back, documented
as **read-only interop**: reading state, exporting keying material, feeding a profiler.
Anything a caller does through the native handle that *changes* behaviour is behaviour the other backends do not
have, and becomes a portability bug shaped like a feature.

## The license trap — read before writing `dependency.yml`

mbedTLS is **`Apache-2.0 OR GPL-2.0-or-later`**.

`tools/deps/deps.py` splits a license expression and requires **every** branch to be on the allowlist, since we may
end up relying on any of them.
`GPL-2.0-only` is explicitly on the denied list in [license-policy.yml](../../tools/deps/license-policy.yml).

So a literal `license: Apache-2.0 OR GPL-2.0-or-later` **fails the `deps-licenses` gate**.

Declare `license: Apache-2.0` — the branch we take — and record the dual licensing in `notes:`.
Do not widen the allowlist.

## Mechanics

Model on [extern/libspng](../../extern/libspng/) — a vendored git pin.
Copy `_force_rmtree` verbatim from its vendor script: git packs `.git` objects read-only, and `shutil.rmtree` fails on
Windows without it.

Vendored means **no `.install/pin.txt` gate and no `prereqs.py` entry**; the target always exists and the link is
unconditional.

Then `uv run dev.py deps licenses` to write `docs/licenses/mbedtls.txt` and regenerate the index.

mbedTLS will be the second-largest vendored dependency after zstd, so the new `SC_USE_VENDORED_MBEDTLS` option needs a
comment making the always-present argument, the way the existing options do.

## Trust-store adapters

- Windows: `CertGetCertificateChain`.
- Apple: `SecTrustEvaluateWithError`.
- Linux: the distro PEM paths, which vary and must be probed rather than assumed.
- Android: best-effort stub until that tier is real; the real answer is JNI.
