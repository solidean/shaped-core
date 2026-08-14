# Milestone 1 — The value codec

**Goal.** `vdoc::value`: a canonically-encoded binary value where equality is byte equality, and decoding refuses anything non-canonical.

**Why first.** Everything above this rests on byte equality.
Op diffing is a memcmp, and content addressing hashes value bytes without a separate canonicalization pass.
So is the "did these two concurrent writers agree" check, which is the hottest question the merge layer asks.
Getting this wrong is not a local bug; it silently corrupts merge behaviour three layers up.

The design is [concept.md](../concept.md#values).
Depends on milestone 0 only for hashing, and only where a value is hashed.

---

## The encoding

```text
value := tag byte | payload
```

| kind | tag payload |
|------|-------------|
| `null` | empty |
| `boolean` | one byte, exactly `0` or `1` |
| `integer` | 8 bytes, little-endian, two's complement |
| `number` | 8 bytes, the IEEE-754 binary64 bit pattern verbatim |
| `string` | `u32` byte length, then UTF-8 bytes, unterminated |
| `bytes` | `u32` byte length, then the bytes |
| `array` | `u32` payload byte length, `u32` element count, then elements |
| `object` | `u32` payload byte length, `u32` entry count, then entries |

An object entry is `u32` key length, key bytes, then a value.

The container payload length is what makes **skipping a subtree O(1)**, which is what makes reading one field of a large value cheap and comparing two values a length check plus a memcmp.

## Canonicality, and why decoding enforces it

Exactly one encoding per value, enforced at decode:

- object keys sorted ascending by byte order, and no duplicate keys;
- a `boolean` payload that is neither `0` nor `1` is invalid;
- container length prefixes must match their contents exactly, with no trailing bytes;
- a nesting depth over the fixed maximum is invalid.

**Tolerating a non-canonical encoding is not leniency, it is a correctness bug.**
Two equal values with different bytes break equality, hashing, diffing and every merge decision built on them — silently, and far from here.

The depth limit exists so a corrupt or hostile input cannot drive the decoder into unbounded recursion.
Pick a fixed limit, state it in the header, and test the boundary.

## What the format deliberately omits

None of these needs a rule, and adding one would be a mistake:

- **No float canonicalization.** `NaN` payloads and both zeroes are stored as written.
  An application that cares normalizes before writing; the library does not rewrite a caller's number.
- **No shortest-form integers.** One width, so one encoding.
- **No hash or reference type.** Asset references are ordinary strings.
- **No JSON round-trip obligation.** Text output is one-way, for humans.

---

## Work items

1. **`value_kind`**, the tag enum, with the byte values pinned in the header — they are a format constant, not an implementation detail, and reordering the enum must not be able to change the format.
2. **`value_view`** — a non-owning view over encoded bytes.
   - `kind()`, the typed accessors (`as_bool` / `as_i64` / `as_f64` / `as_string` / `as_bytes`), container access by index and by key, `size()`.
   - `bytes()` — the canonical encoding, which is what everything durable commits to.
   - Byte equality and hashing.
3. **`value`** — the owning form, `cc::small_vector<cc::byte, N>` backed.
   Use `N = 1`: `cc::small_vector` is 48 bytes and grows its inline buffer to fill that footprint.
   So `N = 1` already yields roughly 36 inline bytes, which covers essentially every real value without allocating.
   A larger `N` buys nothing until it grows the struct, and growing the struct costs every property in the document.
4. **Scalar factories** — `value::of(...)` for each scalar kind, plus a null.
5. **`value_builder`** — incremental arrays and objects, nestable.
   The builder **sorts object keys on build** and rejects a duplicate key.
   That is the only place sorting happens; a decoder validates but never repairs.
6. **Decoding** — `try_decode(span<byte const>) -> cc::result<value_view, value_decode_error>`, validating canonicality in one pass.
   The error enum is string-free, naming what was wrong and the byte offset.
7. **Skip** — advance past a value without decoding it, using the length prefixes.
8. **Debug text** — a one-way JSON-ish projection for dumps and test failure output.
   It is explicitly not a serialization format, and nothing may parse it back — say so in the header.

## API surface this lands

```text
vdoc::value_kind
vdoc::value            owning, small-vector backed
vdoc::value_view       non-owning
vdoc::value_builder    arrays and objects, sorts keys on build
vdoc::try_decode       the only route from bytes to a value_view
vdoc::to_debug_string  one-way text projection
```

## Tests

The first `versioned-document-test` binary, which also means adding the test target to [CMakeLists.txt](../../CMakeLists.txt) and its `sc_add_nexus_web_runner` line.

- **Round-trip** every kind, including empty strings, empty containers, and the integer and float extremes.
- **Canonicality is enforced**, for hand-built bytes with unsorted keys, duplicate keys, a `boolean` of `2`, a wrong length prefix, or trailing bytes.
  Each must be a decode error, and each is a separate test.
- **Byte equality**: values built by different routes but semantically identical compare equal; the builder's key sorting is what makes an object literal order-independent.
- **Skip** lands exactly on the next value, for deeply nested and adjacent values alike.
- **Depth limit**: at the limit decodes, one over is an error, and neither overflows the stack.
- **Inline storage**: assert the actual inline capacity, so a future `cc::small_vector` change that silently pushes every value onto the heap is caught here rather than in a profile.
- **Fuzz** the decoder with `nx`'s fuzzing support: no input, however malformed, may crash, hang, or read out of bounds.
- **`NaN` and `-0.0` survive verbatim**, and two `NaN`s with different payloads are different values.
  This is documented behaviour, so it is pinned by a test.

## Acceptance

- Every non-canonical input in the tests is rejected, and no repair path exists anywhere in the decoder.
- Equality and hashing touch only bytes — there is no structural comparison in the library.
- A value holding a scalar or a small struct does not allocate.
- The fuzzer runs clean.
- [structure.md](../structure.md)'s `value` entry is `[done]`, and the cheat-sheet's value section has lost its `[planned]` marking.
