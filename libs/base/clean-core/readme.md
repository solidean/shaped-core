# clean-core

Foundational C++23 building blocks: data structures, memory utilities, assertions,
and low-level primitives. Namespace `cc`. **No dependencies** — this is the bottom
of the shaped-core stack.

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
| `common/`    | compiler macros, type/meta `utility`, `flags`, `hash`, and the assertion suite (`assert`, `asserts`, `assertf`, `assert-handler`) |
| `platform/`  | compiler/OS introspection — `native` (symbol demangling), `source_location`, `stacktrace` |
| `math/`      | bedrock math helpers (`bit`) needed before the dedicated math library exists |
| `memory/`    | allocation handles (`allocation`, `node_allocation`) over memory resources |
| `container/` | owning containers (`array`/`vector` + `fixed_`/`unique_` variants, `map`, `set`, `ringbuffer`, `bitset`, `disjoint_set`, `pair`, `tuple`, `variant`) and views (`span`, `strided_span`) |
| `sequence/`  | the lazy ranges API (`sequence`) |
| `string/`    | `string`, `string_view`, `char_predicates`, `to_string`, `to_debug_string` |
| `function/`  | type-erased callables (`function_ref`, `unique_function`) |
| `error/`     | fallible value types (`optional`, `result`) |
| `thread/`    | concurrency primitives (`mutex`) |

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

- [docs/_index.md](docs/_index.md) — clean-core's documentation hub.
- [coding-guidelines](../../../docs/coding-guidelines.md) — conventions all
  shaped-core code follows (`.clang-format` is authoritative for formatting).
