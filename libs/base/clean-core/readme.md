# clean-core

Foundational C++23 building blocks: data structures, memory utilities, assertions, and low-level primitives.
Namespace `cc`. **No dependencies** — this is the bottom of the shaped-core stack.

```cpp
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>

cc::vector<cc::string> names;
names.push_back("shaped");
```

Headers are included by their full path from `src/`, e.g.
`#include <clean-core/container/vector.hh>`. `fwd.hh` (at the root) forward-declares
the public types for headers that only need a declaration.

## File organization

Source lives in `src/clean-core/`, grouped by topic:

| Folder       | What's in it |
|--------------|--------------|
| `common/`    | compiler macros, type/meta `utility`, `enum_traits` + `flags`, the `hash` protocol, and the assertion suite (`assert`, `asserts`, `assertf`, `assert-handler`) |
| `bytes/`     | algorithms over byte ranges — the `hash128` / `hash256` digest values and the `blake3` hasher. `common/hash.hh` stays put: it is how a *type* participates in hashing, which containers depend on |
| `platform/`  | OS-facing utilities — `console` (color), `native` (demangling), `source_location`, `stacktrace`, and `win32_sanitized`, the sanctioned route to `<Windows.h>` |
| `math/`      | bedrock math helpers needed before the dedicated math library exists — `bit`, the `random` PRNG, and `wide_arith`'s 128-bit primitives |
| `memory/`    | allocation handles (`allocation`, `node_allocation`) over memory resources |
| `container/` | owning containers (`array`/`vector` + `fixed_`/`unique_`/`small_` variants, `map`, `set`, `pair`) and views (`span`, `strided_span`, `pinned_data`) |
| `sequence/`  | the lazy ranges API (`sequence`) — an early prototype, see [docs/sequence.md](docs/sequence.md) |
| `string/`    | `string`, `string_view`, `char_predicates`, the compile-time-checked `format` / `print`, `to_string` / `to_debug_string`, and UTF-16 `conversion` |
| `function/`  | type-erased callables (`function_ref`, `unique_function`) |
| `error/`     | fallible value types (`optional`, `result`) and the crash handler |
| `thread/`    | `async` and its work-stealing pool, `threaded_actor`, and the primitives under them (`atomic`, `mutex`, `spin`, `thread`) |
| `record/`    | `cc::rec` — the event stream logging, profiling, values, stats and tracing all write into ([systems/recording](docs/systems/recording.md)) |

`impl/` subfolders hold private implementation details (e.g.
`container/impl/allocating_container.hh`, `memory/impl/object_lifetime_util.hh`) —
don't include them directly.

## Building & testing

Build and test through the repo driver — never run the `clean-core-test` binary
directly:

```bash
uv run dev.py test            # build + run the full suite (clean-core + nexus)
uv run dev.py test "<pattern>"  # just the matching test(s) while iterating
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the
full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance (symbols, signatures, gotchas).
- [docs/containers.md](docs/containers.md) — choosing a container, and the contracts they all share.
- [docs/strings.md](docs/strings.md) — `cc::string` / `cc::string_view`: null-termination, invalidation, SSO, and hashing.
- [docs/formatting.md](docs/formatting.md) — `cc::format`, the placeholder grammar, and making a type formattable.
- [docs/sequence.md](docs/sequence.md) — `cc::sequence`, the lazy forward cursor: what the prototype does today, and the design the rest is intended to follow.
- [docs/_index.md](docs/_index.md) — clean-core's documentation hub.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all shaped-core code follows.
  `.clang-format` is authoritative for formatting.
