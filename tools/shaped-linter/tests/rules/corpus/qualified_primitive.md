# qualified-primitive

The sized aliases of `cc::primitive_defines` — `u32`, `isize`, `byte`, … — are vocabulary, and are always
spelled bare. Every namespace-qualified spelling of one is a finding, wherever it appears.

Each `cpp` block below is linted with the full rule set and the finding count must match its annotations
exactly. `[qualified-primitive]` means "one finding here", `~[qualified-primitive]` means "must stay
quiet", and `fix="…"` pins a replacement text — for this rule always the empty string, since its fix
*deletes* the qualifier. What it deletes is a byte range, which the corpus cannot express; the smoke test
pins that.

## Every alias is covered

All thirteen names `cc::primitive_defines` declares, one block per family so a failure names the family.

```cpp [qualified-primitive] [qualified-primitive] [qualified-primitive] [qualified-primitive] fix=""
namespace cc { void f(cc::i8 a, cc::i16 b, cc::i32 c, cc::i64 d); }
```

```cpp [qualified-primitive] [qualified-primitive] [qualified-primitive] [qualified-primitive] fix=""
namespace cc { void f(cc::u8 a, cc::u16 b, cc::u32 c, cc::u64 d); }
```

```cpp [qualified-primitive] [qualified-primitive] fix=""
namespace cc { void f(cc::f32 a, cc::f64 b); }
```

```cpp [qualified-primitive] [qualified-primitive] [qualified-primitive] fix=""
namespace cc { void f(cc::byte a, cc::isize b, cc::nullptr_t c); }
```

## Every position a type can sit in

The rule is a spelling check, so a parameter, a return type, a template argument, an alias' right-hand
side and a functional cast are all the same case.

```cpp [qualified-primitive]
namespace cc { cc::isize size_of(thing const& t); }
```

```cpp [qualified-primitive] [qualified-primitive]
namespace sg { void f(sg::buffer<cc::u32> b, cc::isize offset); }
```

```cpp [qualified-primitive]
namespace cc { using my_index = cc::isize; }
```

```cpp [qualified-primitive]
namespace cc { void f(thing const& t) { g(cc::u32(t.count)); } }
```

```cpp [qualified-primitive]
namespace cc { struct S { void f(); cc::u64 hash() const; }; }
```

## A leading `::` is the same name, rooted at global scope

The rewrite takes the `::` with it, which is why the finding's span starts there.

```cpp [qualified-primitive] fix=""
namespace sg { void f(::cc::u64 x); }
```

## Where the bare name is reachable

Only a reachable bare name gets a fix. Inside a namespace that re-exports the aliases through its own
`fwd.hh`, or under a using-directive this file actually shows, it is reachable — so these pin a fix.

```cpp [qualified-primitive] fix=""
namespace babel { void f(cc::isize n); }
```

```cpp [qualified-primitive] fix=""
namespace cc::impl { void f(cc::byte b); }
```

```cpp [qualified-primitive] fix=""
using namespace cc::primitive_defines;
void f(cc::u32 x);
```

A namespace the linter has never heard of counts too, as long as *this file* shows it nominating the
aliases — which is how a new library is covered inside its own `fwd.hh` without an edit to the rule.

```cpp [qualified-primitive] fix=""
namespace zzz { using namespace cc::primitive_defines; }
namespace zzz { void f(zzz::u32 x); }
```

Only the namespace the directive sits *directly* in gains the names. A directive inside `outer::inner`
does nothing for `outer`, so `outer::u32` names nothing and stays quiet — while `inner::u32` is a finding.

```cpp [qualified-primitive]
namespace outer { namespace inner { using namespace cc::primitive_defines; } }
void f(outer::u32 a);
void g(inner::u32 b);
```

## Where it is not reachable, the finding carries no fix

A directive's reach ends with its scope, so the second call below is outside it. Both are findings; only
the first can be rewritten, and pinning `fix=""` as a *set* covers exactly that — one rewrite over two
findings.

```cpp [qualified-primitive] [qualified-primitive] fix=""
void f() { using namespace cc::primitive_defines; cc::u32 a = g(); }
void h() { cc::u32 b = g(); }
```

## Negatives — the look-alikes that must stay quiet

The bare spelling is the whole point of the rule.

```cpp ~[qualified-primitive]
namespace cc { void f(u32 a, isize b, byte c, nullptr_t d); }
```

A longer identifier that merely starts like an alias is a different name. The scan matches whole tokens,
which is what keeps `cc::byte_stream_builder` out of a rule about `cc::byte`.

```cpp ~[qualified-primitive]
namespace cc { void f(cc::byte_stream_builder& b, cc::u32string const& s, cc::i64_traits const& t); }
```

A clean-core *type* keeps its `cc::` — the convention is about the primitives, not about `cc::span` and
friends.

```cpp ~[qualified-primitive]
namespace cc { void f(cc::string const& s, cc::span<int> v, cc::vector<int>& w); }
```

`std::byte` is somebody else's alias, and a namespace nobody re-exports through names something unrelated.

```cpp ~[qualified-primitive]
void f(std::byte a, foo::u32 b, my_lib::isize c);
```

A deeper qualification names a different entity entirely: the `cc` in `a::cc` is not clean-core.

```cpp ~[qualified-primitive]
namespace cc { void f(a::cc::u32 x, S::cc::byte y); }
```

Used as a scope of its own, the name is not the alias.

```cpp ~[qualified-primitive]
namespace cc { void f(cc::byte::inner x); }
```

Member access is not qualification.

```cpp ~[qualified-primitive]
namespace cc { void f(thing& a, thing* b) { a.cc::u32; b->cc::isize; } }
```

A comment, a string literal and a preprocessor directive each lex as **one** token of their own kind, so
a qualified spelling inside any of them never reaches the scan. That is the boundary, and it also means
the rule cannot see into a macro body.

```cpp ~[qualified-primitive]
// cc::u32 in a line comment
/* cc::isize in a block comment */
#define COUNT_TYPE cc::u32
char const* s = "cc::byte";
```

The declaration site spells them bare, so `fwd.hh` itself is quiet.

```cpp ~[qualified-primitive]
namespace cc::primitive_defines
{
using i64 = int64_t;
using isize = i64;
}
```
