# clean-core docs

Documentation hub for clean-core. For the library overview, public types, and how
to include headers, start at the [readme](../readme.md). For repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

## Source organization

clean-core's headers live in `src/clean-core/`, grouped by topic:

```text
clean-core/
  fwd.hh        # forward declarations of the public types
  common/       # macros, utility/meta, flags, hash, assertions
  platform/     # native (demangling), source_location, stacktrace
  math/         # bit utilities
  memory/       # allocation, node_allocation (+ impl/)
  container/    # array/vector families, map, set, span, strided_span, … (+ impl/)
  sequence/     # the lazy ranges API
  string/       # string, string_view, char_predicates, to_string, to_debug_string
  function/     # function_ref, unique_function
  error/        # optional, result
  thread/       # mutex
```

`impl/` subfolders are private implementation details. The
[readme](../readme.md#file-organization) has the full per-folder table.

## Topics

No deep-dive docs yet — add them here as kebab-case `.md` files and link them from
this list.

## Conventions

- Namespace `cc`; **no dependencies** (bottom of the library stack).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md);
  `.clang-format` is authoritative for formatting.
