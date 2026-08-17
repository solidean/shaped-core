# Concept: values

A value is a **canonically-encoded, self-describing byte sequence**.
Not a tree of nodes, not a variant of owning types, not JSON.

This codec is not frozen: a general-purpose any-value format landing elsewhere could replace it, which would break the on-disk format rather than refactor it.
See [decisions.md](../decisions.md#the-codec-starts-in-vdoc-not-in-clean-core) for what that would and would not promise.

```text
value := tag byte | payload
```

| kind      | payload |
|-----------|---------|
| `null`    | empty |
| `boolean` | one byte, exactly `0` or `1` |
| `integer` | 8 bytes, little-endian, two's complement |
| `number`  | 8 bytes, the IEEE-754 binary64 bit pattern verbatim |
| `string`  | `u32` byte length, then UTF-8 bytes, unterminated |
| `bytes`   | `u32` byte length, then the bytes |
| `array`   | `u32` payload byte length, `u32` element count, then the elements back to back |
| `object`  | `u32` payload byte length, `u32` entry count, then entries; an entry is a `u32` key length, the key bytes, then the value |

A length prefix counts **the bytes that follow it** — the data for `string` and `bytes`, the count field plus the entries for `array` and `object`.
One meaning across all four kinds makes skipping a single rule, `5 + prefix`, whatever the tag — see [decisions.md](../decisions.md#a-length-prefix-counts-the-bytes-that-follow-it).

The container length prefixes exist so that **skipping a subtree is O(1)**, which is what makes reading one field of a large value cheap and comparing two values a length check followed by a memcmp.

## Equality is byte equality, and that is the whole point

Two values are equal exactly when their bytes are equal.
Hashing is over the bytes.
There is no structural comparison anywhere in the library.

That property is load-bearing far beyond convenience:

- diffing an edit against its parents is a memcmp, not a tree walk;
- content-addressing an op needs no separate canonicalization pass over values;
- deciding whether two concurrent writers *agreed* — the single hottest question the merge layer asks — is a memcmp.

Byte equality is only meaningful if each value has exactly one valid encoding, so the format is **canonical and decoding enforces it**:

- object keys are sorted ascending by byte order, and duplicate keys are invalid;
- a `boolean` payload other than `0` or `1` is invalid;
- container length prefixes must match their contents exactly, with no trailing bytes.

A non-canonical encoding is a **decode error**, not something tolerated.
Tolerating it would silently break equality, hashing, and every merge decision built on them.

Decoding also enforces a maximum nesting depth, so a hostile or corrupt input cannot drive the decoder into unbounded recursion.

## What the format deliberately does not have

- **No float canonicalization.** `NaN` payloads and the two zeroes are stored as written, and two `NaN`s with different payloads are different values.
  An application that cares normalizes before writing, and doing it here would mean rewriting a caller's number behind their back.
- **No shortest-form integers.** An integer is 8 bytes, always, so there is one encoding because there is one width.
- **No UTF-8 validation.** Canonicality is structural, and byte equality does not care whether the bytes are text.
  `string` says what the application means by the bytes, and validating that is the application's job, exactly like float normalization.
  See [decisions.md](../decisions.md#decoding-does-not-validate-utf-8).
- **No hash or reference type.** Asset references are ordinary strings — see [assets and blobs](assets-and-blobs.md#assets-are-loosely-coupled-by-design).
- **No round-trip obligation to JSON.** The library can *print* a value as JSON-ish text for a human, and that is a one-way debugging projection.
  JSON is the display metaphor for the model, never its storage.

## Values are small, and stored inline

Real values are a scalar, a small struct, or a short array — a position, a name, a colour, a handful of flags.
So a value's in-memory storage is `cc::small_vector<cc::byte, N>`.

`cc::small_vector` occupies 48 bytes and grows its inline buffer to fill whatever footprint it already has, so `N = 1` already yields roughly 36 inline bytes.
That covers essentially every real value without an allocation, and the ones it does not are correct, just heap-backed.

**Bulk data does not belong in a value.** A mesh, a texture or a point cloud is a blob, referenced by an asset id — see [assets and blobs](assets-and-blobs.md).
