# Strings

`cc::string` and `cc::string_view`, and the contracts they share.

Per-member documentation lives in the headers; the [cheat-sheet](../cheat-sheet.md) lists them.
This page owns what would otherwise be restated at every call site: null-termination, when a pointer dies, how storage moves between inline and heap, and what hashes equal to what.

Turning values *into* strings is [formatting](formatting.md)'s topic.

## The two types

`cc::string` owns its bytes and copies deeply.
`cc::string_view` is a non-owning `char const*` plus `isize`, trivially copyable, and the type nearly every function should take when it only reads.

Conversion is implicit in **both** directions, and only one of them is free:

* `string_view sv = s;` views the string's bytes and costs nothing;
* `string s = sv;` **copies**, and allocates whenever the content does not fit inline.

Both come from `string_view`'s and `string`'s container constructors, which accept anything with `.data()` and `.size()`.
That is convenient and easy to do by accident, so a `cc::string` parameter where a `cc::string_view` would do is a silent copy at every call.

`string_view` is a borrowed range (`cc::enable_borrowed_range<string_view>` is true): its validity depends on the viewed storage, never on the view object.

## Bytes, not codepoints

`size()` counts bytes.
Embedded `'\0'` bytes are allowed and counted, and indexing is by byte.

Neither type does Unicode: no codepoint iteration, no grapheme clusters, no normalization, no case folding beyond ASCII.
`cc::utf8_to_utf16` in `string/conversion.hh` is the one transcoding operation clean-core has.

## Null-termination and C interop

**Neither type is null-terminated**, and `data()` must never be handed to a C API that expects `'\0'`.
`cc::string` does not maintain a terminator, so that every append does not have to.

Two operations bridge to C:

* `c_str_materialize()` writes a `'\0'` just past the content and returns the pointer.
  It is non-const, may allocate, and may move a small string to the heap — `size()` does not change.
* `c_str_if_terminated()` is a const inspection: it returns the pointer when a `'\0'` already happens to sit at `size()`, and `nullptr` otherwise.

The pattern is to try the const one first and materialize only on failure, which matters most for a string you only have a `const&` to, where materializing means copying.
`string::create_copy_c_str_materialized(sv)` builds such a copy in one step and guarantees `c_str_if_terminated()` succeeds on it.

Call these immediately before the C call and never store the result.

## When a pointer or view dies

**A pointer, reference or `string_view` into a `cc::string` is valid until the next non-const operation on that string.**

Non-const is the right boundary rather than "mutation": `c_str_materialize()` changes no content and can still reallocate.
Everything that appends, resizes, reserves, replaces, or shrinks can invalidate, as can assigning or destroying the string.
A `subview` is a `string_view` into the string and dies with the same rule.

`string_view` itself owns nothing, so the question always belongs to whatever holds the bytes — a `string`, a literal, or a buffer.
Views of string literals are the one case that never dies.

## Storage: inline or heap

A `cc::string` is in one of two modes, and `is_small()` says which.

Small mode keeps the content inline in the string object, so a short string never touches an allocator.
The inline capacity is `small_capacity`, **derived from the heap layout rather than fixed**.
The inline buffer fills the space before the allocation header's `custom_resource` pointer, minus one byte for the size tag.
That is 39 bytes on 64-bit targets and fewer where pointers are smaller, such as wasm32.
Code that needs the number should read `small_capacity`, not assume 39.

Growth past `small_capacity` materializes to the heap and moves the content there.
The reverse happens in exactly one place: **`shrink_to_fit()` returns a heap string to inline storage whenever its content fits**, freeing the allocation outright.
No other operation demotes, so a string that once spilled stays on the heap until you ask.

Front capacity — unused bytes *before* the content, from `reserve_front` — exists only in heap mode, since the inline layout has no room for an offset.
Nothing consumes it today; it survives back-growth until `shrink_to_fit()`.

### Memory resources

A `cc::string` remembers the `cc::memory_resource` it was built with, across every operation including the transitions above, and `nullptr` means the default global resource.
The pointer shares a word with the SSO tag bit, which is why `memory_resource` pointers must stay aligned.

Copy *construction* always adopts the source's resource.
Copy *assignment* keeps the destination's only when the source is on the heap: an inline source is block-copied, resource word and all.
Move leaves the source empty and small.

## Comparison, ordering and hashing

Equality is by content and works across the two types: `cc::string` has a member `operator==` accepting anything convertible to `string_view`.

Ordering is lexicographic by byte, so it sorts by raw UTF-8 rather than by any locale or collation, and a prefix sorts before the string that extends it.

The two types reach it differently.
`string_view` spells out `<`, `>`, `<=` and `>=` as hidden friends, which ADL finds whenever one operand is already a `string_view`.
`cc::string` instead has an `operator<=>` accepting anything convertible to `string_view`, and C++20 synthesizes the four relational operators from it.
That includes the reversed forms, so `"lemon" < s` works as well as `s < "nectarine"`.

`compare()` is still there on both types when you want all three outcomes from one pass rather than two comparisons.

Hashing is structural over the bytes, via `cc::make_hash_of_bytes` (XXH3-64).
`cc::string` delegates to `cc::string_view`, so **equal content hashes equally** regardless of inline-vs-heap storage or which of the two types holds it.
That is what makes a `string_view` usable for heterogeneous lookup in a `cc::map` keyed by `cc::string`, with no temporary allocation.
[benchmarks/string-hash-benchmark](benchmarks/string-hash-benchmark.md) is why XXH3 is the default.

## Character predicates

`string/char_predicates.hh` holds the classification and conversion functions — `is_space`, `is_digit`, `is_alphanumeric`, `to_lower`, and the rest.
They are `constexpr`, **locale-independent and ASCII-only**, which is exactly why clean-core does not use `<cctype>`.

The header also carries the equality and comparison functors `equal_case_sensitive`, `equal_case_insensitive`, `compare_ascii_case_sensitive` and `compare_ascii_case_insensitive`.
Those are more load-bearing than they look.
`equal_case_sensitive` is the default `EqualF` for `string_view`'s decompose / matching / strip family.
Passing `equal_case_insensitive` instead makes any of those matches case-insensitive.
`starts_with`, `ends_with` and `contains` take no `EqualF` and are always case-sensitive.

## Thread safety

Neither type synchronizes, and a view into a string another thread may mutate has the same lifetime problem as any other view.
