# clean-core cheat sheet

Foundational C++23 building blocks that replace most `std::` usage: primitives,
assertions, owning containers and views, strings, fallible value types,
callables, memory, and low-level utilities. Namespace `cc`. **No dependencies.**

Headers are included by full path from `src/`:
`#include <clean-core/<topic>/<name>.hh>`. `fwd.hh` forward-declares the public
types. This is a fast-recall map — for the *why*, read the header `///` docs and
[coding-guidelines](../../../docs/coding-guidelines.md).

How to read this: each block leads with the include; one symbol per line with a
trailing comment giving the return type / intuition. Format conventions live in
[docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

## Primitives & types

```cpp
#include <clean-core/fwd.hh>           // forward decls + primitive type aliases
i8 i16 i32 i64                         // signed ints  (vocabulary types — write them bare inside our libs)
u8 u16 u32 u64                         // unsigned ints
f32 f64                               // float / double
byte                                  // std::byte
isize                                 // SIGNED size/index type (= i64), NOT size_t — used everywhere
// Prefer sized types when range matters; plain `int` is fine for small loop counters.

// They live in cc::primitive_defines. To get them bare in YOUR library (not the global namespace),
// pull them into your own namespace once, e.g. in your fwd.hh:
namespace my_lib { using namespace cc::primitive_defines; }   // nexus does this -> nx::i32, nx::isize, ...
```

## Assertions

```cpp
#include <clean-core/common/assert.hh>   // leanest; string-literal message (no <format>/<string>)
CC_ASSERT(cond, "message");              // message is MANDATORY; on fail: debug-break then abort
CC_ASSERT_ALWAYS(cond, "msg");           // stays active even in release builds
CC_UNREACHABLE("msg");                   // assert(false) + compiler unreachable hint
CC_DEBUG_BREAK();                        // break if a debugger is attached, else no-op

#include <clean-core/common/asserts.hh>  // runtime cc::string_view message
CC_ASSERTS(cond, sv);  CC_ASSERTS_ALWAYS(cond, sv);

#include <clean-core/common/assertf.hh>  // std::format message (pulls in <format>)
CC_ASSERTF(idx < n, "index {} out of range {}", idx, n);
CC_ASSERTF_ALWAYS(cond, "fmt {}", x);
// Enabled in debug + relwithdebinfo, stripped in release (unless CC_ENABLE_ASSERT_IN_RELEASE).
// For invariants/pre/postconditions only — never for user input or expected errors (use result<>).
```

## Containers — owning (value semantics, deep copy)

```cpp
#include <clean-core/container/vector.hh>   // cc::vector<T> — growable, owns cc::allocation<T>
auto v = cc::vector<int>::create_defaulted(n);   // also: create_filled(n, val), create_copy_of(span),
                                                 //       create_uninitialized(n) [trivial T only],
                                                 //       create_with_capacity(n), create_with_resource(res)
v.push_back(x);  v.emplace_back(args...);        // append (reallocates if needed)
v.push_back_stable(x);                           // append, asserts spare capacity (no realloc)
v.pop_back();                                    // -> T (moved out);  remove_back() discards
v.remove_at(i);  v.remove_at_unordered(i);       // erase by index (ordered / O(1) swap-with-last)
v.remove_all_where(pred);  v.remove_all_value(x);// -> isize removed;  also remove_first/last_*
v.retain_all_where(pred);                        // keep only matching
v.resize_to_defaulted(n);                        // also resize_to_filled/_uninitialized/_constructed,
                                                 // clear_resize_to_*, resize_down_to(n)
v.reserve(n);  v.reserve_exact(n);  v.shrink_to_fit();
v.size();  v.empty();  v.capacity();  v[i];  v.front();  v.back();  v.data();  v.begin();  v.end();
v.clear();  v.fill(x);  auto a = v.extract_allocation();

#include <clean-core/container/unique_vector.hh>  // cc::unique_vector<T> — move-only, else same API
#include <clean-core/container/array.hh>          // cc::array<T> — fixed-size heap, no growth ops
#include <clean-core/container/unique_array.hh>   // cc::unique_array<T> — move-only array
// array factories mirror vector's create_* (no push/pop/resize); has fill / extract_allocation.

#include <clean-core/container/fixed_array.hh>     // cc::fixed_array<T, N> — inline T[N] aggregate
cc::fixed_array<int, 3> fa = {1, 2, 3};            // operator[], front/back, data, begin/end, size()
auto& [a, b, c] = fa;                              // tuple protocol: get<I>(), structured bindings
```

## Views — non-owning (must not outlive their data)

```cpp
#include <clean-core/container/span.hh>
cc::span<int> s(ptr, n);                  // also span(begin,end), span(c_array), span(container)
cc::span<int const> cs = {1, 2, 3};       // init-list ctor ONLY for const T, ONLY as a temporary arg
s[i];  s.front();  s.back();  s.data();  s.begin();  s.end();  s.size();  s.empty();
s.subspan(off);  s.subspan({.offset=o,.size=n});  s.subspan({.start=a,.end=b});   // asserts valid range
s.is_subspan(off / {.offset,.size} / {.start,.end});   // -> bool; true => matching subspan won't assert
s.subspan_clamped(...);                   // same 3 overloads, clamps range into [0,size()] instead of asserting
cc::fixed_span<int, 3> fs(ptr);           // compile-time size N; adds get<I>() tuple protocol

#include <clean-core/container/strided_span.hh>   // cc::strided_span<T> — view with a byte stride
ss.start_ptr();  ss[i];  ss.size();  ss.stride_bytes();
ss.is_contiguous();                       // -> bool
ss.as_span();                             // -> cc::optional<span<T>> (nullopt if non-contiguous)
ss.reversed();                            // negated-stride view
```

## Strings (UTF-8)

```cpp
#include <clean-core/string/string.hh>    // cc::string — owning, SSO (<= 39 bytes inline), deep-copyable
cc::string str = "shaped";                // ctors: char, (ptr,size), (begin,end), c-string, container
auto s2 = cc::string::create_filled(n, 'x');   // also create_copy_of(sv), create_uninitialized(n),
                                               //       create_with_capacity(n), create_copy_c_str_materialized(sv)
str.push_back('!');  str.append(sv);  str += other;  auto j = str + "x";   // mutate / concat
str.size();  str.empty();  str[i];  str.data();   // data() is NOT null-terminated
str.front();  str.back();  str.compare(o);  str.find(x,pos=0);  str.rfind(x,pos=-1);   // string_view reads forwarded
str.subview(off / {.offset,.size} / {.start,.end});   // -> string_view (invalidated by mutation)
str.substring(off / {.offset,.size} / {.start,.end}); // -> owning cc::string copy
str.replace_all(from, to);                        // -> isize count; char/char or sv/sv (empty from = no-op)
str.replace_first(from, to);  str.replace_last(from, to);   // -> bool; char/char or sv/sv
str.replace({.offset,.size} / {.start,.end}, with);         // replace a range with a string_view
str.is_small();                                   // -> bool (currently in SSO mode)
str.c_str_materialize();                          // -> char const* '\0'-terminated (valid until next mutation)
str.c_str_if_terminated();                        // -> char const* or nullptr if not terminated

#include <clean-core/string/string_view.hh>   // cc::string_view — non-owning; implicitly from cc::string
cc::string_view sv = "abc";               // ctors: (ptr,size), (begin,end), c-string, literal, container
sv.subview(off);  sv.subview({.offset=o,.size=n});  sv.subview({.start=a,.end=b});   // named-range (cc::offset_size/start_end)
sv.subview_clamped(off, len);
sv.remove_prefix(n);  sv.remove_suffix(n);
sv.starts_with(x);  sv.ends_with(x);  sv.contains(x);   // x = string_view or char
sv.find(x, pos = 0);  sv.rfind(x, pos = -1);            // -> isize, or -1 if not found
sv.compare(o);  sv == o;  sv < o;                       // lexicographic

#include <clean-core/string/to_string.hh>        // cc::to_string(v) -> cc::string for bool/char/ints/floats/ptr/...
#include <clean-core/string/to_debug_string.hh>  // cc::to_debug_string(v, cfg = {}) -> diagnostics string
// to_debug_string: quotes strings/chars, recurses into ranges [..] and tuples (..); best-effort, non-semantic.
```

## Optional & result (fallibility)

```cpp
#include <clean-core/error/optional.hh>   // cc::optional<T> — value | cc::nullopt (no operator* / ->)
cc::optional<int> o = 42;  o = cc::nullopt;
o.has_value();  o.value();                // value() ASSERTS when empty (no exception)
o.value_or(fallback);                     // value or fallback
o.emplace_value(args...);                 // construct in place -> T&
o.map([](int x){ return x + 1; });        // -> optional<U>  (void f -> optional<unit>)
cc::optional<T>::create_emplaced(args...);// for immovable T
o == other;  o == value;                  // equality (no relational ops)

#include <clean-core/error/result.hh>     // cc::result<T, E = cc::any_error> — value | error sum type
cc::result<int> f() { return 42; }        // success: implicit from a T
cc::result<int> g() { return cc::error("boom"); }      // error: ALWAYS via cc::error(...)
r.has_value();  r.has_error();
r.value();  r.error();                    // both ASSERT on the wrong state
r.value_or(fb);  r.error_or(fb);  r.value_assert("msg");
r.or_throw();                             // -> T&& or throws cc::result_exception
r.with_context("while parsing");          // chain context (E = any_error); also with_context_lazy
CC_RETURN_IF_ERROR(expr);                 // early-return the error from the current function
// cc::any_error: with_context(msg), site(), to_string(), has_stacktrace() — move-only, type-erased.
```

## Callables

```cpp
#include <clean-core/function/function_ref.hh>   // cc::function_ref<R(Args...)> — NON-owning, trivially copyable
void process(cc::function_ref<int(int)> f);      // bind any lambda/fn-ptr/functor; must outlive the ref
f(42);  f.is_valid();  bool(f);

#include <clean-core/function/unique_function.hh> // cc::unique_function<R(Args...)> — owning, MOVE-ONLY
cc::unique_function<void()> uf = [x = make_thing()]{ use(x); };   // captures move-only / pinned state
auto uf2 = cc::unique_function<void()>::create_from(alloc, args...);  // in-place with explicit node_allocator
uf();  uf.is_valid();                     // backed by cc::node_allocation (very cheap)
```

## Memory (lower-level)

```cpp
#include <clean-core/memory/allocation.hh>        // cc::allocation<T> — move-only owning block + live range
auto al = cc::allocation<int>::create_defaulted(n, res);  // create_empty/_filled/_uninitialized/_copy_of
al.obj_span();  al.resource();  al.is_valid();  al.alloc_size_bytes();
cc::memory_resource;  cc::default_memory_resource;        // pluggable allocator interface (default backed by mimalloc)
cc::system_memory_resource;                               // malloc/free opt-out; pass &it as `res` to bypass mimalloc

#include <clean-core/memory/node_allocation.hh>   // cc::node_allocation<T> — move-only single-object slab handle
auto na = cc::node_allocation<T>::create_from(cc::default_node_allocator(), args...);
*na;  na->member;  na.is_valid();

#include <clean-core/memory/unique_ptr.hh>        // cc::unique_ptr<T> — move-only single-object owner (wraps node_allocation)
auto p = cc::make_unique<T>(args...);              // *p; p->member; p.get(); p.is_valid(); if (p) ...
p = nullptr;                                       // destroys + clears (no reset()); ==/!= vs ptr/nullptr; hidden-friend hash
```

## Utility & bit

```cpp
#include <clean-core/common/utility.hh>
cc::move(v);  cc::forward<T>(v);  cc::exchange(obj, new);  cc::swap(a, b);
cc::min(a, b);  cc::max(a, b);  cc::clamp(v, lo, hi);  cc::min({a, b, c});
cc::is_power_of_two(x);  cc::align_up(v, align);  cc::align_down(v, align);  cc::is_aligned(v, align);
cc::int_div_round_up(n, d);  cc::wrapped_increment(pos, max);
cc::invoke(f, args...);                   // calls callables AND member pointers uniformly
cc::offset_size{.offset=o, .size=n};  cc::start_end{.start=a, .end=b};   // named-range args (span/string subview/replace)
CC_DEFER { cleanup(); };                  // run at scope exit — CAPTURES BY REFERENCE
cc::overloaded{ [](int){}, [](float){} }; // combine callables into one overload set (for visit)

#include <clean-core/math/bit.hh>
cc::has_single_bit(x);  cc::bit_ceil(x);  cc::bit_floor(x);  cc::bit_width(x);
cc::bit_rotate_left(x, n);  cc::bit_rotate_right(x, n);  cc::popcount(x);
cc::count_leading_zeroes(x);  cc::count_trailing_zeroes(x);  // + _ones variants
cc::atomic_add(v, x);                     // also atomic_sub/and/or/xor (via std::atomic_ref) -> old value

#include <clean-core/math/random.hh>
cc::random rng(seed);                     // deterministic PCG32; MOVE-ONLY (use .clone() to duplicate a stream)
rng.next_u32();  rng.next_u64();          // raw uniform bits
rng.uniform(a, b);                        // integer in [a,b] (unbiased) OR float/double in [a,b)
rng.uniform_bool();                       // fair coin
rng.uniform_in(range);  rng.shuffle(range); // pick element / in-place permute (indexable range)
rng.clone();                              // independent generator at the same stream position
```

## Sequence (lazy ranges — emerging API)

```cpp
#include <clean-core/sequence/sequence.hh>   // cc::sequence<RangeT> — single-pass; non-copy/non-move
cc::sequence{some_range}.count();            // use as a temporary; terminal ops consume the range
cc::sequence{v}.count_if(pred);  .any(pred);  .all(pred);  .index_of(pred);  .find(pred);  .sum();
cc::sequence{v}.accumulate(init, fn);  .each(fn);
cc::sequence{v}.to_vector();  .to_array();  .to_container<C>();  .push_to(existing);
```

## Threading

```cpp
#include <clean-core/thread/mutex.hh>      // cc::mutex<T> — Rust-style: data only reachable under the lock
cc::mutex<std::vector<int>> m;
m.lock([](auto& d){ d.push_back(1); });   // -> result of the callback
m.try_lock([](auto& d){ ... });           // -> cc::optional<R> (or bool for void) — nullopt if not acquired
m.wait(cv, pred, [](auto& d){ ... });     // wait on condition_variable, then operate
```

## Gotchas

- **Assertions are on in debug & relwithdebinfo, off in release.** The default
  presets are `relwithdebinfo-*` (asserts ON). If you touch assert-gated code,
  also build a `release-*` preset. `CC_ASSERT`'s message argument is mandatory.
- **`isize` is signed `i64`, not `size_t`** — intentional, to avoid unsigned
  underflow. `find`/`rfind` return **`-1`** (not a huge unsigned) on no-match.
- **`string` / `string_view` are NOT null-terminated.** `data()` is not a C
  string — use `str.c_str_materialize()` (valid only until the next mutation).
- **`string` SSO holds ≤ 39 bytes inline** before it heap-allocates.
- **`optional` has no `operator*` / `operator->`.** Use `value()`, which
  *asserts* when empty rather than throwing.
- **Return errors with `cc::error(...)`** — never an implicit conversion.
  `value()`/`error()` assert on the wrong state; use `CC_RETURN_IF_ERROR` to
  propagate.
- **Move-only types:** `unique_vector`, `unique_array`, `unique_function`,
  `any_error`, `allocation`, `node_allocation`. Plain `vector`/`array`/`string`
  copy *deeply* (value semantics) — reach for the `unique_*` variant to forbid copies.
- **`function_ref` is non-owning** — the referenced callable must outlive it
  (binding a temporary is UB). `CC_DEFER` captures by reference — keep captured
  state alive.
- **`create_uninitialized` requires a trivial `T`.**
- **Not yet implemented (stubs — don't reach for these):** `map`, `set`,
  `ringbuffer`, `fixed_vector`, `bitset`, `tuple`, `variant`, `disjoint_set`,
  and `flags`. Check the header before relying on one.
