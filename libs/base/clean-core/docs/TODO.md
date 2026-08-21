# clean-core TODO

Running list of known follow-ups.
Add entries as we discover them, and remove them as they land.

## common

- **Enum value names, through `cc::custom::enum_traits`.**
  A first version prints `enum_name(integer_value)`, and `cc::flags::to_string` then prints a comma-separated list of them.
  That needs a variadic enumerator list in the macro plus a name table on the traits, next to `is_flag_enum` and `flag_storage_type` — which is the shape the traits already have.
  The catch is that only ONE specialization of `cc::custom::enum_traits<E>` may exist, so a names-only macro cannot sit alongside a `CC_FLAG_ENUM_*` one for the same enum.
  The two have to converge into one macro that declares everything, with `is_flag_enum` telling a nameable enum from a flag enum.
- **Migrate the remaining hand-rolled flag enums to `cc::flags`.**
  `sr::key_modifiers` is the last one still carrying its own operators plus a free `has_all`.
  All of sg's have migrated — `buffer_usages`, `texture_usages`, `access_flags`, `pipeline_stage_flags`, `color_write_mask`, `accel_build_flags`.

- **Revisit the regular-void stance across the vocabulary types.**
  Today `void` is rejected and you spell `cc::unit` by hand: `result<void, E>` is a static_assert pointing at it, `optional<void>` has the same shape, and `async<void>` was never wired up at all.
  Generic code that forms `xyz<T>` where `T` can reasonably be `void` hits this everywhere, so a per-type alias fixes one corner and leaves the rest.
  The direction is to support `xyz<void>` as an affordance meaning `xyz<unit>` — same type, no conversions — decided once for `result`, `optional` and `async` together rather than type by type.
  Two consequences the async side already ran into, and that the decision has to answer.
  A `shared_async<void>` dependency should contribute an ordering edge and NO argument to the `make_async_*` sugar.
  And it becomes indistinguishable from one deliberately carrying a `unit`.

## container

- **`bitset` printing and allocation interop.**
  Neither bit set has a `to_string`: it would drag `cc::string` into a container header, the way `variant`'s missing one does.
  The bit order is already decided — **index 0 leftmost**, since a bit set is an indexed bit array rather than a number.
  `cc::bitset` also cannot yet adopt or extract its `cc::allocation<u64>`.
- **`ringbuffer` adoption and extraction.**
  It holds its `cc::allocation<T>` as a raw byte handle, so neither `create_from_allocation` nor `extract_allocation` exists yet.
  Adoption can take the largest power of two at or below the incoming capacity; extraction needs the content linearized first, or a check that it already is.
- **Grow `tuple` and `variant`.**
  The first version deliberately left out converting construction from another `tuple<Us...>`, `tuple_cat`, `variant`'s `operator<=>` and multi-variant visitation.
  A `to_string` hidden friend for `variant` is missing too — it would drag `to_debug_string.hh` into a container header, so a `variant` currently debug-prints as a raw byte dump.

## bytes

- **base64.**
  `babel::base64` exists and is text-oriented: it takes a `cc::string_view`, tolerates whitespace and both alphabets, and returns a `cc::string`.
  A byte-level codec belongs here instead, and moving babel's would drag its tolerant-input contract down with it — the two want different answers on malformed input.
  Blocked on somebody below babel needing it; nobody does yet.
- **CRC32 and Adler32.**
  Wanted by every container format that stores a per-member checksum — zip, gzip, PNG — so this lands with the first of those rather than before it.
  Neither is a hash in the `common/hash.hh` sense and neither is cryptographic; they belong next to the codecs, not next to `blake3`.
- **Further compression algorithms, deflate first.**
  Deflate is what a zip or gzip container needs, and it slots in as one `compression_algorithm` value plus one backend file.
  Its three framings — raw deflate, zlib and gzip — are exactly what `compression_framing` already models, which is why that is an axis rather than a bool.
  Adding it also means a line in `cc::detect_algorithm`, which is the edit that compiles cleanly when forgotten.
- **Async compression.**
  `compress_async`, mirroring `algorithm/sort_async.hh`: explicit extra API over the synchronous core, never a hidden worker pool.
  zstd's own multithreading is deliberately not enabled, so this would chunk over `cc::async` instead and keep the memory cost where the caller can see it.
- **Error correction.**
  Reed-Solomon or similar, for bytes that have to survive a corrupted medium rather than merely get smaller.
  Speculative — nothing here needs it yet, and it is listed so the folder's shape is on record rather than because it is planned.

## cc::rec

- **A query index over a recording.**
  The queries are linear scans today, which is fine to somewhere around a million events and no test comes close.
  An index would be a hash from descriptor to event offsets plus a sorted timestamp array, built on first query and cached on the recording.
- **A richer value codec.**
  `CC_RECORD` takes scalars, enums, pointers and text; `desc::fields` already describes multi-field payloads, and nothing has needed one yet.
- **A static/interned tier for recorded text.**
  A literal already costs nothing when it is a site's NAME, and only a runtime string is copied — an interned handle could be stored as its id instead.
- **Sampling threads the recorder has never heard of.**
  The sampler walks the thread registry, which a thread joins by recording something — so a thread that records nothing is invisible, and that is exactly the thread worth sampling.
  Enumerating the process's OS threads instead would cover it, at the cost of sampling threads nothing in this process owns.
- **Symbolizing a foreign recording.**
  `cc::symbolizer` resolves against this process's loaded modules, so a `.ccrec` from another run or another machine mostly resolves to nothing.
  Doing better needs the recording to carry its module base table and build ids, and the analysis side to load the matching binaries.
- **A POSIX sampler.**
  Windows suspends a thread and walks it from outside; POSIX has no equivalent.
  There it wants `SIGPROF` via `timer_create`, plus a handler that walks its own stack and hands the frames and the anchor to the sampler thread.
  The event layout and the splice are already shared, so only the capture half differs.
- **Interning captured stacks.**
  A sampling profiler at a kilohertz across twenty threads is megabytes a second of return addresses, most of them repeats.
  A hash from stack to id, stored per event, is the fix; the early-out above shortens the stacks but does not deduplicate them.
