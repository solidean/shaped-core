# co-await-in-initializer

A `co_await` with an earlier element ahead of it in a brace initializer, which crashes MSVC.
The fix is always a named local, so a case MSVC happens to compile is reported too.

## a later element is reported

```cpp [co-await-in-initializer]
task f() { auto p = pair{seed(), co_await make()}; }
```

## a designator is an element

```cpp [co-await-in-initializer]
task f() { auto p = routes{.text = probe, .native = co_await make()}; }
```

## assignment form is the same initializer

```cpp [co-await-in-initializer]
task f() { pair p = {seed(), co_await make()}; }
```

## an array is the same

```cpp [co-await-in-initializer]
task f() { heavy p[2] = {seed(), co_await make()}; }
```

## an array's extent is no lambda's capture

```cpp [co-await-in-initializer] [default-init-assignment]
task f() { heavy p[2]{seed(), co_await make()}; }
```

## a call inside the element is still inside it

```cpp [co-await-in-initializer]
task f() { auto p = pair{seed(), wrap(co_await make())}; }
```

## a brace initializer passed to a call

```cpp [co-await-in-initializer]
task f() { co_await use(*ctx, {.name = "quad", .mesh = co_await make()}); }
```

## an initializer inside an initializer has the outer's elements ahead of it

```cpp [co-await-in-initializer]
task f() { auto p = outer{seed(), inner{co_await make()}}; }
```

## a macro's body is read

```cpp [co-await-in-initializer]
#define ROUTES(entry) route_pair{.text = a(entry), .native = co_await b(entry)}
```

## a type with no destructor crashes nothing, and tokens cannot see it

```cpp [co-await-in-initializer]
task f() { auto p = int_first{1, co_await make()}; }
```

## the first element has nothing ahead of it

```cpp ~[co-await-in-initializer]
task f() { auto p = pair{co_await make(), seed()}; }
```

## the first element may hold two awaits of one choice

```cpp ~[co-await-in-initializer]
task f() { auto p = pair{c ? co_await a() : co_await b()}; }
```

## a local awaited first is the fix

```cpp ~[co-await-in-initializer]
task f() { auto const made = co_await make(); auto p = pair{seed(), made}; }
```

## a function's arguments are not an initializer

```cpp ~[co-await-in-initializer]
task f() { take(seed(), co_await make()); }
```

## a statement in a block is not an element

```cpp ~[co-await-in-initializer]
task f() { auto a = seed(), b = seed(); co_await make(); }
```

## a lambda's body is a function of its own

```cpp ~[co-await-in-initializer]
task f() { auto p = pair{seed(), [&]() -> task { co_await make(); }}; }
```

## a lambda with no parameters

```cpp ~[co-await-in-initializer]
task f() { auto p = pair{seed(), [&] { co_await make(); }}; }
```

## a trailing return type opens a body

```cpp ~[co-await-in-initializer]
auto f(int a, int b) -> cc::shared_async<pair<int, int>> { co_await make(); }
```

## a control statement's body

```cpp ~[co-await-in-initializer]
task f() { if (x) { co_await make(); } else { co_await make(); } }
```

## a mention in a comment or a string

```cpp ~[co-await-in-initializer]
task f() { auto p = pair{seed(), "co_await make()"}; /* pair{a, co_await b} */ }
```

## declaring the operator awaits nothing

```cpp ~[co-await-in-initializer]
namespace cc { template <class U, class E> awaiter<U, E> operator co_await(async<U, E> const& dep); }
```

## a reproduction has to spell the bug

```cpp ~[co-await-in-initializer] path="docs/bugs-external/some-msvc-crash/repro.cc"
task f() { auto p = pair{seed(), co_await make()}; }
```

## other directives stay opaque

```cpp ~[co-await-in-initializer]
#error pair{a, co_await b}
```
