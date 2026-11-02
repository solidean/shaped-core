# Generics

*Incubator: not normative.*

Most of this is specified now ([CHK-338 to CHK-341](../semantics/checking.md#generics)): bodies checked per instance, stated arguments, value parameters and generic structs.
What is left is listed under **Open**, and the file goes once that is settled.
This file is the design that generalizes the parameters and the deduction.

## The idea

Every call inlines ([function-model.md](function-model.md)), so a generic is a function whose body is checked again for each argument list, and nothing more.
There are no type classes, no traits and no bounds.
A shader holds no data structure complex enough to repay them.

### A generic parameter is an ordinary parameter

* **`[n: int]` declares a generic parameter of any type, and a bare `T` is short for `T: type`.**
  `type` is a type like any other ([types-as-values.md](types-as-values.md)), so `T` and `n: int` are one feature.
* **A `[]` parameter is a `()` parameter with two differences**: its argument must be constant, and a caller may omit it and have it deduced.
  Compile-time functions and `[]` parameters are one mechanism.
* **Any constant is an argument**: a type, an integer, an enum case, a float, a struct value.
* **Parameters are in scope left to right**, so `[T, n: T]` is a parameter of the type the one before it names.
* **A generic parameter is a constant in the body, and may stand in a type position**: `float[n]`, `buffer[T]`.
  An ordinary parameter may as well, since a type position only has to reduce to a type after inlining.
  A call that hands such a parameter a value that is not constant is an error at that call.

```sgl sketch
fun first[n: int, T](values: T[n]) -> T => values[0]

fun scaled(T: type, x: float) -> T => T(x * 2.0)   // a type through (), never deduced
```

### A call states some arguments and deduces the rest

* **Generic arguments are written as call arguments are**: `foo[1, int, .bla = bool](…)` gives the first two by position and `bla` by name.
  Every parameter not given is deduced.
* **The overload set is pruned by the shape of the generic arguments before anything is deduced.**
  A candidate leaves when it has fewer parameters than positional arguments, or when a name matches none of its parameters.
  It leaves too when an argument cannot be of its parameter's type, such as `int` where an `int` value is asked.
* **Each argument of the call contributes on its own, and the contributions are merged.**
  They must agree wherever two bind the same parameter, and together they must bind every parameter not given.
  The order of the arguments does not matter.
  * An argument whose parameter's type names generic parameters binds them to the argument's type exactly.
  * A literal contributes the set of types it fits and a default type.
    Merging intersects the sets, and the default applies only to a parameter nothing else binds, so `max(1, x)` with `x: float` deduces `float`.
  * A lambda contributes once the parameters it takes are bound, and its result binds its return type.
  * The type expected where the call stands contributes last, to parameters still unbound (CHK-82).
* **Contributions that disagree leave the candidate unmatched**, and a parameter nothing binds is an error whose detail names it.
* **`foo[1]` is generic arguments after a function's name, and a subscript after a value**: name resolution tells them apart, and the form tree parses both alike.
  A call reads naturally, `foo[1](x)`.

### A body is checked per instantiation

* **Each distinct argument list checks the body anew**, and an instance is interned under its arguments.
* **A name in a generic body resolves where the generic is written.**
  Only types and overload selection wait for the arguments, so what is visible at a call never changes what a name means.
* **An error is reported at its line in the body, with the instantiations that reached it**: "in `foo[float]`, called at a.sgl:12".
  The same error from several instantiations is reported once.
* **A generic that nothing instantiates is checked for its names alone, and a warning says so.**
  A `test` that calls it with chosen arguments is how a generic is checked with X, Y and Z; it needs no annotation of its own.
  The language server checks a generic body through the instantiations its tests make.

### Generic structs

* **`struct name[…]:` takes the same parameters a function does**, and they are in scope in every member and every method.
* **The constructor mirrors the parameters**: `pair(1.0, 2.0)` deduces `A` from the fields it fills.
  A parameter that no field names cannot be deduced, so it must be given.
* **In the front end an instance is nominal**: `foo[x]` and `foo[y]` are one type exactly when `x` and `y` are equal.
* **Arguments are equal when their values are bitwise equal, with every NaN canonical.**
  `0.0` and `-0.0` are two arguments, since `1.0 / x` tells them apart; every NaN is one value, equal to itself.
* **After the front end, types are structural.**
  Two instances with the same fields lower to one struct, so `foo[x]` and `foo[y]` whose argument reaches only methods emit as one type.
  Whether two types were one is not observable after lowering.

```sgl sketch
struct ring[n: int, T]:
    items: T[n]
    head: int

    fun capacity(self) -> int => n

let r = ring[4, float](items = (1.0, 2.0, 3.0, 4.0), head = 0)
let s = ring(items = (1, 2), head = 0)   // n = 2, T = int, deduced from the fields
```

## What it touches

* The check pass:
  * CHK-340's deduction becomes the per-argument merge.
  * CHK-78's refusal of stated type arguments goes.
  * CHK-339 opens to the program, to several parameters and to value parameters.
* Name resolution: a generic body's names resolve at its declaration, its types and overloads at each instantiation.
* The AST: a named argument (AST-24) stands in a square list as it does in a paren group.
* Diagnostics: the chain of instantiations behind an error, one report for the same error from several instances, and the warning on a generic nothing instantiates.
* Interning: an instance's key holds value arguments, compared bitwise with NaN canonical.
* Lowering and emit: struct instances with the same fields become one struct.
* The [compilation model](compilation-model.md): a module's generic is checked in the layer that instantiates it, which is what it assumes already.
* `slib`'s host code: a struct instance in GPU memory is one C++ struct, named as the target text names it, `pair_float_int`.
* The language server: checking a generic body through its tests.

## Already fixed by the syntax

* A generic list is a fused square list, the same form as a subscript, so `buffer[float]` and `weights[3]` parse alike.
* `struct name[A]:` reads its parameters as fields, as a function's are (AST-152), so `[n: int]` parses already.
* `[]` marks what a caller may omit and have deduced, and `()` what it may not.

## Open

* Whether a value parameter is deduced from anything but an array's length or an image's format.
* Whether a bare template where a value's type stands, `let b: box`, means its all-defaults instance; today it is the template.
* Whether instances of two templates with the same fields lower to one struct too; one template's alike instances do (EMIT-179).
* The set of types a literal fits, which [literal-types.md](literal-types.md) decides.
* Whether a library module's generic that nothing instantiates warns too; today only the program's do.
