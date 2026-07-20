# Coding Guidelines

The coding standards and design principles for the shaped-core libraries.
Priorities: correctness, performance, maintainability, readability.
Most examples use `clean-core` (`cc::`) — the shared foundation every other library builds on.

> [.clang-format](../.clang-format) is authoritative for formatting.
> Where this document and the format config disagree, **the config wins** — fix the doc, not the code.
> [.clang-tidy](../.clang-tidy) is still being calibrated; treat its warnings as advisory, not gospel.

---

## The style is still evolving — how to deal with older code

These guidelines are **living**: they get sharper as we go.
The codebase is too large to rewrite on every refinement, so the rule is a ratchet, not a big bang.

- **New code follows the newest guidelines**, always.
  That is this document plus the library-local `docs/coding-guidelines.md` where one exists.
- **You will see older style** — reflowed comments, `T x(args);`, structs filled field-by-field.
  That is history, not a counter-example.
  Do **not** proactively convert it.
- **Migrate drive-by** what you are already editing — the function *and* its doc comment.
  Editing a file doesn't mean sweeping the file; never sweep neighbors.
- **Keep the guidelines current.**
  A new convention is recorded in the same change that introduces it.
  Here, in the library-local guidelines, in the affected cheat sheets, in [CLAUDE.md](../CLAUDE.md).
  A convention that lives only in a review comment does not exist.

Two things override drive-by migration.
Wording the author deliberately set — keep it.
Churn that would bury the actual point of a diff — do it separately.

---

## Prose style — one semantic point per line

**Never reflow prose into a justified block. A new point starts a new line.**

This binds **everything we write in prose**:

- `///` doc comments and `//` inline comments
- **every Markdown file in the repo** — [docs/](_index.md), readmes, cheat sheets, [CLAUDE.md](../CLAUDE.md), skill files
- commit messages and PR descriptions

Reflowed prose cannot be skimmed.
Every point starts at an unpredictable column, so the eye must read all of it to find the line that matters.
Reading the *first few words of each line* has to give the shape of the whole passage.

```cpp
// good — each line is one point, skimmable down the left edge
// Robust full GWN — exact integer predicates keep the integer and fractional terms in agreement.
// No spurious ±1 near the surface.
// Prepared object, built once from the mesh.
// Bakes a fixed axis-aligned ray; takes no x0.
// Requires Embree. See gwn_mesh_robust.hh.

// bad — one reflowed block; every point starts mid-line
/// Whether the GPU device has been lost (driver reset / TDR / removed adapter). Sticky once set:
/// the context is unusable and must be torn down and recreated. Submit / advance / fence waits and
/// the throwing create wrappers raise sg::device_lost_exception once this is true; a caller polling
/// the `try_*` surface breaks its retry loop by checking this (device loss never comes true).
```

Concretely:

- **A line ends because the point ends — never because a column was reached.**
  There is no fill column. Don't wrap at 80, at 100, or at 120 out of habit.
  The 120-column limit binds *code*, not prose; clang-format does not reflow comments.
- **Line length is free.** Typically 20–150 characters, whatever the point needs; **200 is the hard ceiling**.
  A point that long usually holds two — split it at the seam rather than wrapping it.
- **A short orphan line is the tell.** A line carrying only a few trailing words of the line above means you wrapped early — join them.
  Every line is either a whole point, or long enough that it plainly had to break.
- **New point ⇒ new line.**
  A sentence ending mid-line is the signal you got it wrong.
- **Front-load.** Put the surprising part first, on its own line.
  Preconditions, ownership, threading and edge cases outrank restating the signature.
- **Cut ruthlessly.** Short lines are not the goal, concision is.
  A long comment split across many lines is still a long comment.
  If it explains several unrelated concerns, split it or move the rationale to a higher-level doc.

---

## Language & Compiler Requirements

**C++ Standard:** C++23 minimum

**Supported Platforms:**
- **64-bit only** — no 32-bit targets are planned (WASM's `wasm32` is a 64-bit register
  target with a 32-bit address space, and counts as 64-bit here).
- Architectures: x64, ARM64, wasm32.
- Tiered: Windows/Linux/macOS/Emscripten single-threaded (Tier 1, CI-tested), iOS/Android
  (Tier 2), the other WASM combinations, WASI, and consoles (Tier 3, planned). See
  [platforms.md](platforms.md) for the full matrix.

**Compiler Support:**
- **First-class:** Clang and MSVC *(TODO: minimum versions)*
- **Second-class:** GCC (temporary issues may occur, fixes welcome)

---

## Repository Structure

The repo is a growing collection of libraries under `libs/<category>/<lib>` (today
`libs/base/clean-core` and `libs/base/nexus`; more will follow). Each library is laid out
consistently:

- **`src/<lib>/`** — Library implementation (`.hh` and `.cc` files colocated)
- **`tests/`** — Test code using nexus (separate build target named `<lib>-test`)
- **`docs/`** — Optional library-local documentation (prefer Markdown)

Cross-library and repo-wide docs live in [docs/](_index.md).

---

## Naming Conventions

| Element                                       | Convention    | Example                            |
|-----------------------------------------------|---------------|------------------------------------|
| Types (struct, class, enum, concept, typedef) | `snake_case`  | `string_view`, `dynamic_array`     |
| Functions                                     | `snake_case`  | `to_string()`, `get_size()`        |
| Variables                                     | `snake_case`  | `buffer_size`, `input_data`        |
| Enum values                                   | `snake_case`  | `error_none`, `format_utf8`        |
| Namespaces                                    | `snake_case`  | `cc`, `cc::impl`                   |
| Template parameters                           | `UpperCase`   | `template <class T, int Size>`     |
| Private members                               | `_snake_case` | `_internal_state`, `_cached_value` |
| Macros                                        | `UPPER_CASE`  | `CC_ASSERT`, `CC_FORCE_INLINE`     |

**Note:** Template parameters are often re-exposed as `snake_case` type aliases inside the struct/class.

---

## Code Style & Formatting

### General Principles

- **clang-format is mandatory** (requires clang-format >= 22). Source files must not change under
  clang-format execution. Use trailing `//` comments to steer formatting locally when necessary.
  Header include order is also handled by clang-format.
- **Brace style: Allman** (opening brace on its own line) — clang-format-enforced. Short *inline*
  function bodies may stay on one line.
- **Column limit: 120.** Keep lines within it; clang-format wraps the rest.
- One declaration per line. Never `int a, b;`
- Prefer short, tight sections optimized for skimming.
- **Comparison direction consistency:** When chaining comparisons with `&&`, maintain consistent direction.
  ```cpp
  // Good: consistent direction
  if (a < b && b < c) { ... }
  if (min <= value && value < max) { ... }

  // Avoid: inconsistent direction
  if (b > a && b < c) { ... }
  if (value >= min && max > value) { ... }
  ```

### Type Declarations & Const

**East const everywhere:**
```cpp
T const x = ...;
span<T const> data;
cc::string const& name;
```

**Pointers and references bind left** (`PointerAlignment: Left`); combined with east const this reads
as `T const* p` and `T const& r`.

**Prefer almost-always-auto style:**
```cpp
auto const x = T{ ... };  // preferred
auto const y = T(a, b);   // preferred — call the constructor on the right-hand side
T const x = { ... };      // acceptable but less consistent
T x;                      // fine if initialized later
```

**Never constructor-init a variable** (`T x(args...);`).
It reads as a plain declaration until you reach the parenthesis, and it is the most-vexing-parse trap.
Move the construction to the right-hand side, where `auto` can carry the left:

```cpp
tg::pos3f const p(x, y, z);           // avoid
auto const p = tg::pos3f(x, y, z);    // prefer
```

### Headers & Forward Declarations

- Forward-declare all important types in a `fwd.hh` file.
- **Headers must compile standalone** without requiring additional includes. Rare exception: mutually recursive templated code (requires proper documentation).
- **Include style:** Fine to rely on transitive includes or do explicit includes—no strong preference.
- Keep headers lightweight when possible. Use the [vimpl pattern](https://solidean.com/blog/2025/the-vimpl-pattern-for-cpp/) for non-performance-critical types.
- Avoid opening namespaces unnecessarily. Prefer qualified names:
  ```cpp
  struct cc::string { ... };
  void cc::to_string(...) { ... }
  ```
  This works when the declaration already exists.
- Use `impl` nested namespaces for implementation details that must be visible (e.g., due to inlining requirements):
  ```cpp
  namespace cc::impl
  {
      // implementation details here
  }
  ```
  **Note:** The previous convention of using `detail` does not apply anymore.
  A single `impl` namespace is **shared across the whole library**, so be mindful of name collisions: give
  implementation symbols a component-specific prefix when their bare names would be generic enough to clash
  with other components (e.g. `format_parse_spec` / `format_field`, not `parse_spec` / `field`).
- Use a `custom` nested namespace for **customization points that users specialize** (class-template
  specialization): declare the primary template in `<lib>::custom`, have users add their specializations
  there, and never require them to name `impl` types (pass public types across the boundary; offer public
  delegation helpers where useful). The resolution order and naming details live in
  [clean-core/docs/customization-points.md](../libs/base/clean-core/docs/customization-points.md). For
  simple, non-specialization extension points, prefer a hidden-friend or member function (e.g. `to_string()`).

### Performance-Critical Code

All functions expected to be inlined **must** be implemented in headers.
Non-performance-critical functions can live in `.cc` files to reduce compile times.

**Goal:** Maximum performance without requiring LTO, while remaining compile-time conscious.

**Performance priorities:** High runtime performance > low compile times > low binary size. Tradeoffs must remain reasonable. Prefer O(1) template instantiations over O(n) where feasible.

**Definition of "hot-path":** Code reasonably used in the innermost/most-executed parts of user code where it can actually be the bottleneck. A little math or conversion before a filesystem syscall will never matter performance-wise.

---

## Language Features

### Templates & Constraints

- Use concepts, `requires` constraints, and `static_assert` judiciously.
- **Prefer `static_assert` in templated code** to produce high-quality error messages, even if less SFINAE-friendly. SFINAE is for overload control; minimize overloading in general. Exception: view types that are inclusive in what implicitly constructs to them.
- Prefer `requires` + `static_assert` over SFINAE and pre-C++20 template metaprogramming.
- **Minimize template bloat:** Use type erasure at key points. Balance fast, inlineable hot-path code with reasonable compile times and binary size. Techniques include thin templates, vimpl, and type erasure.
- Explicitly prefix `cc::` even inside the library when taking templated arguments to prevent unintended ADL capture.
- Non-trivial ADL usage must always be explicitly marked.
- Use C++23 deducing `this` when it provides clearer or more efficient code:
  ```cpp
  struct example
  {
      template <class Self>
      auto&& get_value(this Self&& self) { return std::forward<Self>(self)._value; }
  };
  ```

### Templated Callables & Invocables

When accepting templated callables (predicates, functors, callbacks), choose the invocation method based on flexibility needed:

- **Is there a reasonable index to optionally pass?** → Use `cc::invoke_with_optional_idx` (supports `pred(elem)` and `pred(idx, elem)`)
- **Could pointer-to-member or pointer-to-member-function make sense?** → Use `cc::invoke`
- **Otherwise** → Use direct `operator()` for best compile times

**Example with optional index:**
```cpp
template <class Pred>
void remove_all_where(Pred&& pred)
{
    // ...
    if (cc::invoke_with_optional_idx(idx, pred, elem))  // supports pred(elem) and pred(idx, elem)
        // ...
}

// Usage:
vec.remove_all_where([](auto const& e) { return e.is_empty(); });
vec.remove_all_where([](int i, auto const& e) { return i > 5 && e.value < 0; });
vec.remove_all_where(&T::is_empty);     // pointer-to-member-function works too
```

**Example with fixed signature:**
```cpp
template <class EqPred>
bool strip_matching_prefix_with(string_view s, EqPred&& eq)
{
    // ...
    if (eq(c0, c1))  // direct call: only eq(char, char) makes sense
        // ...
}
```

### constexpr & noexcept

- **constexpr:** Use only when anticipating actual compile-time usage. Avoid sprinkling `constexpr` everywhere, especially on functions that cannot realistically execute at compile time.
- **noexcept:** Do not spam. Performance benefit is niche; only add where measurably beneficial.

### Attributes & Annotations

- `[[nodiscard]]` for non-void functions, **except** obvious getters (`get_xyz`, `is_xyz`, `has_xyz`).
- All single-argument constructors must be `explicit` unless there's a **documented** reason for implicit conversion.

### Constructors vs Factory Methods

**Keep types default constructible.** Non-default-constructible types are painful to use in containers, optional, and generic code. Provide an "invalid" or "empty" state if needed.

Avoid non-trivial constructors. Prefer static factory methods instead:

```cpp
// Good: default constructible with factory methods
struct texture
{
    [[nodiscard]] static cc::result<texture> create_from_file(cc::string_view path);
    [[nodiscard]] static texture create_with_dimensions(int width, int height);

    texture() = default;  // always provide default ctor

    bool is_valid() const { return _width > 0; }

private:
    int _width = 0;  // invalid state when 0
};

// Avoid: complex ctor that can fail
struct texture
{
    texture(cc::string_view path);  // what if file doesn't exist?
};
```

**Factory method naming convention:**
- **Must use `create_` prefix** (e.g., `create_from_file`, `create_with_dimensions`)
- **Must be marked `[[nodiscard]]`**
- **Rationale:** "from" reads like conversion only, "make" implies less work. "create" is consistent and clear. Since C++ allows calling static functions through instances, the prefix prevents confusion with mutating member functions. The consistent prefix also aids discoverability via autocomplete.

**Benefits of factory methods:**
- Can return `cc::result` or `optional` to properly handle failures
- Can return different types (base class, interface, optimized variants)
- Have descriptive names that clarify intent
- More flexible for future changes

**Benefits of default constructibility:**
- Works seamlessly with containers, `optional`, and generic algorithms
- Simplifies two-phase initialization patterns
- Reduces friction in generic code

### Initialization

**Reach for designated initializers first.**
They name each field where it is set, survive field reordering, and default what you don't mention.

```cpp
auto const config = config_t{.buffer_size = 1024, .enable_cache = true, .timeout_ms = 5000};
```

**Do not fill a struct field-by-field after declaring it.**
The `x.a = …; x.b = …;` shape repeats the variable name on every line.
It leaves a forgotten field silently at its default, and the object exists half-built in between.

```cpp
// avoid
shader_description sd;
sd.source = source;
sd.entry_point = entry;
sd.stage = stage;
compile(sd);

// prefer
compile({.source = source, .entry_point = entry, .stage = stage});
```

**Use-once descriptions belong in the call.**
A description built and immediately handed to one function is written *as* that argument.
Braced list for a `span` parameter, designated initializers per element.
A named local earns its name only when it is reused, or when naming it clarifies a long call.

```cpp
// prefer — the views exist only for this call
auto group = ctx.transient.create_binding_group(layout, {{.name = "scene", .view = tlas->as_view()},
                                                         {.name = "Output", .view = img.as_readwrite_view()}});
```

Fall back to plain braced initialization where designated initializers don't apply — a container, or a non-aggregate:
```cpp
auto const values = cc::vector<int>{1, 2, 3};
```

When the remaining fields must come from API calls (handles returned by `add_*` helpers, say), designated-init what you can and let the calls follow:
```cpp
auto rpd = sg::raytracing_pipeline_description{.layout = layout, .max_payload_size = sizeof(float) * 4};
auto const raygen_h = rpd.add_raygen_shader(cc::move(raygen));
```

---

## API Design & Ownership Semantics

### Pointers & References

- **Raw pointers are non-owning optional references.** They never own memory (except internally in container implementations).
- Prefer view types (`span`, `string_view`, `function_ref`) when ownership is not required.
- When ownership is required, prefer value types or unique/move-only types over shared ownership.
  - Shared ownership (e.g., `shared_ptr`) adds complexity and overhead; use only when truly necessary.

### Passing Arguments

| Size / Ownership               | Convention                             |
|--------------------------------|----------------------------------------|
| Small value types (≤ 32 bytes) | Pass by value                          |
| No ownership transfer          | Pass by `const&` or view type          |
| Ownership transfer             | Pass by value, then `cc::move` internally |

Actively prevent unnecessary copies (e.g., `cc::vector` copies). Use `const&` or view types.

### Move & Copy Semantics

When defining copy/move constructors or assignment operators, **always specify all four** and explicitly default or delete them:
```cpp
T(T const&) = default;
T(T&&) = default;
T& operator=(T const&) = default;
T& operator=(T&&) = default;
```

---

## Error Handling

Tiered error handling philosophy — the full authority (decision guide, the `try_*` + throwing-façade
pattern, why device resets / alloc failures are *not* assertions) lives in
[error-handling.md](error-handling.md):

| Mechanism                 | Use Case                                                             |
|---------------------------|----------------------------------------------------------------------|
| `CC_ASSERT`               | Contract violations — preconditions, postconditions, invariants (programmer bugs) |
| `cc::result` / `optional` | Frequent or expected failures the caller can handle locally          |
| Exceptions                | Exceptional errors requiring non-local control flow                  |

**Assertions:** Use `CC_ASSERT(cond, msg)` liberally in headers and `.cc` files to check preconditions, postconditions, and invariants. Assertions must be side-effect free (the expression must not be load-bearing for correctness; temporary debug output is fine) and **must never fire on user input or environment** — those are `result`/exceptions, since a release build (assertions compiled out) must still handle them.

**Undefined behavior:** Avoid relying on UB. Each explicit UB usage must be heavily documented and justified.

---

## Integer & Numeric Types

- **`int` is the vocabulary type for small integers.** Reach for it by default for any
  integer that is reasonably small (comfortably within the 31/32-bit range — counts,
  loop counters, worker/thread counts, sizes of small collections) *and* is not
  data-layout relevant. Don't use a sized type like `i32` there just because it looks
  more precise — `int` is the intent-carrying choice and the sized alias adds noise.
- Use explicitly sized types (`i32`, `u64`, `f32`, etc.) only when bit width or precision
  actually matters: serialized/ABI layout, hashing, bit manipulation, values that can
  straddle the 32-bit limit, or GPU/interop structs.
- **Never write `cc::isize` / `cc::u64` / … inside a library.** The sized aliases are vocabulary — as
  close to language-provided as we get — and a `cc::` prefix on them is noise, not information.
  Pull them into your own namespace once, in your `fwd.hh`, and then write them bare everywhere:
  ```cpp
  namespace my_lib { using namespace cc::primitive_defines; }   // in my_lib/fwd.hh
  ```
  clean-core, nexus, typed-geometry, sg, sr, sv and slib all already do this, so inside any of them
  `isize` / `i64` / `f32` just work. The `cc::` prefix stays right for actual clean-core *types*
  (`cc::span`, `cc::vector`, …) — the ADL-capture rule is about those, not about the primitives.
- **Write `sizeof(T)` bare** at call sites — no `cc::isize(sizeof(T))` armor.
  The implicit conversion to `isize` is fine; the cast is pure noise.
  If a linter complains, turn that check off in [.clang-tidy](../.clang-tidy) with a rationale comment — don't decorate every call site.
  The cast belongs only inside arithmetic a library performs for its callers — `offset * isize(sizeof(T))` in a container header.
  There the signedness of the whole expression is the point.
- Avoid "magic sentinels" like `-1` for invalid states. Prefer `optional` or `variant` unless there's a justified performance or memory reason.
- Give each distinct index / handle / id role its own strong enum:
  `enum class name : int { invalid = -1 };`. The compiler then rejects mixing roles
  (passing an `op_index` where a `type_index` is wanted), and the named `invalid`
  member replaces a bare `-1` sentinel. This is the positive form of the rule above:
  when a raw `int` index would otherwise carry an `-1` "none", reach for a strong
  enum instead. Crossing into the underlying integer for subscripting or arithmetic
  is an explicit `int(x)` at the use site — keep loop counters a plain `int` and
  construct the strong index where it is stored (e.g. `nx::fuzz::type_index`).

---

## Standard Library & Dependencies

**Avoid `std::` code.** Almost always use `cc::` equivalents instead.

**Exception:** a small set of *blessed* stdlib headers is allowed where re-creating them is infeasible or pointless—`<type_traits>`, `<typeinfo>` / `<typeindex>`, `<atomic>`, `<initializer_list>`. These are thin wrappers around compiler/runtime machinery that we don't re-wrap. clean-core keeps the authoritative list (with justifications) in [libs/base/clean-core/docs/blessed-stdlib-headers.md](../libs/base/clean-core/docs/blessed-stdlib-headers.md); the list grows by targeted addition only.

**Rationale:** Keep compiler intrinsics and `__builtin`s encapsulated in `cc::` implementations so downstream code avoids compiler and platform specifics. Provides a consistent, cohesive foundational library from a single source.

---

## Build Configurations

**Debug, RelWithDebInfo, Release must not majorly differ in behavior or implementation.**

Ideally the only difference is that Release doesn't have `CC_ASSERT` anymore. Debug validations should be handled via additional CMake options and remain available in RelWithDebInfo/Release as well. This applies equally to features like logging levels.

**Keep compiler optimization level orthogonal to other features.**

---

## Concurrency

All functions and types are **single-threaded** ("externally synchronized") by default unless noted.
Thread-safe types typically include `atomic_` in their name.

**A build may have no threads at all** — WebAssembly, or any build configured `-DSC_THREADS=OFF` (see
[platforms.md](platforms.md#threading-sc_threads)). `CC_HAS_THREADS` reports which you are in. Two rules
follow:

- **Use `cc::atomic`, never `std::atomic` directly** ([clean-core/thread/atomic.hh](../libs/base/clean-core/src/clean-core/thread/atomic.hh)).
  With threads it *is* `std::atomic`; without them it is a plain value with the same API, so a refcount
  bump is an `add` instead of a `lock xadd`. A hand-written `std::atomic` stays atomic in a build that
  provably has no concurrency, and no flag can reach it. `<atomic>` is blessed to appear in our headers
  but not to be called into — see [blessed-stdlib-headers.md](../libs/base/clean-core/docs/blessed-stdlib-headers.md).
- **Never gate API on the flag.** Threaded types keep their full surface and fall back to running on the
  calling thread; document the fallback instead of `#if`-ing the declaration away. `cc::threaded_actor`
  is the model: same API, and unthreaded the caller drives it. The corollary is that whoever would have
  blocked must now pump — a wait only the absent thread could satisfy is a deadlock, not a slow path
  (see [shaped-graphics threading](../libs/graphics/shaped-graphics/docs/concepts/threading.md)).

---

## Operators & Overloading

Use **hidden friends** for operator overloading where possible:
```cpp
struct vec3
{
    friend vec3 operator+(vec3 a, vec3 b) { return { a.x + b.x, ... }; }
};
```
**Benefits:** Improved compile times, reduced symbol pollution, better ADL control.

---

## Visibility & API Grouping

Use **redundant visibility modifiers** to logically group APIs.
Place group comments above the modifier, preserving space for individual documentation comments:

```cpp
class foo
{
    // construction
public:
    foo() = default;

    // queries
public:
    /// returns the current size
    int size() const;

    // modifiers
public:
    /// resizes the container to n elements
    void resize(int n);
};
```

---

## Comments & Documentation

[Prose style](#prose-style--one-semantic-point-per-line) applies here first:
one semantic point per line, no reflowed blocks, free line length.

### Code Comments

- Explain invariants, assumptions, and non-obvious design decisions.
  Favor **why** over **how** — the code already shows how.
- Do not restate code. Answer "what would surprise a competent reader here?"
- Inline comments justify unusual operations, hidden dependencies, representation choices or correctness constraints.
  Delete comments that merely describe the action being performed, unless they serve the grouping rule below.
- Use comments for grouping and structure.
  Skimming a long function's comments should reveal its logical flow.

### Documentation Comments

Use `///` for documentation.
**No doc tags, no XML** (`@param`, `\return`, `<summary>`, …) — API docs aren't generated here, and the tags only cost signal in the editor.

Doc comments are read in the source, unrendered. Write for that.

- Describe resulting state, ownership, lifetime, and invariants — not implementation steps.
- State constraints as *what must hold*: "capacity must be > 0", not "asserts on capacity <= 0".
- Call out edge cases: zero/empty, threading, which `result` it fails with, laziness/caching.
- Don't restate the signature. Spend the comment on what the types don't show.
- No comments on trivial getters and one-liners.
- **One usage example** (~2-10 lines) in the doc comment of each major struct/data type.

```cpp
/// Allocates a new buffer of `capacity` elements, zero-initialized.
/// Capacity must be > 0.
/// Fails with alloc_error when the allocator is exhausted.
[[nodiscard]] cc::result<buffer> allocate_buffer(int capacity);
```

---

## Macros

Macros must be **justified.** They should provide value that language constructs cannot replicate.

**Valid use cases:**

| Macro              | Justification                                        |
|--------------------|------------------------------------------------------|
| `CC_ASSERT(expr)`  | Stringification and conditional compilation          |
| `CHECK(a < b)`     | Expression capture for diagnostics                   |
| `LOG(...)`         | Conditional compilation based on build config        |
| `CC_DEFER`         | Scope-exit semantics not realizable without macros   |
| `CC_FORCE_INLINE`  | Wraps compiler-dependent attributes portably         |

If a language feature (template, `constexpr`, inline function) can achieve the same result, prefer it over a macro.

---

## Testing

Test every feature you can using the **nexus companion library.**

**Core testing constructs:**

```cpp
TEST("descriptive test name")
{
    // test body
}

TEST("test with config", disabled, seed(123))
{
    // test body with configuration
}
```

**Organizing tests:**

```cpp
TEST("feature group")
{
    SECTION("specific behavior")
    {
        CHECK(value == expected);
    }

    for (auto i = 0; i < 10; ++i)
    {
        SECTION("subsection nr {}", i)  // parameterized sections
        {
            REQUIRE(critical_condition);
        }
    }
}
```

**Assertions:**
- `CHECK(expr)` — Record failure but continue test
- `REQUIRE(expr)` — Abort test on failure

**Advanced testing:**
- **Fuzz testing** — Property-based testing with random inputs
- **Performance testing** — Measure execution characteristics
- **Benchmarking** — Compare performance across implementations

See [nexus' Catch2-runner compatibility](../libs/base/nexus/docs/catch2-runner-compat.md) for how the
test binaries are discovered and filtered, and [building-and-testing](guides/building-and-testing.md)
for how to run them.

---

## Common Design Pitfalls

This section documents subtle correctness issues that frequently arise in low-level library design.

### Subobject-Safe Move Assignment

A move-assignment operation is **subobject-safe** if it remains well-defined even when the right-hand side aliases a subobject transitively owned by the left-hand side.

In other words, `x = cc::move(y)` must not assume that `y` is independent of `x`; `y` may live inside `x`.

**Common failure mode:**

Many move-assignment implementations eagerly destroy or reset the left-hand side (or the right-hand side) before all reads from the source are complete.

If the source aliases a subobject of the destination, this can destroy the source object mid-assignment, leading to use-after-destruction and undefined behavior.

Simple `this != &other` checks do **not** protect against this.

**Example of the problem:**

```cpp
struct container
{
    int* _data = nullptr;

    // BROKEN: not subobject-safe
    container& operator=(container&& rhs)
    {
        if (this != &rhs)
        {
            delete[] _data;     // destroys _data
            _data = rhs._data;  // but rhs._data might have been inside _data!
            rhs._data = nullptr;
        }
        return *this;
    }
};

// This can break:
container outer;
outer._data = new int[10];
container* inner = reinterpret_cast<container*>(outer._data);
*inner = cc::move(outer);  // inner is destroyed before we read from outer
```

**Design guideline:**

Components in these libraries should aim to provide subobject-safe move assignment, or clearly document when they do not.

This property is essential for composability: if a type `T` is subobject-safe move-assignable, then wrappers and aggregates (e.g., `optional<T>`, containers, resource owners) should preserve that guarantee rather than weakening it through additional side effects on the source object.

**Recommended pattern:**

```cpp
// Safe: read all data from rhs before modifying this
container& operator=(container&& rhs)
{
    if (this != &rhs)
    {
        auto* old_data = _data;
        auto* new_data = rhs._data;

        _data = new_data;
        rhs._data = nullptr;

        delete[] old_data;  // destroy after all reads complete
    }
    return *this;
}
```

---

## Stability & Evolution

**API Stability:** High priority. These are foundational libraries. We reserve the right to make breaking changes where they significantly improve a library.

**ABI Stability:** Low priority. Expect to build from source in most environments.

**Evolution strategy:** Prefer monotonic extension over replacement. When superseding types, aim for "this type remains useful in scenario X, though the newer Y fits most use cases better" rather than "do not use this type anymore."

**Deprecation:** Currently in greenfield phase with no pressing deprecation story.

**Experimental API:** Experimental/"incubator" API is fine but must be contained in `experimental/` for now. Liberal use of `friend` in core API is allowed to access internals in `experimental/`.

---

## Summary Checklist

- [ ] Prose (comments + docs) is one point per line, never a reflowed block
- [ ] New code in the current style; code you edited migrated drive-by, nothing else swept
- [ ] East const (`T const`)
- [ ] Almost-always-auto style; no `T x(args);` constructor-init of variables
- [ ] clang-format applied (120 cols, Allman, clang-format >= 22)
- [ ] Headers compile standalone
- [ ] Designated initializers first; no field-by-field struct filling
- [ ] Use-once descriptions written inline at the call site
- [ ] Bare `sizeof(T)` at call sites, no `cc::isize(sizeof(T))`
- [ ] Non-trivial logic uses static factory methods, not constructors
- [ ] Forward declarations in `fwd.hh`
- [ ] Single-argument ctors are `explicit`
- [ ] All four copy/move operations explicitly defined
- [ ] `[[nodiscard]]` on non-void, non-getter functions
- [ ] `CC_ASSERT` for preconditions and invariants (side-effect free)
- [ ] Raw pointers are non-owning
- [ ] Prefer value types or move-only types over shared ownership
- [ ] Avoid unnecessary copies (use `const&` or view types)
- [ ] Small values (≤ 32 bytes) passed by value
- [ ] Documentation uses `///` with natural language and usage examples
- [ ] Group comments above visibility modifiers
- [ ] Hidden friends for operators where possible
- [ ] No unnecessary `constexpr` or `noexcept`
- [ ] Macros are justified
- [ ] Tests written in nexus
- [ ] Use `impl` namespace (not `detail`) for implementation details
- [ ] Specialization-based customization points live in the `custom` namespace and don't expose `impl` types
- [ ] Consider C++23 deducing `this` where appropriate
- [ ] Move assignment is subobject-safe (or documented otherwise)
- [ ] Avoid `std::`—use `cc::` equivalents (exception: `<type_traits>`)
- [ ] No reliance on UB (or heavily documented/justified)
- [ ] Template bloat minimized (type erasure, thin templates)
- [ ] `static_assert` used for quality error messages in templates
- [ ] Debug/Release behavior is consistent
- [ ] Experimental API in `experimental/` directory
