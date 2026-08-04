---
name: modernizing-old-code
description: Playbook for porting a working-but-old implementation (from the previous clean-core or another codebase) into current shaped-core — finding its home, swapping dead APIs for current ones, and matching today's C++23 style. Includes a living list of old→new gotchas. Use whenever you're handed a legacy snippet/file to "bring up to speed", "modernize", "port into clean-core", or "make it fit our style".
when_to_use: "modernize this", "bring this up to speed", "port this into clean-core", "our old impl was", "make this fit our new style", "update this old code", "this is from the old clean-core"
allowed-tools: Read Edit Write Bash Glob Grep mcp__repo_tools__repo_search mcp__repo_tools__repo_structure mcp__repo_tools__file_structure mcp__repo_tools__build_diag mcp__repo_tools__test_diag
---

## What this is

It will keep happening: we have a working-but-old version of something, usually from the previous clean-core, and want it in the current shaped-core ecosystem.
The code *works*, but it predates C++23, the current allocation model, the current naming and comment conventions, and the current directory layout.
This skill is the porting checklist plus a running list of concrete old→new differences.

**Do not paste old code in and tweak it.**
Treat the old code as a *spec of behavior*, then re-implement it against current APIs and style.
The old version will name functions, headers and patterns that no longer exist.

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
4. **Pick the right home.** `libs/<category>/<lib>/src/<lib>/<area>/`.
   Match by responsibility, not by what the old file was called.
   If unsure between two areas — `container/` versus `memory/`, say — ask; it is a real decision.
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
  object use `cc::node_allocation<T>::create_from(cc::default_node_allocator(),
  args...)` — a move-only handle with `operator*`/`->`/`is_valid`, wait-free
  destruction, no stored allocator.
  For buffers and arrays use `cc::allocation<T>` or a container (`cc::vector`, `cc::unique_array`).
  `cc::unique_ptr` itself is now just a thin wrapper over `node_allocation`, so don't hand-roll `new`/`delete`.

Customization points
- `struct cc::hash<T>` / `struct cc::less<T>` specializations are **gone**. The
  hash customization point is now a **hidden friend `u64 hash(T const&)`** (see
  [common/hash.hh](../../../libs/base/clean-core/src/clean-core/common/hash.hh)).
  Ordering, when actually needed, is a hidden-friend `operator<=>`. Don't port
  the old specialization structs.
- Equality: define one hidden-friend `operator==`; C++20 synthesizes `!=` and the reversed operand orders.
  The old hand-written 6–8 operator overloads are no longer needed.

The current house style is [docs/coding-guidelines.md](../../../docs/coding-guidelines.md), and `.clang-format` is authoritative for formatting — run it and let it win over any prose.
What a *port* specifically trips on, beyond that:

- **C++23.** Reach for deducing-this (`template <class Self> auto&& f(this Self&& self)`), concepts and `requires`, `if constexpr`, instead of older idioms.
- **Qualified out-of-line definitions** (`template <class T> struct cc::foo { … };`), not `namespace cc { … }` blocks around the type.
  Namespace blocks are used only for free-function *declarations*; see `default_node_allocator` and `make_unique`.
- Prefer `cc::` utilities over `std::`: `cc::move`, `cc::forward`, `cc::exchange` (in `common/utility.hh`), `cc::span`, `cc::vector`, `cc::optional`, `cc::result`, `cc::string`.
- Byte views exist — don't hand-roll `reinterpret_cast`. `cc::span` has
  `s.as_bytes()` / `s.as_mutable_bytes()` (and `reinterpret_as<U>()` /
  `try_reinterpret_as<U>()`); `cc::string` / `cc::string_view` have `as_span()` /
  `as_bytes()` (plus `as_mutable_span()` / `as_mutable_bytes()` on `string`); and free
  `cc::as_bytes(c)` / `cc::as_mutable_bytes(c)` byte-view any container exposing
  `.data()`/`.size()` (all `cc`/`std` containers, string, string_view). To byte-view a
  bare C array or pointer+len, wrap in `cc::span` first, then `.as_bytes()`. `cc::byte`
- `always_false<T>` from the old clean-core does **not** exist.
  For an unsupported-specialization guard, a plain primary-template `static_assert(!std::is_array_v<T>, "…")` usually covers the case without a dependent-false helper.

Comments / docs
- `///` plain prose for type and member docs, `//` inline, and **no** Doxygen, Javadoc or XML tags.
  A good `///` says what the thing is *for* and calls out the edge cases: zero and empty, ownership, threading, which `result` it can fail with.
  The full rules, including the one-semantic-point-per-line prose style, are [coding-guidelines.md](../../../docs/coding-guidelines.md)'s.

Header idioms to copy
- Group-comment visibility blocks with repeated access specifiers: `// properties`, `// smart pointer interface`, `// ctors/dtor`, `// factory`, `// members`, each followed by `public:` or `private:`.
- `[[nodiscard]]` on observers and factories; types are default-constructible; `explicit operator bool` for validity.
  Non-trivial construction goes through a `create_*` static factory, or a free `make_*`.

Layout / build / tests
- Library layout is `libs/<category>/<lib>` with colocated `.hh`/`.cc` under
  `src/<lib>/<area>/`. Add new public types to `fwd.hh`.
- `CMakeLists.txt` lists headers (FILE_SET) and test sources explicitly — adding
  a file without registering it means it silently isn't built.
- Tests use nexus (`TEST` / `SECTION` / `CHECK` / `REQUIRE`), in a file named `<type>-test.cc`.
  **Never run a `*-test` binary directly** — go through `uv run dev.py test "<pattern>"`, then diagnose with `build_diag` / `test_diag`.
  A filter that matches nothing in *any* binary now fails loudly with a "did you mean …" diagnostic, and binaries the filter does not select are skipped rather than reported.

## Worked example: `cc::unique_ptr`

The old `unique_ptr` used `cc::alloc` / `cc::free`, `struct hash<>` and `struct less<>` specializations, ~8 comparison operators, and a `unique_ptr<T[]>` plus `always_false` guard.
The modern port is [memory/unique_ptr.hh](../../../libs/base/clean-core/src/clean-core/memory/unique_ptr.hh).
It wraps a `cc::node_allocation<T>` member, and `make_unique` calls `node_allocation<T>::create_from(cc::default_node_allocator(), …)`.
There is one hidden-friend `operator==` set, plus `u64 hash(unique_ptr const&)`.
A single `static_assert(!std::is_array_v<T>)` replaces the `always_false` guard and the `T[]` specialization, and clearing is assigning `nullptr` rather than a `reset()`.
It lives in `memory/` next to the `node_allocation` it wraps, not in `container/`, since it owns exactly one object.

## Keep this current

Every time you port something and hit a difference that isn't listed above, add it to the Gotchas section.
This list is the whole point of the skill: it should get more complete with each migration.
