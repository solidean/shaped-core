# default-init-assignment

Every variable initializer uses assignment form `T v = value;`, never brace form `T v{value};` —
data members, function locals and namespace-scope variables alike.

The rewrite moves the `=` in and leaves the braces alone: `T v{x}` becomes `T v = {x}`, never `T v = x`.
Dropping the braces reads better but changes direct-list-init into copy-init, and a linter with no type
information cannot tell when that is legal — an aggregate has no converting constructor to fall back on,
and neither does a type whose constructor is `explicit`. `= {x}` is copy-*list*-init, which every
aggregate and every implicit constructor accepts.

The nicer forms — a member's plain `= value` and a local's `auto v = T(value)` — ride along as a **hint**,
which `--fix` never applies. Both can fail to compile or change which constructor runs, so a human signs
off on them; see the block comment in [default_init_assignment.cc](../../../src/shaped-linter/rules/default_init_assignment.cc)
for exactly which hazard belongs to which form.

Blocks below are annotated with `[default-init-assignment]` for "fires once here",
`~[default-init-assignment]` for "must stay quiet", and `fix="…"` for a replacement text the preceding
rule produces — chained once per distinct rewrite, since a rule's fixes are pinned as a set.
`hint="…"` pins the hint channel the same way.

## The fix shapes

The initializer is carried over verbatim, whatever is in it.

```cpp [default-init-assignment] fix=" = {0}" hint=" = 0"
struct S { cc::atomic<cc::u32> x{0}; };
```

```cpp [default-init-assignment] fix=" = {nullptr}" hint=" = nullptr"
struct S { cc::atomic<ring*> _ring{nullptr}; };
```

```cpp [default-init-assignment] fix=" = {false}" hint=" = false"
struct S { cc::atomic<bool> _scan_pending{false}; };
```

Empty braces stay empty braces — `= {}` is the value-initializing form, `= ` alone is not valid.

```cpp [default-init-assignment] fix=" = {}"
struct S { int value{}; };
```

A list stays a list.

```cpp [default-init-assignment] fix=" = {a, b}"
struct S { P p{a, b}; };
```

A designated initializer likewise.

```cpp [default-init-assignment] fix=" = {.a = 1}"
struct S { P p{.a = 1}; };
```

A comma inside a call is just part of the initializer text.

```cpp [default-init-assignment] fix=" = {f(a, b)}" hint=" = f(a, b)"
struct S { int n{f(a, b)}; };
```

Whitespace before the brace is absorbed by the rewrite rather than left dangling.

```cpp [default-init-assignment] fix=" = {0}" hint=" = 0"
struct S { int x {0}; };
```

An array bound belongs to the declarator, so the rewrite starts after it and `[N]` survives.
The replacement text alone cannot show that — [engine-test.cc](../../rules/engine-test.cc) pins the applied result.

```cpp [default-init-assignment] fix=" = {1, 2}"
struct S { T a[N]{1, 2}; };
```

## Every scope, not just record bodies

A function local.

```cpp [default-init-assignment] fix=" = {0}"
void f() { int y{0}; }
```

A static local inside a member function.

```cpp [default-init-assignment] fix=" = {1}"
struct S { void f() { static cc::atomic<int> s{1}; } };
```

A namespace-scope variable.

```cpp [default-init-assignment] fix=" = {0}"
namespace n { cc::atomic<int> g{0}; }
```

A file-scope variable, with no namespace around it.

```cpp [default-init-assignment] fix=" = {7}"
cc::atomic<int> g{7};
```

An out-of-line static member definition. Its declarator-id carries a `::`, but the leading `int` is the
type, so this stays a declaration — the mirror image of the qualified temporaries further down.

```cpp [default-init-assignment] fix=" = {8}"
int S::x{8};
```

A local nested inside an `if` body — the parser descends into blocks, not just function bodies.

```cpp [default-init-assignment] fix=" = {2}"
void f() { if (c) { int y{2}; } }
```

A local inside a loop body.

```cpp [default-init-assignment] fix=" = {3}"
void f() { for (auto const& x : v) { int y{3}; } }
```

A local inside a lambda that is being assigned to a variable.

```cpp [default-init-assignment] fix=" = {4}"
void f() { auto g = [] { int y{4}; }; }
```

A lambda with a parameter list and a `mutable` specifier between it and the body.

```cpp [default-init-assignment] fix=" = {5}"
void f() { auto g = [](int a) mutable { int y{5}; }; }
```

A lambda passed straight as a call argument.

```cpp [default-init-assignment] fix=" = {6}"
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

A *qualified* temporary just as much. `cc::T` is one name, not a type plus a declarator, so there is no
variable here to rewrite — counting the tokens before the brace would have found three and been wrong.

```cpp ~[default-init-assignment]
void f() { cc::T{1}; }
```

And a temporary that is immediately called — the shape that made `cc::void_function{}()` come out as the
uncompilable `cc::void_function = {}()`.

```cpp ~[default-init-assignment]
void f() { cc::void_function{}(); }
```

Assigning through such a call does not make it a declaration either.

```cpp ~[default-init-assignment]
void f() { cc::identify_function{}(x) = 20; }
```

A temporary whose type carries template arguments, qualified or not.

```cpp ~[default-init-assignment]
void f() { cc::vector<int>{1, 2}; }
```

```cpp ~[default-init-assignment]
void f() { vector<int>{1, 2}; }
```

A leading `::` roots the name at global scope and still leaves no type ahead of it.

```cpp ~[default-init-assignment]
void f() { ::cc::T{1}; }
```

A temporary as the right-hand side of a compound assignment. Here there *are* tokens ahead of the qualified
name — `s +=` — so a type-is-in-front test passes and only the operator gives it away.
This is what turned `s += cc::string_view{" world"}` into the uncompilable `s += cc::string_view = {" world"}`.

```cpp ~[default-init-assignment]
void f() { s += cc::string_view{" world"}; }
```

Any other operator likewise.

```cpp ~[default-init-assignment]
void f() { total = total + P{1, 2}; }
```

```cpp ~[default-init-assignment]
void f() { obj.field->reset(T{1}); }
```

A ternary whose branches are both temporaries.

```cpp ~[default-init-assignment]
void f() { g(c ? P{1} : P{2}); }
```

A comparison. A real declaration reaches its `>` only inside a template-argument skip, so a `>` left at
top level means the segment started mid-expression.

```cpp ~[default-init-assignment]
void f() { a > T{1}; }
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

## Several declarators in one statement

Every brace-initialized declarator is its own finding with its own rewrite.

```cpp [default-init-assignment] [default-init-assignment] fix=" = {1}" fix=" = {2}" hint=" = 1" hint=" = 2"
struct S { int a{1}, b{2}; };
```

Each keeps whatever suffix it carries.

```cpp [default-init-assignment] [default-init-assignment] fix=" = {1}" fix=" = {3}" hint=" = 1" hint=" = 3"
struct S { int a{1}, b[2]{3}; };
```

Only the brace-initialized ones are reported: `b` is already assignment form and `c` has no initializer.

```cpp [default-init-assignment] fix=" = {1}" hint=" = 1"
struct S { int a{1}, b = 2, c; };
```

A comma inside an initializer does not start a declarator.

```cpp [default-init-assignment] [default-init-assignment] fix=" = {f(1, 2)}" fix=" = {3}" hint=" = f(1, 2)" hint=" = 3"
struct S { P a{f(1, 2)}, b{3}; };
```
