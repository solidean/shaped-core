# clean-core cheat sheet

Foundational C++23 building blocks that replace most `std::` usage.
Primitives, assertions, owning containers and views, strings, fallible value types, callables, memory, and low-level utilities.
Namespace `cc`.
**No dependencies.**

Headers are included by full path from `src/`: `#include <clean-core/<topic>/<name>.hh>`.
`fwd.hh` forward-declares the public types.

This is a fast-recall map — for the *why*, read the header `///` docs and [coding-guidelines](../../../docs/coding-guidelines.md).
[docs/containers.md](docs/containers.md) has the contracts every container shares: choosing one, what `T` must be, bounds checking, and invalidation.

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type / intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

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

#include <clean-core/common/assert-handler.hh>   // who gets called when one fires
cc::impl::scoped_assertion_handler h(fn);        // push on THIS THREAD's stack; a throwing handler is fine, it cannot leak to another thread
cc::impl::scoped_fallback_assertion_handler f(fn); // process-wide, consulted when the failing thread's own stack is empty
// The fallback is a plain function pointer and fires on threads nobody scoped — worker threads — so it must not throw.
```

## Containers — owning (value semantics, deep copy)

```cpp
#include <clean-core/container/vector.hh>   // cc::vector<T> — growable, owns cc::allocation<T>
auto v = cc::vector<int>::create_defaulted(n);   // also: create_filled(n, val), create_copy_of(span),
                                                 //       create_uninitialized(n) [trivial T only],
                                                 //       create_with_capacity(n), create_with_resource(res)
v.push_back(x);  v.emplace_back(args...);        // append (reallocates if needed)
v.push_back_stable(x);                           // append, asserts spare capacity (no realloc)
v.push_back_range(rng);                          // append a whole range (sized -> single reservation); push_back_range_stable(rng) needs capacity
v.insert_at(i, x);  v.emplace_at(i, args...);    // insert at index -> T&; i == size() appends; arg may reference v
v.insert_range_at(i, rng);                       // insert a SIZED range at index -> cc::span<T> of what landed
v.replace_range({.offset = i, .size = n}, rng);  // [head][n at i][tail] -> [head][rng][tail]; n and rng.size() are
                                                 // independent, tail moves ONCE -> span the rng now occupies
                                                 // insert_range_at / replace_range: rng must NOT alias v (asserts)
v.pop_back();                                    // -> T (moved out);  remove_back() discards
v.remove_at(i);  v.remove_at_unordered(i);       // erase by index (ordered / O(1) swap-with-last)
v.remove_at_range({.offset = i, .size = n});     // erase a run; also remove_at_range_unordered
v.remove_from_to(start, end);                    // the [start, end) spelling; also remove_from_to_unordered
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

#include <clean-core/container/pair.hh>            // cc::pair<T, U> — aggregate .first/.second, ==/<=>/hash
#include <clean-core/container/tuple.hh>           // cc::tuple<Ts...> — INDEX-based access only (no get<T>)
auto t = cc::tuple{1, 2.5f, cc::string("hi")};     // CTAD decays; default ctor VALUE-initializes (like pair)
t.get<0>();  get<0>(t);                            // member or free; both forward the value category
auto& [a2, b2, c2] = t;                            // structured bindings
t.emplace<2>("bye");                               // destroys element 2, constructs in place (no move needed)
t == t;  t < t;  cc::make_hash(t);                 // element-wise ==, lexicographic <=>, structural hash
cc::apply(f, t);                                   // spreads elements as arguments (works on pair/fixed_array too)

#include <clean-core/container/variant.hh>         // cc::variant<Ts...> — alternatives must be PAIRWISE DISTINCT
cc::variant<int, cc::string> v = 42;               // value ctor takes EXACT type matches only
v.emplace<1>("hi");  v.index();                    // index-based mutation; index() is always valid
v.visit([](int i){ … }, [](cc::string& s){ … });   // handlers are combined into one overload set
v.is<cc::string>();                                // -> bool; a non-alternative T does not compile
v.as<cc::string>();                                // -> string& / string const& / string&&; asserts on a different one
v.try_as<cc::string>();                            // -> string* / string const*; null on a different one (no rvalue overload)
v.take<cc::string>();                              // -> string, moved out; v stays valid, holds a moved-from string
v.try_take<cc::string>();                          // -> optional<string>; nullopt on a different one
cc::variant<int, immovable>::create_emplaced<1>(7);// prvalue, so immovable alternatives work

#include <clean-core/container/small_vector.hh>   // cc::small_vector<T, N> — growable, N-min inline (SVO)
cc::small_vector<int, 4> sv;                       // 48 B here; N is a MINIMUM inline cap; over-aligned T OK
sv.push_back(1); sv.emplace_back(2);               // push_back/emplace_back/pop_back/clear/resize/reserve
sv.push_back_range(rng);                           // also insert_at/emplace_at/insert_range_at/replace_range
sv.is_inline();                                    // true while still on the inline buffer (no heap held)
sv.inline_capacity();                              // actual inline cap >= N (auto-grows to fill footprint; 9 here)

#include <clean-core/container/fixed_vector.hh>  // cc::fixed_vector<T, N> — inline, N is a HARD capacity cap
cc::fixed_vector<int, 4> fv;                       // never allocates; pushing past N ASSERTS (no heap spill)
fv.push_back(1); fv.emplace_back(2);               // mirrors vector, minus reserve*/shrink_to_fit/extract_allocation
fv.push_back_range(rng);                           // also insert_at/emplace_at/insert_range_at/replace_range
fv.full();  fv.capacity();                         // -> bool;  -> N (static constexpr)

#include <clean-core/container/ringbuffer.hh>     // cc::ringbuffer<T> — O(1) push/pop at BOTH ends, grows
auto rb = cc::ringbuffer<int>::create_with_capacity(64); // capacity is ALWAYS a power of two (rounded up)
rb.push_back(1);  rb.push_front(0);                // symmetric; emplace_/try_/_stable/_overwriting variants too
rb.pop_front();  rb.pop_back();                    // -> T (nodiscard); remove_front/_back drop it, remove_*_n(k)
rb.try_pop_front();                                // -> optional<T>; nullopt when empty (the drain-loop spelling)
rb.push_back_overwriting(2);                       // full => drops the front instead of growing (newest-N window)
rb[0];  rb.front();  rb.back();                    // index 0 is the FRONT; no data(), the content can WRAP
auto [s0, s1] = rb.segments();                     // -> pair<span<T>,span<T>>; s1 empty unless wrapped
rb.linearize();                                    // -> span<T> over all of it; no-op when not wrapped
rb.push_back_stable(3);                            // asserts instead of growing => references stay valid
rb.reserve(5); rb.capacity();                      // -> 8: reserve is exact, a push doubles instead

#include <clean-core/container/fixed_ringbuffer.hh> // cc::fixed_ringbuffer<T, N> — inline, any N, never allocates
cc::fixed_ringbuffer<float, 128> hist;             // same API minus reserve/shrink_to_fit/_stable (always stable)
hist.push_back_overwriting(dt);                    // the fixed-size history window; push_back would ASSERT when full

#include <clean-core/container/bitset.hh>          // cc::bitset — runtime bit count, heap u64 words
auto bs = cc::bitset::create_defaulted(n);         // n unset bits; create_filled(n, true) / create_with_capacity(n)
bs.set(i);  bs.set(i, on);  bs.unset(i);  bs.toggle(i);  // bs[i] -> bool (const) / a cc::bit_ref PROXY (mutable)
bs.set_all();  bs.set_all(on);  bs.unset_all();    // unset_all keeps size(); clear() makes size() 0
bs.set_range(start, count);                        // word-wise; unset_range/toggle_range too
bs.any_set();  bs.none_set();  bs.all_set();       // NO empty() on purpose — it would read as none_set()
bs.set_bit_count();  bs.unset_bit_count();         // -> isize
bs.find_first_set();  bs.find_last_set();          // -> isize, -1 if none; find_first_unset/find_last_unset too
for (auto i : bs.set_indices()) {}                 // one step per SET bit, not per bit; unset_indices() likewise
bs.set_all_of(o);  bs.retain_all_of(o);            // union / intersection, IN PLACE; asserts equal size()
bs.unset_all_of(o);  bs.toggle_all_of(o);          // difference / symmetric difference; no |&^~ operators at all
bs.has_all(o);  bs.has_any(o);  bs.is_disjoint(o);  bs.intersection_bit_count(o);
cc::bitset::create_union_of(a, b);                 // + create_intersection_of/_difference_of/_symmetric_difference_of
bs.push_back(true);  bs.pop_back();                // -> bool (nodiscard); remove_back() drops it
bs.resize_to_filled(n, value);  bs.reserve(n);  bs.shrink_to_fit();

#include <clean-core/container/fixed_bitset.hh>    // cc::fixed_bitset<N> — inline, constexpr, never allocates
cc::fixed_bitset<8> mask;                          // 1 BYTE (smallest word covering N); all-zero, unlike fixed_vector
template <cc::fixed_bitset<8> Mask> void f();      // structural: usable as a non-type template parameter
mask = cc::fixed_bitset<8>::create_from_u64(0b1011);  // N <= 64 only; .to_u64() back

#include <clean-core/container/disjoint_set.hh>   // cc::disjoint_set<IdxT> — union-find over elements 0..count-1
auto ds = cc::disjoint_set<i32>::create_singletons(n);  // also create_with_capacity(n) / create_with_resource(res)
ds.element_count();  ds.partition_count();  ds.capacity();  // NO size(): it wouldn't say elements or sets
ds.merge_by_element(a, b);                         // -> bool merged (false = they were already together)
ds.merge_by_representative(ra, rb);                // both args MUST be representatives
ds.get_representative(e);                          // -> IdxT; NON-const, a find rewires the links it walked
ds.are_in_same_set(a, b);  ds.size_of_set_by_element(e);    // non-const for the same reason
ds.get_parent(e);  ds.is_representative(e);  ds.size_of_set_by_representative(r);  // const: one link, no compression
ds.add_element();  ds.add_elements(k);             // -> IdxT first new index; each new element is a singleton
ds.append(other);                                  // -> IdxT offset; keeps other's partitioning, merges nothing across
ds.compute_components(out_comp_to_repr, out_elem_to_comp);  // both OVERWRITTEN, capacity reused -> no alloc on repeat
auto const c = ds.compute_components();            // same into a fresh {component_to_representative, element_to_component}
c.component_count();                               // -> isize; == ds.partition_count()
ds.reserve(n);  ds.clear();  ds.reset_to_singletons(n);
```

## Associative

```cpp
#include <clean-core/container/map.hh>            // cc::map<K,V> — separate-chaining hash map
cc::map<cc::string, int> m;                       // K/V may be immovable; refs stay valid across other inserts/grows
m[key] = v;                                        // mutable op[]: get-or-default-insert (needs V(), K(key)); NO const op[]
m.get(key);                                        // -> V& / V const&, ASSERTS present (heterogeneous, no insert)
m.get_ptr(key);                                    // -> V* / V const*, nullptr if absent
m.contains(key);                                   // -> bool (heterogeneous)
m.get_to(key, out);                                // -> bool; copies value into out& only on hit (const)
m.get_or(key, fallback);                           // -> V by value (value or fallback copy) (const)
m.get_or_default(key);                             // -> V by value (value or V{}) (const)
m.erase(key);  m.clear();  m.reserve(n);           // erase -> bool removed;  clear keeps buckets;  reserve grows buckets
m.size();  m.empty();  m.bucket_count();
for (auto [k, v] : m) { v += 1; }                  // proxy {K const& key; V& value}; order arbitrary; mutate v via ref

auto e = m.entry(key);                             // one lookup, reused for insert (Rust-style); copies the probe key
if (!e.exists()) e.emplace(vargs...);              // vacant path: K from probe key, V from vargs, no re-hash/re-lookup
e.value();  e.key();                               // valid once occupied;  e.get_or_emplace(vargs...) -> V&
e.emplace_with_key(kargs, vargs...);               // rare: build K from explicit args instead of the probe key
// entry is invalidated by ANY structural mutation of the map (incl. another entry's emplace) — don't mutate in between.
// heterogeneous lookup needs the probe type to hash-equal & compare-equal to K (e.g. string_view vs string).
// on a string-keyed map a literal probes directly: m["axis"], m.get("axis"), m.erase("axis") — no view to name.
// keys must hash well-mixed: buckets mask low bits. default_hash finalizes; a custom Hash MUST avalanche.
// move: O(1) (nodes stay put).  copy: deep, only if K and V are copyable (else move-only).
// customize: cc::map<K,V,Hash,KeyEqual> — Hash{}(k)->u64 (transparent), KeyEqual{}(a,b)->bool (transparent).

#include <clean-core/container/set.hh>            // cc::set<T> — hash set (a cc::map<T, unit> underneath, value is free)
cc::set<cc::string> s;                             // same properties as map: immovable T ok, stable refs, heterogeneous
bool added = s.insert(x);                          // -> true if newly added; builds T from x (heterogeneous = emplace)
s.contains(x);  s.erase(x);                        // -> bool (heterogeneous);  erase -> true if removed
s.size();  s.empty();  s.clear();  s.reserve(n);  s.bucket_count();
for (T const& x : s) { ... }                       // elements are immutable; order arbitrary
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
// TODO(clean-core): std::unordered_map inside; cc::map has landed, the migration has not.
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
cc::as_pinned_data(pinned_data<T>);               // identity: an already-pinned range passes through, same owner
cc::make_pinned_data(container_or_shared_ptr);    // pinned_data/shared_ptr -> wrap; owning rvalue -> move; borrow/lvalue -> copy
```

## Strings (UTF-8)

The contracts behind these — null-termination, invalidation, SSO transitions, hashing — live in [strings](docs/strings.md).
Formatting has [its own page](docs/formatting.md).

```cpp
#include <clean-core/string/string.hh>    // cc::string — owning, SSO (<= 39 bytes inline on 64-bit), deep-copyable
cc::string str = "shaped";                // ctors: char, (ptr,size), (begin,end), c-string, container
auto s2 = cc::string::create_filled(n, 'x');   // also create_copy_of(sv), create_uninitialized(n),
                                               //       create_with_capacity(n), create_copy_c_str_materialized(sv)
str.push_back('!');  str.append(sv);  str += other;  auto j = str + "x";   // mutate / concat
str.resize_to_uninitialized(n);  str.resize_to_defaulted(n);  str.resize_to_filled(n,'x');  str.resize_down_to(n);
str.clear_resize_to_uninitialized(n);        // + _defaulted(n) / _filled(n,'x') — discard content, then size to n
str.reserve_back(n);                         // n MORE bytes; + reserve_back_exact(n); no-op while SSO already fits
str.reserve_front(n);  str.reserve_front_exact(n);  // front slack; a small string always allocates (SSO has no front offset)
str.capacity_back();  str.capacity_front();  // -> isize free bytes at back / front (front is 0 while SSO)
str.shrink_to_fit();                         // release excess capacity; content that fits inline ALWAYS returns to SSO
str.size();  str.empty();  str[i];  str.data();   // data() is NOT null-terminated
str.front();  str.back();  str.compare(o);  str.find(x,pos=0);  str.rfind(x,pos=-1);   // string_view reads forwarded
str == o;  str < o;                          // o = string / string_view / literal, on either side (operator<=>)
str.subview(off / {.offset,.size} / {.start,.end});   // -> string_view (dies on the next non-const operation)
str.substring(off / {.offset,.size} / {.start,.end}); // -> owning cc::string copy
str.replace_all(from, to);                        // -> isize count; char/char or sv/sv (empty from = no-op)
str.replace_first(from, to);  str.replace_last(from, to);   // -> bool; char/char or sv/sv
str.replace({.offset,.size} / {.start,.end}, with);         // replace a range with a string_view; in place unless `with` aliases str
str.insert_at(i, c);  str.insert_range_at(i, sv);           // insert at byte index; i == size() appends; sv must NOT alias str
str.is_small();                                   // -> bool (currently in SSO mode)
str.as_span();  str.as_mutable_span();            // -> span<char const> / span<char> (content only, no terminator)
str.as_bytes();  str.as_mutable_bytes();          // -> span<byte const> / span<byte>
str.c_str_materialize();                          // -> char const* '\0'-terminated (valid until the next non-const op)
str.c_str_if_terminated();                        // -> char const* or nullptr if not terminated

#include <clean-core/string/string_view.hh>   // cc::string_view — non-owning, trivially copyable
// string -> string_view is free; string_view -> string is ALSO implicit, and copies (allocating past SSO)
cc::string_view sv = "abc";               // ctors: (ptr,size), (begin,end), c-string, literal, container
sv.subview(off);  sv.subview({.offset=o,.size=n});  sv.subview({.start=a,.end=b});   // named-range (cc::offset_size/start_end)
sv.subview_clamped(off, len);
sv.remove_prefix(n);  sv.remove_suffix(n);
sv.starts_with(x);  sv.ends_with(x);  sv.contains(x);   // x = string_view or char
sv.find(x, pos = 0);  sv.rfind(x, pos = -1);            // -> isize, or -1 if not found
sv.compare(o);  sv == o;  sv < o;                       // lexicographic by UNSIGNED byte (no locale/collation)
sv.as_span();  sv.as_bytes();                          // -> span<char const> / span<byte const> (no terminator)

#include <clean-core/string/interned_string.hh>  // cc::interned_string — 8-byte handle to one canonical copy
cc::interned_string id = cc::intern("transform");  // process-wide table, created on first use, never destroyed
id.as_string_view();  id.size();  id.empty();      // as_string_view is a load, not a lookup
a == b;                                   // POINTER compare (canonical within a table); != too
// NO relational operators: the two orders are not interchangeable, so the call site names the one it means
a.compare_bytes(b);                       // -> strong_ordering by canonical BYTES; reproducible, costs a memcmp
a.compare_identity(b);                    // -> strong_ordering by process-local identity; one pointer compare,
                                          //    and a DIFFERENT order every run — only where nobody sees the order
cc::interned_string::by_bytes{};  cc::interned_string::by_identity{};   // the matching sort predicates
hash(id);                                 // precomputed; == hash(id.as_string_view())
cc::interned_string{};                    // the empty string, == intern("") in any table
cc::string_interner t;  t.intern(s);  t.size();    // caller-owned table, for tests that want isolation
// Thread-safe: intern from any thread, no external locking (sharded internally; free when CC_HAS_THREADS == 0).
// A caller-owned table must OUTLIVE every handle it handed out; the process-wide one has no such question.

#include <clean-core/string/glob.hh>             // path globbing (shaped-linter configs, nexus' file filters)
cc::glob_normalize_path(p);               // -> cc::string: '\'->'/', repeated/trailing slashes dropped, /c/x = C:\x = c:/x
cc::glob_matches(pattern, path, {});      // options are NOT defaulted; {} = exact + case-sensitive, both sides already normalized
cc::glob_matches(pat, path, cc::glob_option::normalize | cc::glob_option::ignore_case);   // cc::flags<cc::glob_option>
// '?' one char, '*' a run — neither crosses '/'; '**' does, and the '/' after it is optional ("src/**/x" matches "src/x").
// A pattern ending in '/' means the subtree. Nothing is anchored for you: match a suffix with a leading "**/".
// Normalize once yourself when one side is reused across many comparisons; the option redoes both sides per call.

#include <clean-core/string/to_string.hh>        // cc::to_string(v) -> cc::string for bool/char/ints/floats/ptr/...
#include <clean-core/string/to_debug_string.hh>  // cc::to_debug_string(v, cfg = {}) -> diagnostics string
// to_debug_string: quotes strings/chars, prints pointers as ptr(0xHEX), recurses into ranges [..] and tuples (..).
// Best-effort and non-semantic — the output is unstable across builds, so never parse it.

#include <clean-core/string/format.hh>           // cc::format — std::format/fmtlib-style, COMPILE-TIME-checked
cc::format("{} + {} = {}", 1, 2, 3);             // -> cc::string "1 + 2 = 3"   (bad fmt/args = compile error)
cc::format("{:#06x}  {:>8.2f}", 255, 3.14159);   // "0x00ff      3.14"  — fill/align/sign/#/0/width/group/.prec/type
cc::format("{:'}", 1232453254);                  // "1'232'453'254"  — digit grouping (sep = ' , _ … ; 3 dec / 4 hex)
cc::format_append(str, "x={}", 7);               // append into an existing cc::string (no temporary)
str.appendf("x={}", 7);                          // same, as a cc::string member (needs <clean-core/string/format.hh>)
cc::format_to(cc::span<char>(buf, n), "{}", v);  // -> isize, non-allocating; return > n means truncated
// Placeholders: {} auto-index, {N} positional (don't mix), {{ }} escape braces. No ADL on args.
// Types: d/x/X/o/b/B/c ints, f/F/e/E/g/G floats, s string/bool (bool also takes d/b/B/o/x/X), p pointer.
// The full grammar and the per-type table: docs/formatting.md.
// Customize: specialize cc::custom::formatter<T> — gets the raw spec string_view; provide
//   static void format(cc::format_sink, cc::string_view spec, T const&) + static consteval void validate(spec).
//   Delegate to the standard grammar via cc::format_value(sink, spec, v) / cc::validate_format_spec(spec).
//   (Or just give T a member to_string() for the plain "{}" case.)

#include <clean-core/string/print.hh>            // print/println -> stdout, eprint/eprintln -> stderr (via fwrite)
cc::print(sv);  cc::println("done");             // raw string_view (braces NOT interpreted); println() = just '\n'
cc::println("{} + {} = {}", 1, 2, 3);            // cc::format string + args (>=1 arg picks the format overload)
cc::eprint("oops: {}", err);  cc::eprintln();    // stderr variants
// println/eprintln ALWAYS flush; print/eprint stay buffered (append your own '\n', or call cc::flush()).
// println(sv) writes text and newline separately, so concurrent printers can interleave; the format overloads are one write.
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

#include <clean-core/error/exception.hh>  // the blessed route to <exception> / <stdexcept>; declares nothing itself
// Return a result; where something is genuinely exceptional (a lost device, or_throw), throw through this header.
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
cc::default_node_allocator();                             // node_allocator& — this thread's default; inline TLS load,
                                                          // resolved once per thread (never null)
cc::set_default_node_allocator(&a);                       // repoint this thread's default; nullptr resets (re-resolves)
cc::get_default_node_allocator();                         // node_allocator* — raw slot, null if not resolved yet
cc::scoped_default_node_allocator g(&a);                  // scoped override; restores the previous slot (nests)
// GOTCHA: an allocator must outlive every node allocated from it — frees are pointer-derived and never consult an
// allocator, so nothing detects a violation. Deregister before it dies (~node_allocator asserts it isn't installed).

#include <clean-core/memory/unique_ptr.hh>        // cc::unique_ptr<T> — move-only single-object owner (wraps node_allocation)
auto p = cc::make_unique<T>(args...);              // *p; p->member; p.get(); p.is_valid(); if (p) ...
p = nullptr;                                       // destroys + clears (no reset()); ==/!= vs ptr/nullptr; hidden-friend hash

#include <clean-core/memory/shared_ptr.hh>         // cc::shared_ptr<T, Traits=default_shared_traits<T>> — 8 B, intrusive
// PROVISIONAL: shaped by cc::async's needs, its only user; the Traits protocol is expected to be simplified (docs/systems/shared-ptr.md)
auto s = cc::make_shared<T>(args...);              // only way to construct (strong=1); *s; s->m; s.get(); default: [T|control]
cc::weak_ptr<T> w = s;  w.lock();                   // weak (if Traits::supports_weak); lock() -> shared_ptr or empty
// custom Traits (all static, on T*) for intrusive counts: node_size/node_align + init_control/inc_strong/
// release_strong->{destroy,free}/destroy_object/free_storage (+weak: inc_weak/release_weak->bool/try_lock_strong).
// release_strong {true,false} => destroy_object THEN release_weak (order is load-bearing); {true,true} => both, skip
// release_weak. Base-keyed Traits serves derived T via upcast (cc::async keys one on async_node_base);
// from_alive(T*) MINTS a handle from a pointer known alive (strong > 0).
T* raw = s.release();  auto back = cc::shared_ptr<T>::adopt(raw);   // MOVE a count out of / into a handle: count-neutral
// release/adopt (twin of weak_ptr's) park a strong count in hand-rolled storage (tagged word, lock-free array of
// raw ptrs) with no inc/dec pair. Whoever holds the raw pointer owes the release, or it leaks.
cc::fused_refcount                                 // strong hi / weak lo in one atomic<u64>; both stock Traits use it
// sole-owner (1,1) teardown = one acquire load, ZERO locked RMWs; leaves the counts untouched (nothing may read them)
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
cc::memcpy(dst, src, n);  cc::memmove(dst, src, n);  cc::memset(dst, v, n);  cc::memcmp(a, b, n);  // <cstring>'s, blessed here
// NOT strlen / strcmp: cc::string_view(cstr) walks it once and knows .size(), and two views compare with ==.
CC_DEFER { cleanup(); };                  // run at scope exit — CAPTURES BY REFERENCE
cc::overloaded{ [](int){}, [](float){} }; // combine callables into one overload set (for visit)

#include <clean-core/math/bit.hh>
cc::has_single_bit(x);  cc::bit_ceil(x);  cc::bit_floor(x);  cc::bit_width(x);
cc::bit_rotate_left(x, n);  cc::bit_rotate_right(x, n);  cc::popcount(x);
cc::count_leading_zeroes(x);  cc::count_trailing_zeroes(x);  // + _ones variants

#include <clean-core/common/endian.hh>            // durable formats: the byte order is the FORMAT's, never the host's
cc::load_bytes_le<u32>(bytes, offset=0);          // -> u32; byte-wise, so buffer alignment never enters into it
cc::load_bytes_be<f64>(bytes, offset=0);          // floats go through their bit pattern; NaN payloads and -0.0 survive
cc::store_bytes_le<u64>(bytes, offset, v);  cc::store_bytes_be<i16>(bytes, offset, v);
// T is an integer or float of 1/2/4/8 bytes. `bool` is REJECTED: it has no unique object representation,
// so a stored byte other than 0 or 1 would load as a bool that is neither true nor false — store a u8 you validate.
// offset + sizeof(T) must be in range: a CC_ASSERT, so bounds-check untrusted input BEFORE loading.

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

## Timing

The seam that replaces `<chrono>` — which only `time.cc` may include, being the most expensive header MSVC ships.
Seconds as a `double` is the vocabulary; there are no `time_point` / `duration` types.

```cpp
#include <clean-core/common/time.hh>
cc::current_time_steady_secs();            // -> double; MONOTONIC, arbitrary zero => only a DIFFERENCE means anything
                                           //    the one to measure "how long did this take" with
cc::current_time_wall_secs();              // -> double; seconds since the Unix epoch, comparable across processes/runs
                                           //    the one to PERSIST (an expiry, a timestamp) — and it can step either way,
                                           //    so a difference of two readings is not a duration
cc::has_cycle_counter();                   // -> constexpr bool; false on ARM and WASM
cc::current_cycles();                      // -> u64 TSC on x86, 0 where there is none; inline, for a benchmark's inner loop
                                           //    constant-rate, so it tracks wall time rather than work done, and nothing
                                           //    here converts it to seconds (the rate needs calibration you do yourself)
```

## Flags

One macro at global scope, taking the enum's namespace separately — it emits the `cc::custom::enum_traits` specialization,
which is only legal outside the namespace, and reopens the namespace itself for the `|` `&` `^` that ADL can only find inside it.

Two macros, one per encoding, because the choice is not detectable from the enum: nothing about `e = 4` says whether it means bit 4 or bit 2.

```cpp
#include <clean-core/common/flags.hh>

// a PLAIN enum — each value gets a bit of its own, `e` taking bit `e`
namespace app { enum class shape { visible, selected, locked }; }
CC_FLAG_ENUM_INDEXED(app, shape, u32);             // namespace, enum, flag storage

// values that ALREADY ARE bit patterns, so one value may name several bits
namespace app { enum class usage : u32 { vertex = 1u << 0, index = 1u << 1, both = 0b11 }; }
CC_FLAG_ENUM_BITMASK(app, usage, u32);

cc::flags<app::shape> f = app::shape::visible;     // a single flag converts implicitly
f = app::shape::visible | app::shape::locked;      // cc::flags<app::shape>
f.bits;                                            // the raw storage; public, so cc::flags works as a template parameter

f.has(app::shape::locked);                         // bool; under bit_mask, a subset test for a value naming several bits
f.has_any(other);  f.has_all(other);               // bool; `other` may be a single flag
f.is_empty();  f.set_bit_count();                  // bool / i32 (popcount of bits, NOT of enumerators)
f.without(other);                                  // cc::flags; there is no operator~

f.set(v);  f.set(v, on);  f.remove(v);  f.toggle(v);  f.clear();
f |= other;  f &= other;  f ^= other;              // also | & ^, ==, and cc::make_hash
cc::flags<app::shape>::create_from_bits(0b101);
cc::flag_enum<app::shape>;                         // concept: did this enum opt in?
cc::flags<app::shape>::encoding;                   // cc::flag_encoding::bit_index or ::bit_mask
```

The storage is stated, never inferred — `CC_FLAG_ENUM_BITMASK(app, usage, u8)` packs into one byte whatever the enum's own underlying type is.
It also bounds the values: a bit_mask pattern that does not fit asserts, as does a bit_index at or beyond the storage's bit count.

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
//   the protocol, the tier order and the reasoning: docs/customization-points.md
// built-in: string/string_view (bytes, equal across both); vector/array/span/fixed_array/pair/optional (structural);
//           unique_* containers structural; unique_ptr by pointer identity
// a char array hashes as the string it holds (needs string_view.hh), which is what makes m["literal"] work
//   a raw char const* still hashes by ADDRESS — convert it to a string_view before using it as a key
```

`common/hash.hh` is the protocol — how a *type* participates in hashing, which is why the containers depend on it.
The digest algorithms below live in `bytes/` instead, alongside the other algorithms over byte ranges.

## Bytes

```cpp
#include <clean-core/bytes/hash128.hh>
cc::hash128{.low=lo, .high=hi};            // 128-bit value, two u64 limbs; ==, <=> (lex by low,high)
cc::hash128::create(bytes, seed);          // XXH3 128-bit of a span<byte const> + u64 seed (content-addr IDs)
hash(h128);                                // hidden-friend customization point -> low limb (u64)

#include <clean-core/bytes/hash256.hh>
cc::hash256{.l0=..,.l1=..,.l2=..,.l3=..};  // 256-bit value, four u64 limbs; ==, <=> (lex by l0..l3, NOT byte order)
cc::hash256::create(bytes);                // = cc::blake3::create; BLAKE3-256 of a span<byte const>
h256.to_bytes(out32);  cc::hash256::from_bytes(in32);  // the durable 32-byte form: l0 first, each limb little-endian
hash(h256);                                // hidden-friend customization point -> l0 (u64)

#include <clean-core/bytes/blake3.hh>     // the CRYPTOGRAPHIC hash — for content addressing, not for maps
cc::blake3::create(bytes);                 // -> hash256, one-shot
cc::blake3 h;  h.update(bytes);  h.finalize();  h.reset();  // streaming: hash a sequence without concatenating it
// finalize() is const and repeatable; update() may continue after it
// ~8-20x slower than XXH3 depending on input size (tests/benchmarks/hash-benchmark.cc measures both)

#include <clean-core/bytes/compression.hh>  // zstd + lz4; the algorithm is a VALUE a format writes down
cc::compress(bytes);                       // -> vector<byte>; zstd, default level, framed. NO failure channel: preconditions assert
cc::compress(bytes, {.algorithm = cc::compression_algorithm::lz4, .level = -4});
cc::compress_into(bytes, out, cfg);        // -> result<isize> written; out >= compress_bound(size, cfg)
cc::compress_bound(size, cfg);             // -> isize worst case; a buffer this large always suffices
cc::decompress(blob);                      // -> result<vector<byte>>; sniffs the frame magic
cc::decompress(blob, {.algorithm = a, .max_output_size = n});  // untrusted input MUST cap; default is no limit
cc::decompress_into(blob, out, cfg);       // -> result<isize>; the only form that reads an lz4 `raw` blob
cc::detect_algorithm(blob);                // -> optional<algorithm>; nullopt on raw and on garbage
cc::decompressed_size(blob);               // -> optional<isize> the frame declares, without decompressing
cc::compressor c(cfg);  c.compress(bytes); // reuse tier for MANY SMALL blobs; not thread-safe, one per thread
cc::decompressor d(cfg);  d.decompress(blob);
// level is the ALGORITHM'S own scale, never normalized; 0 = its default. zstd 1..22 (default 3), negatives are fast modes
// framing: frame (magic + size + checksum, sniffable) | raw (stripped; format must record algorithm, and size for lz4)
// under ~a few hundred bytes NOTHING compresses without a dictionary — see docs/systems/compression.md

#include <clean-core/bytes/compression_dictionary.hh>  // shared context that makes small blobs compress at all
cc::compression_dictionary::from_bytes(algo, raw);     // adopt content; lz4 has no other form
cc::compression_dictionary::train(algo, samples, n);   // -> result<dict>; zstd only, ~100 samples minimum
dict.algorithm();  dict.id();  dict.bytes();           // id() is what a format records; 0 when there is none
// a const dictionary is shareable across threads, unlike the compressors themselves

#include <clean-core/bytes/compression_stream.hh>  // compression as a stream filter; `frame` framing only
cc::decompressing_read_stream_adapter::create(inner, {.algorithm = a});  // -> result<adapter>; not seekable
cc::compressing_write_stream_adapter::create(inner, cfg);
w.finish();                                // -> result<i64>; MUST be called or the frame is unreadable
```

## Comparators

```cpp
#include <clean-core/common/compare.hh>    // what the ordering algorithms take as `compare`
cc::default_less{}   cc::default_greater{} // a < b / b < a; transparent, only operator< required
cc::compare_by(p1, p2, ...);               // lexicographic: order by p1, break ties with p2, and so on
cc::descending(p);                         // reverses just that one projection inside a compare_by
// each projection goes through cc::invoke, so &type::member works; each is re-evaluated per comparison.
// A comparator must be a STRICT WEAK ORDERING — the sorts assert on one that is not.
cc::sort(entries, cc::compare_by(&entry::group, cc::descending(&entry::score), &entry::name));
```

## Sorting & selection (see [sorting](docs/sorting.md))

pdqsort driven purely by index get + index swap — nothing is ever parked in a temporary, which is what lets one call permute several ranges at once.
Takes any **indexed range** (`size()` + `operator[]`): vector, array, span, strided_span, fixed_array, or your own.

```cpp
#include <clean-core/algorithm/sort.hh>
cc::sort(values);                          // ascending; deterministic, NOT stable; O(n log n) worst case
cc::sort(values, cc::default_greater{});   // cc::default_less / cc::default_greater live in common/compare.hh
cc::sort(values, [](auto& a, auto& b) { return a.score < b.score; });
cc::sort_by(values, &entry::key);          // key fn / pointer-to-member / pointer-to-member-function
cc::sort_descending(values);  cc::sort_by_descending(values, key);
cc::sort_by_cached_key(values, key);       // evaluates key n times into a temp buffer (allocates); sort_by does it O(n log n) times

cc::sort_multi(cmp, keys, a, b, ...);      // sorts keys, applies the SAME permutation to every other range
cc::sort_multi_ascending(keys, a, b);  cc::sort_multi_descending(keys, a, b);   // the cmp-less spellings
cc::sort_multi_by(key, cmp, xs, ys);       // key receives one element of EVERY range
cc::sort_indices(order, keys);             // permutes `order`, keys untouched; ties break on the index => stable
cc::sort_stable(values);  cc::sort_stable_by(values, key);   // equal elements keep their order.
                                           // ALLOCATES n indices — never an element buffer, so no-parking survives

cc::sort_and_dedup(values);                // -> isize new size; sorts, drops duplicates, SHRINKS the container
                                           // (needs resize_down_to; for a view: sort + cc::dedup_sorted_ex)

cc::partition_by(values, is_right);        // -> isize first index of the "right" block; O(n), not stable
cc::partition_stable(values, is_right);    // same, order kept within each block; O(n log n) swaps, NO allocation
                                           // (allocating alternative for big n: cc::sort_stable_by(v, is_right))
cc::sort_at(values, idx);                  // element idx as a full sort would leave it; O(n) even worst case (+ _by)
cc::sort_window(values, {.offset, .size}); // that whole window, sorted; O(n + size log size); may run past the end (+ _by)
cc::sort_first(values, k);                 // the top-k spelling of sort_window({.offset = 0, .size = k})

cc::is_sorted(values);  cc::is_sorted_by(values, key);              // -> bool, O(n)
cc::is_strictly_sorted(values);  cc::is_strictly_sorted_by(v, key); // same, but no two elements may be equivalent
```

Sorting in parallel — its own header, since `sort.hh` must not pull in the async machinery:

```cpp
#include <clean-core/algorithm/sort_async.hh>
auto const sorted = cc::sort_async(values);        // -> cc::shared_async<cc::unit>; NEVER SCHEDULED = NEVER SORTED
auto const next = cc::make_async_lazy([&](cc::unit) { ... }, sorted);   // composing is the point, not the speedup
cc::sort_async_ex(start, size, range, cmp, cutoff);  // seam form; `cutoff` exists so tests can drive the recursion
// values must OUTLIVE it, not be resized, and not be read by anyone else — it permutes throughout.
// The comparator is COPIED per task => must be a pure function. A non-strict-weak one is a data race here,
// not just local corruption. ~6.9x on 31 workers for random i32 @4M; ~parity on already-ordered input.
// No blocking convenience on purpose: pool.blocking_get asserts inside a worker of that same pool.
```

Rearranging by position rather than by comparison — all swap-only, so it composes with the rest:

```cpp
#include <clean-core/algorithm/permutation.hh>
cc::reverse(values);                       // in place
cc::rotate(values, k);                     // left by k, so values[k] ends up first; k is NORMALIZED (any value legal)
cc::apply_permutation(values, indices);    // values[i] <- what indices[i] pointed at (a GATHER, matching sort_indices)
                                           // CONSUMES indices, leaving it the identity — that is what buys O(n)
cc::invert_permutation(indices);           // in place; a sort order becomes RANKS. Index type must be SIGNED.
cc::reverse_ex(start, size, range);  cc::rotate_ex(start, size, k, range);   // seam forms
cc::apply_permutation_ex(size, indices, range);   // + a _multi range => permute parallel arrays in step
cc::partition_stable_ex(start, size, is_right, range);  // is_right takes an INDEX, called once per element
```

Binary search over an already-ordered range.
`_in_sorted` is a contract marker: O(log n) against a precondition YOU must meet.

```cpp
#include <clean-core/algorithm/search.hh>
cc::partition_point(values, pred);           // -> isize; first index where a MONOTONE pred goes false
                                             //    precondition is "partitioned by pred", weaker than sorted => no suffix
cc::first_at_least_in_sorted(values, x);     // -> isize  (std::lower_bound); size() when everything is below
cc::first_greater_in_sorted(values, x);      // -> isize  (std::upper_bound)
cc::find_in_sorted(values, x);               // -> cc::optional<isize>; an index, so finding + using is ONE search
cc::find_range_in_sorted(values, x);         // -> cc::offset_size; ALL equivalents. size 0 = absent,
                                             //    and offset is then the insertion point
// all take an optional comparator, which must be the one the range is ordered by.
// Scanning an UNordered range is cc::sequence's job (.find / .any / .count_if), not this header's.
```

The seam underneath, for data that is not a range — an SoA view, a GPU-side handle array, a proxy.
Its own header, so partitioning and the orderedness queries cost nothing of the pdqsort machinery:

```cpp
#include <clean-core/algorithm/index_swap_range.hh>   // sort.hh includes this; the reverse is not true
cc::index_swap_range<R>                    // concept: r.element_get(i) + r.element_swap(a, b). NO size().
cc::as_index_swap_range(values);           // + _by(values, key), _multi(keys, vals...), _multi_by(key, vals...)
cc::partition_ex(start, size, is_right, range); // is_right takes an INDEX
cc::is_sorted_ex(start, size, range, cmp);      // + is_strictly_sorted_ex
cc::dedup_sorted_ex(start, size, range, cmp);   // -> isize surviving count; compacts runs, does NOT shrink
// an adapter must be trivially copyable and cheap — it is copied down the recursion (sort_ex static-asserts it).

#include <clean-core/algorithm/sort.hh>
cc::sort_ex(start, size, range, cmp, should_sort); // should_sort(start, size) -> bool prunes subranges (=> sort_at)
```

## Sequence (lazy ranges — early prototype, see [sequence](docs/sequence.md))

```cpp
#include <clean-core/sequence/sequence.hh>   // cc::sequence<RangeT> — single-pass; non-copy/non-move
cc::sequence{some_range}.count();            // use as a temporary; terminal ops consume the range
cc::sequence{v}.count_if(pred);  .any(pred);  .all(pred);  .index_of(pred);  .find(pred);  // find needs stable elements
cc::sequence{v}.accumulate(init, fn);  .each(fn);
cc::sequence{v}.to_vector();  .to_container<C>();  .push_to(existing);
// NOT implemented despite being declared: .sum() and .to_array(). map/filter/take/zip and the factories do not exist yet.
```

## Threading

```cpp
#include <clean-core/thread/atomic.hh>     // MANDATORY seam: never name std::atomic directly (see docs/blessed-stdlib-headers.md)
cc::atomic<T> a{0};                        // == std::atomic<T>; a plain T with the same API when CC_HAS_THREADS == 0
a.load(cc::memory_order_acquire);  a.store(v, cc::memory_order_release);  a.fetch_add(1, cc::memory_order_relaxed);
a.compare_exchange_weak(expected, desired, cc::memory_order_acq_rel, cc::memory_order_relaxed);
cc::atomic_ref<T> r(plain_lvalue);         // == std::atomic_ref<T>; atomics over memory you own as a plain value
cc::atomic_flag f;                         // constinit-able, trivially destructible (survives static teardown)
cc::atomic_thread_fence(cc::memory_order_seq_cst);   // no-op without threads
cc::atomic_add(v, x);                      // also atomic_sub/and/or/xor — RMW on a plain lvalue -> OLD value (seq_cst)
                                           // no wait()/notify(): without threads nobody could ever satisfy the wait

#include <clean-core/thread/mutex.hh>      // cc::mutex<T> — Rust-style: data only reachable under the lock
cc::mutex<std::vector<int>> m;
m.lock([](auto& d){ d.push_back(1); });   // -> result of the callback
m.try_lock([](auto& d){ ... });           // -> cc::optional<R> (or bool for void) — nullopt if not acquired
m.wait(cv, pred, [](auto& d){ ... });     // wait on condition_variable, then operate
auto g = m.lock_scoped();                 // -> cc::mutex_guard<T> — RAII hold; g-> / *g reach the value, released when g dies
                                          // move-only. NOT the default: lock(f) is, and it keeps references inside the callback.
                                          // for the critical section that cannot be one call (spans your statements / handed to a caller)

#include <clean-core/thread/thread.hh>
cc::set_current_thread_name("uploader");  // best-effort OS thread name (UTF-8; ≤15 bytes on Linux; no-op where unavailable)
int p = cc::num_hardware_threads();       // >= 1 always; a "don't know" (0) from the platform becomes 1
auto id = cc::current_thread_id();        // cc::thread_id (enum class : u64); equality only — NOT the OS id a debugger shows
cc::mark_current_thread_as_main();        //   claims cc::thread_id::main for this thread; nothing does it implicitly
                                          //   cc::thread_id::invalid (0) is the "no thread" sentinel

#include <clean-core/thread/spin.hh>
cc::spin_pause();                         // CPU spin-wait hint, for a SHORT bounded spin; never a substitute for blocking
                                          // not a scheduling yield: nothing goes to the OS, and it is a no-op where unsupported

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

// Unthreaded mode: no background thread (the only option on single-threaded wasm).
auto b = cc::make_threaded_actor<uploader>(args...);
b->start(cc::threaded_actor_mode::unthreaded);  // registers a pump for its lifetime -> whoever BLOCKS runs it
b->process_messages_if_unthreaded();      // one cycle -> bool "more to do"; no-op when a thread runs
b->process_messages_if_unthreaded_for_ms(4.0); // loop until idle or 4ms; safe to call every frame
// the two above are the primitive; you rarely call them - cc::thread_pump_all() is what drives every actor at once
```

## Cooperative pumping (thread/thread_pump.hh)

```cpp
// A semantic thread with no OS thread registers a pump; every blocking wait sweeps the registry instead of sleeping.
// cc::threaded_actor does this for you when started unthreaded; a hand-rolled thread does it where it would spawn.
auto reg = cc::register_thread_pump([&] { return step_once(); }); // -> RAII; true == "made progress / more to do"
cc::thread_pump_all();                    // -> bool; one cycle of every registration. One atomic load when empty
cc::thread_pump_all_for(4.0);             // loop until idle or 4ms; true == stopped on the budget
cc::registered_thread_pump_count();       // -> isize; a leak check at the end of a run
// GOTCHA: a pump must NOT block on another registration - it holds the only thread, so that one never runs.
//   Sweep from inside a pump instead (this one is skipped, the others run). Blocking on a GPU fence / OS handle is fine.
```

## Async / dataflow (incubator — see docs/systems/async.md)

```cpp
#include <clean-core/thread/async.hh>     // cc::async<T, E = async_error> — eventual result<T, E>; dataflow model
cc::shared_async<T, E = async_error> = cc::shared_ptr<cc::async<T, E>, impl::async_node_traits>; // 8 B intrusive handle

// creation — pick eager (scheduled) or lazy (cold) explicitly at the call site. f may take a leading
// cc::async_context<T, E>& or omit it; extra args are dependencies (shared_async), awaited + unwrapped to plain
// values before f runs; errors short-circuit. T deduced (context-free) or explicit; E defaults to async_error.
auto a = cc::make_async_lazy([]{ return 40; });                             // cold; no context, no deps
auto s = cc::make_async_scheduled<int>([](cc::async_context<int>&){ ... });  // eager: worker scope, else default pool
auto c = cc::make_async_lazy([](int x, int y){ return x + y; }, a, s);      // depends on a,s; f gets plain ints
auto d = cc::make_async_lazy([](int x){ return x + 2; }, a);   // single-dep transform (one-arg variadic form)
auto m = cc::make_async_manual<int>();               // promise-style: external_pending until pushed
auto p = cc::make_async_lazy_emplace<int, cc::async_error, PinnedFrame>(7); // builds the FRAME in place: immovable f
                                                     // ok. T explicit, no dep args (capture + require instead)
// born already ready (no frame, no scheduling); _emplace builds in place (immovable T ok):
auto rv = cc::make_async_from_value(42);   auto re = cc::make_async_from_error<int>(async_error::make_cancelled());
auto rvE = cc::make_async_from_value_emplace<Immovable>(7);  // T explicit; also *_from_error_emplace<T, E>(...)

// you never block on an async: a SCHEDULER drives it. These drive on the AMBIENT one and BLOCK the calling
// thread — a bridge from sync code, not a way to write it. Ready on return: value or error, never pending.
int v = cc::async_blocking_get(a);                                   // -> T (asserts on error/cancel)
cc::result<int, cc::async_error> r = cc::try_async_blocking_get(a);  // fallible; no "not yet" to handle
cc::try_async_blocking_get_for(a, 50);                               // -> cc::optional<result>, the only "not yet"
int v2 = cc::async_blocking_get_on(pool, a);                         // drive on THIS scheduler, not the ambient one
cc::result<int, cc::async_error> r2 = cc::into_result(cc::move(a)); // CONSUME a ready handle: MOVES value/error out
a->is_ready();  a->has_value();  a->has_error();
int const* pv = a->try_value();   // zero-copy, non-owning; null unless ready with a value
cc::async_error const* pe = a->try_error();   // typed E const*; null unless ready with an error
m->push_value(41);  m->push_error(cc::async_error::make_error(cc::any_error("x")));  // complete a manual node

// raw compute frame (perf-critical state machine): async_step_status(async_context<T, E>&) — resolves via ctx,
// returns a status. Annotate -> cc::async_step_status and give T explicitly (make_async_lazy<int>). A frame
// may create + require new deps mid-compute (dynamic dependencies). Raw frames do NOT auto-propagate a dep's
// error — check dep->try_error() and decide (transform/ignore/propagate); the make_async_* sugar DOES.
[step=0, dep=cc::shared_async<int>()](cc::async_context<int>& actx) mutable -> cc::async_step_status {
    if (step++ == 0) { dep = cc::make_async_lazy([]{ return 10; }); actx.require(dep); return actx.wait_for_dependencies(); }
    return actx.resolve_to_value(*dep->value_ptr()); };   // or actx.success(...)
actx.require(dep);              // -> bool ready (NEITHER subscribes NOR schedules — the poll loop owns both);
                                //    else records a pending dep, return wait
actx.resolve_to_value(v)/success(v);  actx.resolve_to_value_emplace(args...);  // emplace: build T in place (immovable ok)
actx.resolve_to_error(E)/error(...);  actx.resolve_to_error_emplace(args...);  actx.wait_for_dependencies();  actx.yield();
// RESOLVE IS TERMINAL (`delete this;`): the frame lives inline in the node and the value is built from payload
// offset 0, so resolving destroys the running closure. Never touch captures or ctx after — `return actx.success(v);`.
// State DOES persist across polls (the frame is never moved). *_emplace args must not alias the frame's captures
// (nor a dep's value they pin) — they are forwarded by reference; the by-value resolves are always safe.
// A frame that THROWS is contained: poll fails the node on E (async_error carries what()), so it is never stuck
// `running` and never terminates a worker. Throwing AFTER resolving is the one unsupported case (asserts, dropped).
template <> struct cc::custom::async_error_from_exception_trait<my_err>  // custom E opts in; without it: assert+rethrow
{ static my_err make(cc::string_view message) { return my_err{cc::string(message)}; } };
// INLINE FRAME BUDGET: 24 B on a one-line node, i.e. sizeof(captures) + 8*(dep count) <= 24; anything over is
// heap-boxed (an alloc per node). Type-dependent — a big T/E widens it — and only 8-aligned, so ask, don't assume:
static_assert(cc::async<int>::frame_fits_inline<my_frame>);   // the real predicate; a copied number goes stale

// two schedulers, same surface. singlethreaded = drives inline + NEVER publishes, so deps cannot run
// concurrently however many cores idle (single-threaded by construction, not by circumstance):
cc::singlethreaded_scheduler sched;  int v = sched.blocking_get(root);  // mirrors pool.blocking_get(root)
cc::optional<...> o = sched.try_blocking_get(root);  // st: nullopt if pumped out but not ready (see async.md
                                                     // "Multi-scheduler correctness"); pool's try_ returns result
cc::async_worker_scope scope(sched);   // bind scheduler to this thread (blocking_get does this itself)
cc::async_no_worker_scope unbound;     // UNbind: run foreign code without its work landing in YOUR queue
root->schedule();  sched.run_until([&]{ return root->is_ready(); }); // the pump; interleave external push here
sched.drain();  sched.empty();      // pump till empty / is anything queued (a queued entry PINS its node alive)

// concurrent execution: work-stealing pool (#include <clean-core/thread/async_thread_pool.hh>)
cc::async_thread_pool pool;                              // >=1 workers; default = hardware concurrency - 1 (below)
cc::scoped_default_async_scheduler const ambient(pool);  // THE ambient scheduler: every async belongs to it
int v = cc::async_blocking_get(root);                    // caller PARTICIPATES (runs the graph, steals), then blocks
// ^ hence the -1 default: the driving thread is a worker for the duration. A graph that never forks stays on it
//   entirely — tens of ns, no cross-thread round trip (docs/systems/async.md "Driving").
//   An app installs one at startup; a nexus run installs one per phase (nx::no_scheduler opts out).
// route a graph to a SPECIFIC pool by submitting its root there (no per-node affinity system):
cc::async_thread_pool rpool(2);  int r = rpool.blocking_get(root2);   // or root2->schedule_on(rpool)
// WITHOUT THREADS (CC_HAS_THREADS == 0) the pool still exists with the same API — no #if at the call site.
// It starts nothing, worker_count() == 0, the ctor's count is ignored, and blocking_get drives the graph
// inline on the caller (it reports no steal-capable peers, so nothing is published). It cannot WAIT though:
// a graph parked on another thread's work never completes, and blocking_get's is_ready() assert says so.
// ambient context — "which logical task is this work part of?", from anywhere inside a frame
// (#include <clean-core/thread/async_ambient.hh>). cc propagates one opaque word and never inspects it.
CC_ASYNC_AMBIENT_TAG(my_tag)                          // define once per consumer; address-unique (ICF-safe)
cc::async_ambient_scope const s(my_tag(), &my_state); // push; RAII and strictly LIFO. The only way to install one
void* v = cc::async_ambient_lookup(my_tag());         // walk the calling thread's chain; null if absent
void* v2 = cc::async_ambient_lookup_in(head, my_tag());  // same, from a head you already hold (no TLS)
s.link();  s.outstanding();      // this scope's chain head / how much async work still carries it (racy; diagnostics)
cc::async_is_polling();          // inside a poll? i.e. would a throw from here be caught and become a node error,
                                 // or escape a worker thread and terminate — the question before reporting by throw
// DRIVE-SITE: a subtree driven from one work item is billed to that item's context, and an inline-driven dep
// inherits its driver's. A node stores it only as a resume token, written once when it is queued/parked/yielded —
// never by a thread merely WAKING it, so a shared dep completing under B cannot contaminate A's continuation.
// Links are refcounted, so work may outlive the scope that named it (prewarming is legal, not a leak); cc does
// not assert on that — read outstanding() and decide for yourself. A RESOLVED node carries no context at all.
// co_await / co_return (#include <clean-core/thread/async_coroutine.hh> — INCLUDING it is what enables coroutines)
// A coroutine IS a compute frame: the node stores one coroutine_handle (8 B, always inline). T must be MOVABLE.
// Take coroutine parameters BY VALUE — they are captured by declared type, so a reference dangles across a suspend.
cc::shared_async<int> load(cc::string p) { auto const& b = co_await read(p); co_return parse(b); } // COLD (= lazy)
cc::async_scheduled<int> spawn(cc::string p) { co_return 1; }  // eager at initial suspend; converts to shared_async
auto a = cc::async_start(load(p));    // "and go": idempotent; no-op if nothing could route (stays cold, drives later)
int const& v = co_await a;            // -> U const& into the node; a FAILED dep short-circuits (locals destroyed,
                                      // rest of the body skipped, error propagates) — no unwind, no catch runs
co_await cc::async_all(a, b, c);      // require ALL, park once; then `co_await a` no longer suspends. Also a span
                                      // overload. THE fan-out spelling: awaiting two COLD asyncs in sequence runs
                                      // them in sequence, since one await parks on one dependency
co_await cc::async_settled(a);        // wait WITHOUT short-circuiting, then read a->try_value()/try_error() (no copy)
cc::result<int> r = co_await cc::async_as_result(a);   // same, as a value (copies; needs a copyable U)
co_await cc::async_yield();           // -> async_step_status::yield; makes the node stealable and lets newer local
                                      // work go first. NOT a fairness knob (LIFO deque pops it back), NOT a way to
                                      // wait on something external — that is a manual node
co_await cc::async_fail("boom");      // never resumes; the uniform failure spelling
co_return cc::error("boom");          // non-unit T only (return_void and return_value cannot coexist)
// Not here yet: plain (non-async) dep args, structured/owned children, immovable T through co_return, error-type
// conversion across a heterogeneous-E graph (the make_async_* sugar assumes a single E; raw frames bridge by hand).
```

## Strings — encoding conversion

```cpp
#include <clean-core/string/conversion.hh>
cc::vector<char16_t> u16 = cc::utf8_to_utf16(sv); // BMP -> 1 unit, astral -> surrogate pair; bad -> U+FFFD
                                                  // NOT NUL-terminated (push_back(u'\0') if you need it)
cc::string u8 = cc::utf16_to_utf8(u16);           // pair -> astral code point; an unpaired surrogate -> U+FFFD
                                                  // takes a span, since a wide OS string is rarely NUL-terminated
```

## Platform

```cpp
#include <clean-core/platform/win32_sanitized.hh> // safe to include ANYWHERE; on Windows pulls in
                                                   // <Windows.h> with WIN32_LEAN_AND_MEAN + NOMINMAX,
                                                   // elsewhere expands to nothing. The sanctioned
                                                   // way to reach windows.h in shaped-core.
                                                   // GOTCHA: it also renames <rpcndr.h>'s global `byte`
                                                   // away, which would otherwise be AMBIGUOUS with cc::byte
                                                   // under a global `using namespace cc::primitive_defines;`.
                                                   // Any gate that pulls a COM SDK stack (<d3d12.h>,
                                                   // <dxcapi.h>, <wrl/*>) must repeat that bracket — those
                                                   // reach <rpcndr.h> past WIN32_LEAN_AND_MEAN.
#include <clean-core/platform/native.hh>
cc::demangle_symbol(symbol)                        // cc::string — human-readable C++ symbol name

#include <clean-core/platform/file_path.hh>       // where scratch files go, and how to unmake one
cc::temp_directory_path();                          // cc::string — no trailing separator; "." where none is named
cc::temp_file_path(prefix, suffix = "");            // "<temp>/<prefix>-<pid>-<counter><suffix>" — CREATES NOTHING
cc::remove_file(path);                              // true == gone, which includes never having been there
                                                    // NOT a filesystem layer: no mkdir, no iteration, no metadata,
                                                    // no path arithmetic. Reading and writing are the file streams'.

#include <clean-core/platform/environment.hh>     // read-only process environment
cc::environment_variable("HOME");                   // cc::optional<cc::string>; EMPTY value reads as absent
cc::is_environment_flag_set("SC_REQUEST_BACKGROUND");  // bool; set and not "0"/"false"/"no"/"off" (case-insensitive)
                                                    // No setter: it is process-global and racy against every other
                                                    // thread — pass the variable when spawning the child instead.

#include <clean-core/platform/stacktrace.hh>       // cc::stacktrace = std::stacktrace where available
cc::stacktrace::current();                          // CC_HAS_STACKTRACE guards rendering (empty stub on wasm)

#include <clean-core/error/crash_handler.hh>
cc::install_crash_handler();                        // segfault/abort/etc -> stderr: reason + hooks + stacktrace
                                                    // + EVERY other thread's stack (Windows; where a hang lives)
cc::add_crash_context_hook(&fn);                    // void()noexcept printed before the trace (keep it tiny)
```

### Terminal color

```cpp
#include <clean-core/platform/console.hh>
namespace ccc = cc::console;
ccc::configure(ccc::color_mode::automatic);         // ONCE, before the first byte of output (incl. usage errors)
                                                    // auto: NO_COLOR beats FORCE_COLOR, else stdout AND stderr
                                                    // must be TTYs; also enables ANSI on old Windows consoles
ccc::color_enabled()                                // bool — false until configure() runs

ccc::colorize(ccc::color::red, sv)                  // cc::string — unchanged when color is off (global flag)
ccc::colorize(ccc::color::red, sv, enabled)         // cc::string — the explicit-flag form, for pure renderers
ccc::red(sv)                                        // cc::string — one shorthand per color, = colorize(color::x, sv)

// ccc::color: bold, dim, italic, underline
//             black, red, green, yellow, blue, magenta, cyan, white
//             bright_black (the usual gray), bright_red, … bright_white
//             (SGR 1-4 / 30-37 / 90-97 — no 256-color or true-color, those need a capability database)
```

The flag is process-global: a test that flips it must put it back.
A renderer that must be testable takes its own `bool color` and uses `colorize`, rather than reading the global.

## Streams (byte I/O)

```cpp
#include <clean-core/streams/stream.hh>       // non-owning, MOVE-ONLY byte stream views over an adapter
cc::read_stream  cc::write_stream  cc::read_write_stream          // + cc::seekable_read_stream, seekable_write_stream, ...
// seekable_* promise FAST seeks (O(1)/O(log n)); a linear-seek source must present as non-seekable instead.
s.flush();                                // -> result<i64> plain flush: refill (read) / drain (write); pos, or -1 if none
s.is_valid();                             // false after a move consumed it
// read (read capability):
s.ready_bytes();                          // -> span<byte const> buffered & ready right now: [curr, end)
s.consume(n);                             // consume n ready bytes
s.at_end();                               // -> result<bool>; dry check on seekable, else refills once
s.read(dst);                              // -> result<isize> up to dst.size(); 0 only at end-of-data
s.read_exact(dst);  s.read_pod<T>();     // -> result<unit> / result<T>; error if the stream ends early
s.read_all();                             // -> result<vector<byte>> whole remaining stream; one precise alloc when the size is known
s.read_line(str);  s.read_line(str, max);// -> result<bool> one line into str (terminator stripped); optional max bytes, else unbounded
// write (write capability):
s.writable_bytes();                           // -> span<byte> free space to write into: [curr, end)
s.produce(n);                              // mark n bytes (written into writable_bytes()) as produced
s.write(src);  s.write_pod(v);            // -> result<unit>; a bounded sink that is full => error
// seekable only:
s.seek_to(abs);  s.skip(delta);  s.seek_from_end(off);   // -> result<i64> new position
s.position();  s.size();  s.remaining_bytes();  // -> result<i64>; dry queries — do NOT disturb the buffer
cc::move(s).try_as_seekable();            // -> optional<seekable_*>; consumes s on success, else s stays valid
cc::read_stream r = cc::move(seekable);   // narrow to any weaker stream; RVALUE ONLY, and it consumes the source

#include <clean-core/streams/span_stream.hh> // in-memory adapters (seekable, unbuffered — whole span is the window)
cc::span_read_stream_adapter(bytes);  cc::span_write_stream_adapter(buf);  cc::span_read_write_stream_adapter(buf);
// construct explicitly; converts IMPLICITLY to the matching stream (or a legal narrowing). Must outlive the stream.

#include <clean-core/streams/file_stream.hh> // buffered file adapters (seekable; 4 KiB inline buffer)
cc::file_read_stream_adapter::open(path);        // -> result<...>  existing file, read
cc::file_write_stream_adapter::create(path);     // -> result<...>  create / truncate, write from 0
cc::file_write_stream_adapter::open(path);       // -> result<...>  keep contents, overwrite in place from 0
cc::file_write_stream_adapter::append(path);     // -> result<...>  window starts at EOF; writing grows the file
cc::file_read_write_stream_adapter::open(path);  // -> result<...>  existing file, read + overwrite + grow-while-writing
adapter.stream();                         // -> the natural seekable_* stream (or implicit conversion, incl. non-seekable)
// Growth: create/open/append differentiate write intent; a read_write stream grows too (write past EOF,
// including a fresh seek-to-end + write) — it tracks the read boundary and write capacity as separate ends.

#include <clean-core/streams/stream_flush.hh> // authoring: write your own adapter (socket, compressor, ...)
cc::seek_dir  cc::stream_flush_fn             // the public flush contract; see docs/writing-a-stream.md
// seek_dir::remaining_size_hint is NOT a seek: bytes still to come, or -1. read_all() sizes its alloc from it,
//   and answering it never implies seekability — that is what try_as_seekable's dry_relative probe is for.
```

## Gotchas

- **Assertions are on in debug & relwithdebinfo, off in release.**
  The default presets are `relwithdebinfo-*`, so asserts are ON; if you touch assert-gated code, also build a `release-*` preset.
  `CC_ASSERT`'s message argument is mandatory.
- **`isize` is signed `i64`, not `size_t`** — intentional, to avoid unsigned underflow.
  `find`/`rfind` return **`-1`** (not a huge unsigned) on no-match.
- **`string` / `string_view` are NOT null-terminated.**
  `data()` is not a C string — use `str.c_str_materialize()`, whose result is valid only until the next non-const operation.
- **`string` SSO holds ≤ 39 bytes inline** (on 64-bit; fewer where pointers are smaller, e.g. wasm32) before it heap-allocates.
- **An `interned_string`'s identity is process-local and must never leave the process.**
  Serialize `as_string_view()`, and hash durable data over those bytes — two runs will not agree on anything else.
- **`interned_string` has no `<`, on purpose.** Everything about the type is a pointer operation except ordering by bytes, so making that cost visible beats a `<` that quietly memcmps.
  `compare_bytes` is the reproducible one and the default choice; `compare_identity` is cheaper and its order changes every run, so never persist or display anything sorted by it.
- **String ordering is by UNSIGNED byte value**, in `string_view::compare`, the `<` family built on it, and `interned_string::compare_bytes` alike.
  `char` is signed here, so the naive comparison would sort every byte >= 0x80 ahead of all ASCII — an order that silently disagrees with every other implementation once it reaches a file or a hash.
- **`hash256` orders by limb, not by its 32 canonical bytes.**
  The order is total and deterministic, so it is fine as a tiebreak — it just is not the order the hex digests sort in.
  Use `to_bytes` / `from_bytes` whenever the 32 bytes themselves are what matters.
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
- **`cc::tuple` and `cc::pair` default-construct their elements VALUE-initialized**, unlike `cc::vector`'s uninitialized resizes.
- **`cc::variant` has no valueless state**, not even after `take<T>()` — that leaves a moved-from alternative behind, at the same index.
  Assignment destroys the active alternative before constructing the new one, so an alternative whose move constructor throws is not supported.
  Duplicate alternatives are a static_assert, since access is keyed on type.
- **A ringbuffer is the one owning container that is NOT contiguous** — no `data()`, no pointer iterator, and its iterator is a real (random-access) type.
  Its heap capacity is always a power of two, so `create_with_capacity(100)` gives you 128.
- **A bit set is NOT a container of addressable bools** — the mutable `operator[]` yields a `cc::bit_ref` proxy, so `auto b = bs[i]` keeps tracking the bitset and `bool b = bs[i]` is the snapshot.
  There is no `begin()`/`end()` over bools; `set_indices()` is the iteration, and it costs one step per set bit.
- **Every two-bitset operation asserts an equal `size()`**, and there are no `|` / `&` / `^` / `~` operators — that precondition is too implicit for an operator.
  `unset_all()` zeroes the bits, `clear()` (bitset only) makes `size()` zero, and `empty()` deliberately does not exist.
- **`disjoint_set` has no `size()`** — it would not say whether it counts elements or sets, so the two counts are `element_count()` and `partition_count()`.
  `get_representative` and everything reaching it are **non-const**: a find rewires the links it walked.
  Its `merge_by_representative` and `size_of_set_by_representative` assert that their arguments really are representatives.
- **`CC_FLAG_ENUM_INDEXED` / `CC_FLAG_ENUM_BITMASK` go at GLOBAL scope and take the namespace as their first argument** — they open that namespace themselves.
  The enum must therefore live in one, and there is no `operator~`: `without()` is the set subtraction every complement was being used for.
- **Pick the encoding deliberately — nothing detects it for you.**
  `INDEXED` on an enum whose values are already bit patterns, or `BITMASK` on a plain one, shifts every flag silently; only a value that overruns the storage asserts.
- **Streams are move-only real types (private-inheritance wrappers over one engine).**
  Conversions only ever NARROW (`seekable_* -> plain`, `read_write -> read`/`write`; `read <-> write` never).
  An adapter converts to any legal narrowing; a stream narrows to another stream only **from an rvalue**, consuming it, so one backend never has two live views.
  A consumed or moved-from stream asserts on use, and narrowing away write capability with unflushed bytes pending asserts too.
- **A sort comparator MUST be a strict weak ordering.**
  The partition scans are deliberately unbounded, so one that is not walks off the range — `CC_ASSERT` catches it in debug and relwithdebinfo, and release has nothing to catch it with.
  `<=` where `<` was meant, and floats that can be NaN, are the two usual causes.
- **No sort here is stable by default** — the plain `cc::sort` / `sort_by` leave equal elements in an unspecified order.
  `cc::sort_stable` / `cc::sort_stable_by` are the stable spellings, and they permute an index array rather than buffering elements, so the swap-only design survives.
  `cc::sort_indices` over `0..n-1` is stable for the same reason: it breaks ties on the index.
- **`cc::sort_by` evaluates its key on every comparison**, i.e. O(n log n) times.
  `cc::sort_by_cached_key` evaluates it exactly n times into a temporary buffer, and is worth it as soon as the key costs more than a member read.
- **`cc::vector` is not an `index_swap_range`** — deliberately, so nothing models the seam by accident.
  `cc::as_index_swap_range(v)` is the adapter, and a hand-written one must be trivially copyable because it is copied down the recursion.
- **Flush a write stream before dropping its adapter** — buffered bytes are lost otherwise, since there is no auto-flush.
  A stream borrows into its adapter, so the adapter must outlive it.
  The file adapter's 4 KiB buffer is *inline*, so once a stream is taken the adapter is effectively pinned; do not move it.
