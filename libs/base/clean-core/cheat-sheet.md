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

#include <clean-core/common/assertf.hh>  // cc::format message (compile-time-checked; pulls in cc::format)
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

#include <clean-core/container/small_vector.hh>   // cc::small_vector<T, N> — growable, first N inline (SVO)
cc::small_vector<int, 4> sv;                       // no alloc up to N; spills to heap past N
sv.push_back(1); sv.emplace_back(2);               // push_back/emplace_back/pop_back/clear/resize/reserve
sv.is_inline();                                    // true while still on the inline buffer (no heap held)
```

## Caching & byte serialization

```cpp
#include <clean-core/container/byte_stream_builder.hh>   // cc::byte_stream_builder — build a blob to hash
cc::byte_stream_builder b;                        // backed by cc::vector<byte>; clear() reuses the allocation
b.add(span<byte const>);  b.add_pod(v);           // raw bytes / one trivially-copyable value
b.add_pod_span(range);  b.add_pod_span_sized(r);  // elems' bytes  /  u64 count prefix + elems
b.add_string(sv);  b.add_optional(opt);  b.add_bool(x); // length-prefixed / presence byte + value / one byte
b.written_bytes();                                // -> span<byte const>; feed to cc::hash128::create(...)
cc::byte_stream_builder::thread_local_scratch();  // per-thread instance, cleared on fetch (don't nest)

#include <clean-core/container/key_value_cache.hh>   // cc::key_value_cache<K,V> — thread-safe tiered get-or-create
cc::key_value_cache<cc::hash128, V> cache;        // serialized under cc::mutex; K hashed via cc::make_hash_finalized
cache.add_default_in_memory_provider(max=4096);   // or add_provider(shared_ptr<key_value_provider<K,V>>) (front=fastest)
V v = cache.acquire(key, [&]{ return make(); });  // first tier to hit backfills faster tiers; full miss runs factory
cache.apply_bookkeeping();                        // e.g. in-memory eviction (clear past max_entries)
// tiers: cc::key_value_provider<K,V> (try_get/set/apply_bookkeeping) — extension seam for disk/network caches.
// TODO(clean-core): std::unordered_map inside, to migrate to cc::map when it lands.
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
s.first_n(n);  s.last_n(n);               // leading / trailing n elements; asserts 0<=n<=size (+_clamped variants)
s.size_bytes();                           // -> isize == size()*sizeof(T)
s.reinterpret_as<U>();                    // -> span<U>; U,T trivially-copyable, sizeof(T)%sizeof(U)==0, keeps const
s.try_reinterpret_as<U>();                // -> optional<span<U>>; nullopt if total bytes % sizeof(U) != 0
s.as_bytes();  s.as_mutable_bytes();      // -> span<byte const> / span<byte> (mutable only for non-const T)
cc::as_bytes(c);  cc::as_mutable_bytes(c);// free fns over any data()/size() container (cc/std, string, string_view)
cc::fixed_span<int, 3> fs(ptr);           // compile-time size N; adds get<I>() tuple protocol
cc::enable_borrowed_range<V>;             // opt-in bool trait: true for span/fixed_span/strided_span/string_view

#include <clean-core/container/strided_span.hh>   // cc::strided_span<T> — view with a byte stride
ss.start_ptr();  ss[i];  ss.size();  ss.stride_bytes();
ss.is_contiguous();                       // -> bool
ss.try_as_span();                         // -> cc::optional<span<T>> (nullopt if non-contiguous)
ss.reversed();                            // negated-stride view
```

## Pinned data — shareable owning view (span + shared owner)

```cpp
#include <clean-core/container/pinned_data.hh>
cc::pinned_data<int> pd = cc::pinned_data<int>::create_filled(n, v);  // also create_defaulted/uninitialized/copy_of(span)
pd.data(); pd.size(); pd.size_bytes(); pd[i]; pd.front(); pd.back(); pd.begin(); pd.end();  // span-like; passes as span<T>
pd.span();                                // -> cc::span<T>
pd.pin();                                 // -> std::shared_ptr<void const>; the type-erased owner (for weak_ptr / lifetime)
pd.subdata(off / {.offset,.size} / {.start,.end});   // -> new pinned_data sharing the owner (+subdata_clamped)
pd.reinterpret_as<U>();  pd.try_reinterpret_as<U>();  pd.as_bytes();  pd.as_mutable_bytes();  // like span, new pinned_data sharing owner
cc::pinned_data<int const> c = pd;        // T -> T const conversion, shares owner

cc::as_pinned_data(std::shared_ptr<Container>);   // wrap a shared contiguous container, never copies
cc::make_pinned_data(container_or_shared_ptr);    // shared_ptr -> wrap; owning rvalue -> move; borrow/lvalue -> copy
```

## Strings (UTF-8)

```cpp
#include <clean-core/string/string.hh>    // cc::string — owning, SSO (<= 39 bytes inline on 64-bit), deep-copyable
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
str.as_span();  str.as_mutable_span();            // -> span<char const> / span<char> (content only, no terminator)
str.as_bytes();  str.as_mutable_bytes();          // -> span<byte const> / span<byte>
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
sv.as_span();  sv.as_bytes();                          // -> span<char const> / span<byte const> (no terminator)

#include <clean-core/string/to_string.hh>        // cc::to_string(v) -> cc::string for bool/char/ints/floats/ptr/...
#include <clean-core/string/to_debug_string.hh>  // cc::to_debug_string(v, cfg = {}) -> diagnostics string
// to_debug_string: quotes strings/chars, recurses into ranges [..] and tuples (..); best-effort, non-semantic.

#include <clean-core/string/format.hh>           // cc::format — std::format/fmtlib-style, COMPILE-TIME-checked
cc::format("{} + {} = {}", 1, 2, 3);             // -> cc::string "1 + 2 = 3"   (bad fmt/args = compile error)
cc::format("{:#06x}  {:>8.2f}", 255, 3.14159);   // "0x00ff      3.14"  — fill/align/sign/#/0/width/group/.prec/type
cc::format("{:'}", 1232453254);                  // "1'232'453'254"  — digit grouping (sep = ' , _ … ; 3 dec / 4 hex)
cc::format_append(str, "x={}", 7);               // append into an existing cc::string (no temporary)
str.appendf("x={}", 7);                          // same, as a cc::string member (needs <clean-core/string/format.hh>)
cc::format_to(cc::span<char>(buf, n), "{}", v);  // -> isize, non-allocating; return > n means truncated
// Placeholders: {} auto-index, {N} positional (don't mix), {{ }} escape braces. Types: d/x/X/o/b/c ints,
// f/F/e/E/g/G floats, s string/bool, p pointer. Numbers go via std::to_chars (one seam). No ADL on args.
// Customize: specialize cc::custom::formatter<T> — gets the raw spec string_view; provide
//   static void format(cc::format_sink, cc::string_view spec, T const&) + static consteval void validate(spec).
//   Delegate to the standard grammar via cc::format_value(sink, spec, v) / cc::validate_format_spec(spec).
//   (Or just give T a member to_string() for the plain "{}" case.)

#include <clean-core/string/print.hh>            // print/println -> stdout, eprint/eprintln -> stderr (via fwrite)
cc::print(sv);  cc::println("done");             // raw string_view (braces NOT interpreted); println() = just '\n'
cc::println("{} + {} = {}", 1, 2, 3);            // cc::format string + args (>=1 arg picks the format overload)
cc::eprint("oops: {}", err);  cc::eprintln();    // stderr variants
// println/eprintln ALWAYS flush; print/eprint stay buffered (append your own '\n', or call cc::flush()).
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

#include <clean-core/math/wide_arith.hh>          // portable extended-precision int primitives (constexpr)
cc::umul128(a, b);  cc::imul128(a, b);            // 64x64 -> {lo, hi} (u128 / i128); never overflows
cc::add_with_carry(a, b, carry_in=0);            // -> {value, carry}; sub_with_borrow -> {value, borrow}

#include <clean-core/math/random.hh>
cc::random rng(seed);                     // deterministic PCG32; MOVE-ONLY (use .clone() to duplicate a stream)
rng.next_u32();  rng.next_u64();          // raw uniform bits
rng.uniform(a, b);                        // integer in [a,b] (unbiased) OR float/double in [a,b)
rng.uniform_bool();                       // fair coin
rng.uniform_in(range);  rng.shuffle(range); // pick element / in-place permute (indexable range)
rng.clone();                              // independent generator at the same stream position
```

## Hashing

```cpp
#include <clean-core/common/hash.hh>
cc::make_hash(a, b, ...);                  // u64, COMPOSABLE (not finalized); ordered combine_hash fold
cc::make_hash_finalized(a, ...);           // u64, make_hash + one avalanche; what hash tables consume
cc::combine_hash(a, b);                    // u64 ordered 2->1 join (wyhash mul-fold); a,b are u64 hashes
cc::combine_hash_unordered(a, b);          // u64 = a + b; commutative; inputs MUST be make_hash_finalized
cc::hash_finalize(x);                      // u64 bijective avalanche (moremur)
cc::make_hash_of_bytes(bytes, seed=0);     // u64 XXH3-64 of a span<byte const>
cc::make_hash_range(r);  cc::make_hash_range_unordered(r); // structural fold over a range (ordered / set-like)
// customize a type: 'friend u64 hash(T const&)' (common) OR specialize cc::custom::hash_trait<T> (override; rare)
// built-in: string/string_view (bytes, equal across both); vector/array/span/fixed_array/pair/optional (structural);
//           unique_* containers structural; unique_ptr by pointer identity

#include <clean-core/common/hash128.hh>
cc::hash128{.low=lo, .high=hi};            // 128-bit value, two u64 limbs; ==, <=> (lex by low,high)
cc::hash128::create(bytes, seed);          // XXH3 128-bit of a span<byte const> + u64 seed (content-addr IDs)
hash(h128);                                // hidden-friend customization point -> low limb (u64)
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

#include <clean-core/thread/thread.hh>
cc::set_current_thread_name("uploader");  // best-effort OS thread name (UTF-8; ≤15 bytes on Linux)

#include <clean-core/thread/threaded_actor.hh> // actor with its own thread + typed message mailbox;
                                                // messages processed one-at-a-time in global send order
class uploader : public cc::threaded_actor_impl<upload_job, flush_cmd> {   // one on_message per type
protected:
    void on_message(upload_job j) override { ... }   // runs on the actor thread; state needs no locks
    void on_message(flush_cmd) override { ... }
    // opt-in hooks: actor_name(), on_thread_init(), on_thread_shutdown(), on_process()->bool
};
auto a = cc::make_and_start_threaded_actor<uploader>(args...); // -> cc::unique_ptr<threaded_actor<...>>
a->enqueue_message(upload_job{...});      // -> bool (false if shutting down); a->shutdown() drains + joins
auto impl = a->take_impl<uploader>();     // std::unique_ptr — only after shutdown; ~handle joins too

// Unthreaded mode: no background thread; you drive the loop (only option on single-threaded wasm).
auto b = cc::make_threaded_actor<uploader>(args...);
b->start(cc::threaded_actor_mode::unthreaded);
b->process_messages_if_unthreaded();      // one cycle -> bool "more to do"; no-op when a thread runs
b->process_messages_if_unthreaded_for_ms(4.0); // loop until idle or 4ms; safe to call every frame
```

## Async / dataflow (incubator — see docs/async.md)

```cpp
#include <clean-core/thread/async.hh>     // cc::async<T> — eventual result<T, async_error>; value/dataflow model
cc::shared_async<T> = std::shared_ptr<cc::async<T>>;   // the normal handle (async<T> is non-copy/non-move)

// creation — pick eager (scheduled) or lazy (cold) explicitly at the call site. f may take a leading
// cc::async_context& or omit it; extra args are dependencies (shared_async or shared_ptr<once_async>),
// awaited + unwrapped to plain values before f runs; errors short-circuit. T deduced or explicit.
auto a = cc::make_async_lazy([]{ return 40; });                          // cold; no context, no deps
auto s = cc::make_async_scheduled<int>([](cc::async_context&){ ... });   // eager (scheduled now if a worker scope active)
auto c = cc::make_async_lazy([](int x, int y){ return x + y; }, a, s);   // depends on a,s; f gets plain ints
auto d = a->map_lazy([](int x){ return x + 2; });   // single-dep transform (also map_scheduled; no plain `map`)
auto o = cc::make_once_lazy([]{ return 7; });        // std::shared_ptr<once_async<int>>; consume as a dep exactly once
auto m = cc::make_async_manual<int>();               // promise-style: external_pending until pushed

// driving BLOCKS the calling thread (busy-waits vs a real scheduler) — top-level/tests only, never in a frame:
int v = cc::async_blocking_get(a);                               // -> T (asserts on error/cancel)
cc::result<int, cc::async_error> r = cc::try_async_blocking_get(a); // fallible drive
a->is_ready();  a->has_value();  a->has_error();
std::shared_ptr<int const> pv = a->try_value();   // zero-copy alias; null unless ready with a value
std::shared_ptr<cc::async_error const> pe = a->try_error();
m->push_value(41);  m->push_error(cc::async_error::make_error(cc::any_error("x")));  // complete a manual node

// raw compute frame (perf-critical state machine): async_result<T>(async_context&). Multi-branch frames MUST
// annotate -> cc::async_result<T>.
[step=0](cc::async_context& actx) mutable -> cc::async_result<int> {
    if (step++ == 0) return actx.await([]{ return 10; });   // child frame may also omit async_context
    return actx.success(5); };
actx.require(dep);              // -> bool ready (no subscription); else records a pending dep, return wait
actx.spawn_child(frame);       // -> once_async<CT>* owned child dep (child frame dies before parent frame)
actx.await(frame);             // spawn_child + wait_for_dependencies()
actx.success(v);  actx.error(async_error|any_error);  actx.wait_for_dependencies();  actx.yield();

// driving decoupled from any executor (the seam); default is inline, on the calling thread:
cc::inline_scheduler sched;  cc::async_worker_scope scope(sched);   // bind scheduler to this thread
root->schedule();  sched.run_until([&]{ return root->is_ready(); }); // pump; interleave external push here
cc::once_async<T>                 // single-consumer owned child (2nd subscription asserts); not a default type

// concurrent execution: work-stealing pool (#include <clean-core/thread/async_thread_pool.hh>)
cc::async_thread_pool pool(cc::num_hardware_threads());  // >=1 workers serving general (bit 0) by default
cc::install_default_async_pool(pool);                    // general-compute nodes route here
int v = pool.blocking_get(root);                         // submit + block THIS (foreign) thread; not from a worker
// affinity: typed u32 mask, overlap => eligible; bit 0 = general (default). One pool per class:
cc::async_thread_pool rpool(2, cc::async_affinity{0b10});     // serves bit 1
auto t = cc::make_async_lazy([]{ ... });                      // create LAZY, then:
t->set_affinity(cc::async_affinity{0b10}, &route_to_rpool);   // freeze-on-schedule; route = raw fn ptr to its pool
// Not here yet: co_await, plain (non-async) dep args, nested once deps, typed/shared errors, eager child reclaim.
```

## Strings — encoding conversion

```cpp
#include <clean-core/string/conversion.hh>
cc::vector<char16_t> u16 = cc::utf8_to_utf16(sv); // BMP -> 1 unit, astral -> surrogate pair; bad -> U+FFFD
                                                  // NOT NUL-terminated (push_back(u'\0') if you need it)
```

## Platform

```cpp
#include <clean-core/platform/win32_sanitized.hh> // safe to include ANYWHERE; on Windows pulls in
                                                   // <Windows.h> with WIN32_LEAN_AND_MEAN + NOMINMAX,
                                                   // elsewhere expands to nothing. The sanctioned
                                                   // way to reach windows.h in shaped-core.
#include <clean-core/platform/native.hh>
cc::demangle_symbol(symbol)                        // cc::string — human-readable C++ symbol name

#include <clean-core/platform/stacktrace.hh>       // cc::stacktrace = std::stacktrace where available
cc::stacktrace::current();                          // CC_HAS_STACKTRACE guards rendering (empty stub on wasm)

#include <clean-core/error/crash_handler.hh>
cc::install_crash_handler();                        // segfault/abort/etc -> stderr: reason + hooks + stacktrace
cc::add_crash_context_hook(&fn);                    // void()noexcept printed before the trace (keep it tiny)
```

## Gotchas

- **Assertions are on in debug & relwithdebinfo, off in release.** The default
  presets are `relwithdebinfo-*` (asserts ON). If you touch assert-gated code,
  also build a `release-*` preset. `CC_ASSERT`'s message argument is mandatory.
- **`isize` is signed `i64`, not `size_t`** — intentional, to avoid unsigned
  underflow. `find`/`rfind` return **`-1`** (not a huge unsigned) on no-match.
- **`string` / `string_view` are NOT null-terminated.** `data()` is not a C
  string — use `str.c_str_materialize()` (valid only until the next mutation).
- **`string` SSO holds ≤ 39 bytes inline** (on 64-bit; fewer where pointers are smaller, e.g. wasm32) before it heap-allocates.
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
