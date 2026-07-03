---
name: modernizing-old-code
description: Playbook for porting a working-but-old implementation (from the previous clean-core or another codebase) into current shaped-core — finding its home, swapping dead APIs for current ones, and matching today's C++23 style. Includes a living list of old→new gotchas. Use whenever you're handed a legacy snippet/file to "bring up to speed", "modernize", "port into clean-core", or "make it fit our style".
when_to_use: "modernize this", "bring this up to speed", "port this into clean-core", "our old impl was", "make this fit our new style", "update this old code", "this is from the old clean-core"
allowed-tools: Read Edit Write Bash Glob Grep mcp__repo_tools__repo_search mcp__repo_tools__repo_structure mcp__repo_tools__file_structure mcp__repo_tools__build_diag mcp__repo_tools__test_diag
---

## What this is

It will keep happening: we have a working-but-old version of something (usually
from the previous clean-core) and want it in the current shaped-core ecosystem.
The code *works*, but it predates C++23, the current allocation model, the
current naming/comment conventions, and the current directory layout. This skill
is the porting checklist plus a running list of concrete old→new differences.

**Do not paste old code in and tweak it.** Treat the old code as a *spec of
behavior*, then re-implement it against current APIs and style. The old version
will name functions, headers, and patterns that no longer exist.

## The loop

1. **Skim the cheat sheets first.** Read the `cheat-sheet.md` of the library
   you're touching and of its key dependencies — almost always
   [clean-core](../../../libs/base/clean-core/cheat-sheet.md) and, for tests,
   [nexus](../../../libs/base/nexus/cheat-sheet.md). They are the fastest map of
   what the current API actually looks like.
2. **Map every old symbol to its modern equivalent.** For each `cc::`/`std::`
   name the old code uses, confirm it still exists and is spelled the same.
   Use `repo_search` / `repo_structure` (not shell). Anything from the Gotchas
   list below has moved or been renamed.
3. **Find a sibling to mirror.** Locate the closest existing modern type and
   copy its structure (include order, group-comment visibility blocks, factory
   shape, doc-comment voice). E.g. for an owning pointer, mirror
   `memory/node_allocation.hh` + `function/unique_function.hh`.
4. **Pick the right home.** `libs/<category>/<lib>/src/<lib>/<area>/`. Match by
   responsibility, not by what the old file was called. If unsure between two
   areas (e.g. `container/` vs `memory/`), ask — it's a real decision.
5. **Implement** in current style (see Gotchas). Headers must compile standalone.
6. **Wire it up** — the three easy-to-forget spots:
   - forward-declare the type in `fwd.hh`;
   - add the header to `FILE_SET public_headers` **and** the test `.cc` to the
     `*-test` executable in the library's `CMakeLists.txt` (sources are listed
     explicitly, **not** globbed);
   - update the library `cheat-sheet.md` and any doc touching public API.
7. **Test** with nexus (`<type>-test.cc`), then **build + run via `dev.py`**
   (see the `building-and-testing` skill). Build a `release-*` preset too if you
   touch `CC_ASSERT`-gated paths (default `relwithdebinfo-*` has asserts on,
   `release-*` has them off).

## Gotchas: old clean-core → current (keep this list growing)

Allocation
- `cc::alloc<T>(args...)` / `cc::free(ptr)` are **gone**. For a single heap
  object use `cc::node_allocation<T>::create_from(cc::default_node_allocator(),
  args...)` — a move-only handle with `operator*`/`->`/`is_valid`, wait-free
  destruction, no stored allocator. For buffers/arrays use `cc::allocation<T>`
  or a container (`cc::vector`, `cc::unique_array`). `cc::unique_ptr` itself is
  now just a thin wrapper over `node_allocation` — don't hand-roll `new`/`delete`.

Customization points
- `struct cc::hash<T>` / `struct cc::less<T>` specializations are **gone**. The
  hash customization point is now a **hidden friend `u64 hash(T const&)`** (see
  [common/hash.hh](../../../libs/base/clean-core/src/clean-core/common/hash.hh)).
  Ordering, when actually needed, is a hidden-friend `operator<=>`. Don't port
  the old specialization structs.
- Equality: define one hidden-friend `operator==`; C++20 synthesizes `!=` and
  the reversed operand orders. The old hand-written 6–8 operator overloads are
  no longer needed.

Style / language
- **C++23.** Reach for deducing-this (`template <class Self> auto&& f(this Self&&
  self)`), concepts/`requires`, `if constexpr`, etc. instead of older idioms.
- **Qualified out-of-line definitions** (`template <class T> struct cc::foo { …
  };`), not `namespace cc { … }` blocks around the type. Namespace blocks are
  used only for free-function *declarations* (see `default_node_allocator`,
  `make_unique`). Nested implementation namespace is `impl`, never `detail`.
- **East const** (`T const`, `span<T const>`, `T const* p`). `snake_case` types/
  functions/variables; `_snake_case` private members; `UpperCase` template
  params; `UPPER_CASE` macros. 120 cols, Allman, 4-space — `.clang-format`
  (>= v21) is authoritative; run it and let it win over any prose.
- Prefer `cc::` utilities over `std::`: `cc::move`, `cc::forward`, `cc::exchange`
  (in `common/utility.hh`), `cc::span`, `cc::vector`, `cc::optional`,
  `cc::result`, `cc::string`, etc.
- `cc::span` has **no `as_bytes` / `as_mutable_bytes` / `reinterpret_as`** yet
  (still a TODO in `container/span.hh`). To go to/from a `cc::span<cc::byte const>`,
  build it explicitly: `cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(
  s.data()), s.size() * cc::isize(sizeof(T)))`. `cc::byte` is `std::byte`.
- `always_false<T>` from the old clean-core does **not** exist. For an
  unsupported-specialization guard, a plain primary-template
  `static_assert(!std::is_array_v<T>, "…")` usually covers the case without a
  dependent-false helper.

Comments / docs
- `///` plain prose for type/member docs, `//` inline. **No** Doxygen/Javadoc/XML
  tags (`@param`, `\return`, `<summary>`). A good `///` says what the thing is
  *for* and calls out edge cases (zero/empty, ownership, threading, which
  `result` it can fail with). No comments on trivial getters. No references to
  the task/PR — that goes in the commit message.

Header idioms to copy
- Group-comment visibility blocks with repeated access specifiers:
  `// properties` / `// smart pointer interface` / `// ctors/dtor` /
  `// factory` / `// members`, each followed by `public:`/`private:`.
- `[[nodiscard]]` on observers and factories; types are default-constructible;
  non-trivial construction goes through `create_*` static factories (or a free
  `make_*`); `explicit operator bool` for validity.

Layout / build / tests
- Library layout is `libs/<category>/<lib>` with colocated `.hh`/`.cc` under
  `src/<lib>/<area>/`. Add new public types to `fwd.hh`.
- `CMakeLists.txt` lists headers (FILE_SET) and test sources explicitly — adding
  a file without registering it means it silently isn't built.
- Tests use nexus (`TEST` / `SECTION` / `CHECK` / `REQUIRE`), file named
  `<type>-test.cc`. **Never run a `*-test` binary directly** — go through
  `uv run dev.py test "<pattern>"`, then diagnose with the `repo_tools`
  `build_diag` / `test_diag`. (A `dev.py` run may report a *sibling* test binary
  as "failed" only because your filter matched no tests in it — "The current
  schedule did not select any tests" is harmless, not a real failure.)

## Worked example: `cc::unique_ptr`

The old `unique_ptr` used `cc::alloc`/`cc::free`, `struct hash<>`/`struct less<>`
specializations, ~8 comparison operators, and a `unique_ptr<T[]>` +
`always_false` guard. The modern port
([memory/unique_ptr.hh](../../../libs/base/clean-core/src/clean-core/memory/unique_ptr.hh)):
wraps a `cc::node_allocation<T>` member; `make_unique` calls
`node_allocation<T>::create_from(cc::default_node_allocator(), …)`; one
hidden-friend `operator==` set + `u64 hash(unique_ptr const&)`; a single
`static_assert(!std::is_array_v<T>)` (no `always_false`, no `T[]`
specialization); clears by assigning `nullptr` (no `reset()`). It lives in
`memory/` next to the `node_allocation` it wraps — not in `container/`, since it
owns exactly one object.

## Keep this current

Every time you port something and hit a difference that isn't listed above, add
it to the Gotchas section. This list is the whole point of the skill — it should
get more complete with each migration.
