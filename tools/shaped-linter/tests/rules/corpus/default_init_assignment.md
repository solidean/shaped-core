# default-init-assignment

Every variable initializer uses assignment form `T v = value;`, never brace form `T v{value};` —
data members, function locals and namespace-scope variables alike.

The fix is not a blind brace-strip: a braced list that *stays* a list keeps its braces behind the `=`.
Blocks below are annotated with `[default-init-assignment]` for "fires once here",
`~[default-init-assignment]` for "must stay quiet", and `fix="…"` for the exact replacement text.

## The fix shapes

A single plain value drops its braces.

```cpp [default-init-assignment] fix=" = 0"
struct S { cc::atomic<cc::u32> x{0}; };
```

```cpp [default-init-assignment] fix=" = nullptr"
struct S { cc::atomic<ring*> _ring{nullptr}; };
```

```cpp [default-init-assignment] fix=" = false"
struct S { cc::atomic<bool> _scan_pending{false}; };
```

Empty braces stay empty braces — `= {}` is the value-initializing form, `= ` alone is not valid.

```cpp [default-init-assignment] fix=" = {}"
struct S { int value{}; };
```

A top-level comma means the braces are a list, so they survive the rewrite.

```cpp [default-init-assignment] fix=" = {a, b}"
struct S { P p{a, b}; };
```

A designated initializer is a list too, whatever its comma count.

```cpp [default-init-assignment] fix=" = {.a = 1}"
struct S { P p{.a = 1}; };
```

A comma *inside* a call is not a top-level comma — this is one value, so the braces go.

```cpp [default-init-assignment] fix=" = f(a, b)"
struct S { int n{f(a, b)}; };
```

Whitespace before the brace is absorbed by the rewrite rather than left dangling.

```cpp [default-init-assignment] fix=" = 0"
struct S { int x {0}; };
```

## Every scope, not just record bodies

A function local.

```cpp [default-init-assignment] fix=" = 0"
void f() { int y{0}; }
```

A static local inside a member function.

```cpp [default-init-assignment] fix=" = 1"
struct S { void f() { static cc::atomic<int> s{1}; } };
```

A namespace-scope variable.

```cpp [default-init-assignment] fix=" = 0"
namespace n { cc::atomic<int> g{0}; }
```

A file-scope variable, with no namespace around it.

```cpp [default-init-assignment] fix=" = 7"
cc::atomic<int> g{7};
```

A local nested inside an `if` body — the parser descends into blocks, not just function bodies.

```cpp [default-init-assignment] fix=" = 2"
void f() { if (c) { int y{2}; } }
```

A local inside a loop body.

```cpp [default-init-assignment] fix=" = 3"
void f() { for (auto const& x : v) { int y{3}; } }
```

A local inside a lambda that is being assigned to a variable.

```cpp [default-init-assignment] fix=" = 4"
void f() { auto g = [] { int y{4}; }; }
```

A lambda with a parameter list and a `mutable` specifier between it and the body.

```cpp [default-init-assignment] fix=" = 5"
void f() { auto g = [](int a) mutable { int y{5}; }; }
```

A lambda passed straight as a call argument.

```cpp [default-init-assignment] fix=" = 6"
void f() { run([] { int y{6}; }); }
```

## Several findings in one block

Each expected finding gets its own annotation, so the count is explicit.

```cpp [default-init-assignment] [default-init-assignment]
struct S { cc::atomic<int> a{0}; cc::atomic<int> b{1}; };
```

A member and a local in the same snippet.

```cpp [default-init-assignment] [default-init-assignment]
struct S { int a{1}; void f() { int b{2}; } };
```

## Look-alikes that must stay quiet

Already in assignment form.

```cpp ~[default-init-assignment]
struct S { int x = 0; };
```

A constructor's mem-initializer list is not a declaration.

```cpp ~[default-init-assignment]
struct S { S() : _x{0} {} int _x; };
```

Nor is it when it initializes several members — only the trailing brace group is the body.

```cpp ~[default-init-assignment]
struct S { S() : _a{0}, _b{1} {} int _a; int _b; };
```

An aggregate at a call site is an argument, not a variable.

```cpp ~[default-init-assignment]
void f() { g({1, 2}); }
```

A braced return value is an expression.

```cpp ~[default-init-assignment]
P f() { return P{1, 2}; }
```

So is a braced temporary in statement position.

```cpp ~[default-init-assignment]
void f() { T{1}; }
```

An enum body is not a record body, and its enumerators are not variables.

```cpp ~[default-init-assignment]
enum class E { A = 1, B = 2 };
```

A subscript carries a `]` that must not read as a lambda introducer.

```cpp ~[default-init-assignment]
void f() { auto v = a[i]; }
```

An array initializer is already assignment form and keeps its own shape.

```cpp ~[default-init-assignment]
int a[] = {1, 2};
```

## Known corner-cuts

These are pinned as they behave today, not as they ideally would — the boundary is documented so it
cannot regress silently. See ../../../docs/architecture.md.

A multi-declarator brace init records only the first declarator — `b{2}` goes unreported.

```cpp [default-init-assignment] fix=" = 1"
struct S { int a{1}, b{2}; };
```

An array data member *is* handled, and the rewrite happens to be right: the declarator-id is still the
last top-level identifier (`a`), because `[N]` is skipped as a balanced group.

```cpp [default-init-assignment] fix=" = {1, 2}"
struct S { T a[N]{1, 2}; };
```
