# Coding Guidelines

This document defines the coding standards and design principles for the clean-core library.
These guidelines prioritize correctness, performance, maintainability, and readability for a foundational C++ library.

---

## Language & Compiler Requirements

**C++ Standard:** C++23 minimum

**Supported Platforms:**
- 64-bit only (Windows, Linux, macOS)
- Architectures: x64 and ARM64

**Compiler Support:**
- **First-class:** Clang and MSVC *(TODO: minimum versions)*
- **Second-class:** GCC (temporary issues may occur, fixes welcome)

---

## Repository Structure

- **`docs/`** — Documentation (prefer Markdown)
- **`src/clean-core/`** — Library implementation (`.hh` and `.cc` files colocated)
- **`tests/`** — Test code using nexus (separate build target)

---

## Naming Conventions

| Element                                       | Convention    | Example                            |
|-----------------------------------------------|---------------|------------------------------------|
| Types (struct, class, enum, concept, typedef) | `snake_case`  | `string_view`, `dynamic_array`     |
| Functions                                     | `snake_case`  | `to_string()`, `get_size()`        |
| Variables                                     | `snake_case`  | `buffer_size`, `input_data`        |
| Enum values                                   | `snake_case`  | `error_none`, `format_utf8`        |
| Namespaces                                    | `snake_case`  | `cc`, `detail`                     |
| Template parameters                           | `UpperCase`   | `template <class T, int Size>`     |
| Private members                               | `_snake_case` | `_internal_state`, `_cached_value` |
| Macros                                        | `UPPER_CASE`  | `CC_ASSERT`, `CC_FORCE_INLINE`     |

**Note:** Template parameters are often re-exposed as `snake_case` type aliases inside the struct/class.

---

## Code Style & Formatting

### General Principles

- **clang-format is mandatory** (currently version 21.1.7). Source files must not change under clang-format execution. Use trailing `//` comments to steer formatting locally when necessary. Header include order is also handled by clang-format.
- One declaration per line. Never `int a, b;`
- Line length should be reasonable for diffs (~100 chars recommended).
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

**Prefer almost-always-auto style:**
```cpp
auto const x = T{ ... };  // preferred
T const x = { ... };       // acceptable but less consistent
T x;                       // fine if initialized later
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
  namespace cc::impl {
      // implementation details here
  }
  ```
  **Note:** The previous convention of using `detail` does not apply anymore.

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
  struct example {
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
void remove_all_where(Pred&& pred) {
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
bool strip_matching_prefix_with(string_view s, EqPred&& eq) {
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
struct texture {
    [[nodiscard]] static cc::result<texture> create_from_file(cc::string_view path);
    [[nodiscard]] static texture create_with_dimensions(int width, int height);

    texture() = default;  // always provide default ctor

    bool is_valid() const { return _width > 0; }

private:
    int _width = 0;  // invalid state when 0
};

// Avoid: complex ctor that can fail
struct texture {
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

Prefer designated initializers where possible:
```cpp
auto const config = Config{
    .buffer_size = 1024,
    .enable_cache = true,
    .timeout_ms = 5000
};
```

Fall back to braced initialization when designated initializers aren't applicable:
```cpp
auto const vec = std::vector<int>{1, 2, 3};
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

Clean-core uses a tiered error handling philosophy:

| Mechanism                 | Use Case                                                             |
|---------------------------|----------------------------------------------------------------------|
| `CC_ASSERT`               | Invariant violations, preconditions, postconditions (programmer bugs) |
| Exceptions                | Exceptional errors requiring non-local control flow                  |
| `cc::result` / `optional` | Frequent or expected failures                                        |

**Assertions:** Use `CC_ASSERT(cond, msg)` liberally in headers and `.cc` files to check preconditions, postconditions, and invariants. Assertions must be side-effect free (the expression must not be load-bearing for correctness; temporary debug output is fine).

**Undefined behavior:** Avoid relying on UB. Each explicit UB usage must be heavily documented and justified.

---

## Integer & Numeric Types

- Use `int` when the size is unimportant (magnitude < millions).
- Use explicitly sized types (`i32`, `u64`, `f32`, etc.) when bit width or precision matters.
- Avoid "magic sentinels" like `-1` for invalid states. Prefer `optional` or `variant` unless there's a justified performance or memory reason.

---

## Standard Library & Dependencies

**Avoid `std::` code.** Almost always use `cc::` equivalents instead.

**Exception:** `<type_traits>` is allowed—these are thin wrappers around compiler intrinsics that we don't re-wrap.

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

---

## Operators & Overloading

Use **hidden friends** for operator overloading where possible:
```cpp
struct vec3 {
    friend vec3 operator+(vec3 a, vec3 b) { return { a.x + b.x, ... }; }
};
```
**Benefits:** Improved compile times, reduced symbol pollution, better ADL control.

---

## Visibility & API Grouping

Use **redundant visibility modifiers** to logically group APIs.
Place group comments above the modifier, preserving space for individual documentation comments:

```cpp
class foo {
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

### Code Comments

- Explain **rationale** and **why**, not just what.
- Use comments to provide grouping and structure.
- Skimming the comments of a longer function should reveal its logical flow.

### Documentation Comments

Use `///` for documentation. **No doc tags, no XML.**

**Design philosophy:** Comments are most often read directly in source code, so they must read naturally without rich formatting tools.

- Write plain, natural language that describes everything important.
- Insert blank lines every few lines to break up walls of text and improve skimmability.
- Keep line length reasonable for diffs.
- **Include at least one usage example** (~2-10 lines) in doc comments for each major struct/data type.

```cpp
/// allocates a new buffer with the specified capacity
///
/// if allocation fails, returns an error
/// the buffer is zero-initialized
///
/// NOTE: capacity must be > 0
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
struct container {
    int* _data = nullptr;

    // BROKEN: not subobject-safe
    container& operator=(container&& rhs) {
        if (this != &rhs) {
            delete[] _data;  // destroys _data
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

Components in this library should aim to provide subobject-safe move assignment, or clearly document when they do not.

This property is essential for composability: if a type `T` is subobject-safe move-assignable, then wrappers and aggregates (e.g., `optional<T>`, containers, resource owners) should preserve that guarantee rather than weakening it through additional side effects on the source object.

**Recommended pattern:**

```cpp
// Safe: read all data from rhs before modifying this
container& operator=(container&& rhs) {
    if (this != &rhs) {
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

**API Stability:** High priority. This is a foundational library. We reserve the right to make breaking changes where they significantly improve the library.

**ABI Stability:** Low priority. Expect to build clean-core from source in most environments.

**Evolution strategy:** Prefer monotonic extension over replacement. When superseding types, aim for "this type remains useful in scenario X, though the newer Y fits most use cases better" rather than "do not use this type anymore."

**Deprecation:** Currently in greenfield phase with no pressing deprecation story.

**Experimental API:** Experimental/"incubator" API is fine but must be contained in `experimental/` for now. Liberal use of `friend` in core API is allowed to access internals in `experimental/`.

---

## Summary Checklist

- [ ] East const (`T const`)
- [ ] Almost-always-auto style
- [ ] clang-format applied (version 21.1.7)
- [ ] Headers compile standalone
- [ ] Designated initializers where possible
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
- [ ] Consider C++23 deducing `this` where appropriate
- [ ] Move assignment is subobject-safe (or documented otherwise)
- [ ] Avoid `std::`—use `cc::` equivalents (exception: `<type_traits>`)
- [ ] No reliance on UB (or heavily documented/justified)
- [ ] Template bloat minimized (type erasure, thin templates)
- [ ] `static_assert` used for quality error messages in templates
- [ ] Debug/Release behavior is consistent
- [ ] Experimental API in `experimental/` directory
