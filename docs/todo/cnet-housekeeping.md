# cnet: the smaller pending things

**Status:** each is decided and small.
One of them is a real defect in babel, found while building this.

The parser fuzzer is **done** — `tests/http1-fuzz-test.cc`, over nexus's API-sequence engine.
So are both examples, `clean-net/download` and `clean-net/dev-server`.
So is the flaky test that used to time out about once in 35 runs: two lost wakeups in `virtual_network`, and a test
that advanced a manual clock past a deadline nobody had computed yet.

## A `doctor` line for the networking environment

`uv run dev.py doctor` should name the TLS backend and which HTTP backends this machine offers — advisory,
tri-state, never a failure.

Model it on [graphics.py](../../tools/dev/lib/toolchain/graphics.py), whose module docstring is the design statement
for exactly this situation: the library degrades rather than disappearing, so a gap is invisible until something does
not work.
A new `network.py` beside it, spliced into `doctor()` the same way.

The checks worth having, and the shapes to copy:

- **The TLS backend**, once mbedTLS is vendored — a fetch-marker style check, though vendored means it is always
  there, so this reports the version rather than presence.
- **A system libcurl**, via `ctypes.util.find_library` — the same shape `_vulkan_runtime_check` uses for the Vulkan
  loader, and the closest existing analogue to "is there a system libcurl".
- **Which HTTP backends this build compiled in**, so the automatic order in
  [cnet-http-backends.md](cnet-http-backends.md) is inspectable rather than inferred.

CI runs doctor purely informationally with `continue-on-error`, so nothing here can break a build.

## A precompiled-header tier

Not yet: the library is too small to measure one honestly.

When there is enough, declare it next to the targets that use it —
`sc_declare_pch_tier(CNET EXTENDS CC_STD HEADERS <clean-net/all.hh>)` — and **pick it with
`uv run dev.py compile-time pch`, never by eye**.
A bigger tier is not automatically better, and choosing by eye is how a bigger one gets chosen than the numbers
support.

`src/clean-net/all.hh` already exists as the umbrella such a tier would name.

---

## A real defect elsewhere: `DATA:` in the glTF reader

**Not a cnet item.** Found while evaluating whether the glTF reader should move onto `cc::uri`, and worth fixing on
its own.

`is_data_uri` in [gltf.cc](../../libs/data/babel-serializer/src/babel-serializer/geometry/gltf.cc) matches the prefix
literally:

```cpp
bool is_data_uri(cc::string_view uri) { return uri.starts_with("data:"); }
```

A URI scheme is **case-insensitive** (RFC 3986 section 3.1), so a `DATA:` URI is currently treated as an external
reference: handed to `read_options::resolve_uri`, or recorded as unresolved.

Three lines and a test.
It changes reader behaviour, which is why it was raised rather than done.

### Why the glTF reader did *not* move onto `cc::uri`

Recorded so the question is not reopened from scratch.

The claim that motivated it — that the reader hand-parses URIs — was overstated.
It is a prefix match plus a `find(";base64,")`; there is no RFC 3986 parser there to replace.

And routing data URIs through `cc::uri_view::parse` would be a **regression**: `base64::decode` deliberately skips
whitespace, so a data URI with a wrapped payload decodes today, while the URI parser rejects raw whitespace outright.

The percent-decoding gap is not a gap either — it is a documented decision.
[gltf.hh](../../libs/data/babel-serializer/src/babel-serializer/geometry/gltf.hh) states that the URI arrives exactly
as written, and that percent-decoding and joining against a base path are the resolver's job, because babel owns no
filesystem policy.
