# Checking

*Tracer: deliberately thin.*

The check pass reads the ASTs of one module and resolves names, checks types and builds the flat tree of every entry point.
It is one pass, not three, and it carries what three samples need.
They are [cube.sgl](../../../tests/samples/cube.sgl), [helpers.sgl](../../../tests/samples/helpers.sgl) and [matrices.sgl](../../../tests/samples/matrices.sgl).
Back to the [semantics](_index.md); the reasons are in [why/checking.md](why/checking.md).

## The pass

* **CHK-1** The check pass reads the [ASTs](../syntax/ast.md) of the files of one module, and it changes none of them.
* **CHK-2** The module is unnamed: the files of the prelude stand in front, then the program's file, and a file is named by its position in that order ([why](why/checking.md#chk-2)).
* **CHK-3** A `module` line is accepted and names nothing.
* **CHK-4** The files stay separate, so a span points into the source of its own file, and a diagnostic names its file.
* **CHK-5** The check pass is total: any ASTs give a checked module and a list of diagnostics, `invalid` nodes and earlier diagnostics included.
* **CHK-6** What did not check has the **error type**.
* **CHK-7** The error type never causes a second diagnostic: whatever meets it is silent ([why](why/checking.md#chk-7)).
* **CHK-8** A construct of the language that the pass does not carry is the normal error `unsupported-yet`, and its detail names the construct ([why](why/checking.md#chk-8)).
* **CHK-9** The pass gives a construct it does not carry no meaning: its type is the error type.

## Symbols

* **CHK-10** A **symbol** is a `fun`, a `struct`, an `enum` or a `binding` declared at the top level of a file, named by its file and its declaration.
* **CHK-11** The module scope is unordered: a symbol may be used above its declaration.
* **CHK-12** A name is declared once in the module scope, unless every declaration of it is a function; a later declaration is the normal error `duplicate-declaration`.
* **CHK-13** Several functions of one name are an **overload set**.
* **CHK-14** A `const`, a `type` alias and a file-scope `sampler` are `unsupported-yet`, and each still owns its name, so a use of it is silent.
  An `enum` is a symbol of its own, by CHK-142.
* **CHK-15** `use` and `notation` are `unsupported-yet`.
* **CHK-16** A symbol is in one of four states: untouched, in compilation, checked, or failed.
* **CHK-17** Compilation is on demand: needing a symbol that is untouched compiles it first ([why](why/checking.md#chk-17)).
* **CHK-18** Needing a symbol that is in compilation is the normal error `dependency-cycle`, its detail names the loop, and what needed it gets the error type.
* **CHK-19** A failed symbol gives whatever needs it the error type, without a diagnostic.
* **CHK-20** Compiling a function means its signature; a body is checked after every signature is known, except by CHK-135 ([why](why/checking.md#chk-20)).

Two structs that hold each other report `dependency-cycle` with the detail `a -> b -> a`.

```sgl sketch
struct a:
    x: b

struct b:
    y: a
```

## Types

* **CHK-21** A type is canonical: two expressions name the same type exactly when they resolve to the same type, and nothing converts implicitly.
* **CHK-22** Each `struct` declaration is one type, whatever its fields.
* **CHK-23** A type position holds a name that resolves to a `struct`; every other expression there is `unsupported-yet`.
* **CHK-24** A name in a type position that is not declared is the normal error `unknown-name`, and one that stands for a function or a binding is `wrong-kind-of-name`.
* **CHK-25** A format is not a type: nothing such as `rgba8` exists.
* **CHK-26** A field, a binding member and a parameter have a type; one without is the normal error `missing-type`.
* **CHK-27** A default value, a `mut` member, a property and a method are `unsupported-yet`.
* **CHK-28** Two fields of one struct, two members of one binding and two parameters of one function differ in name, or the later one is `duplicate-declaration`.

## Enums

* **CHK-142** An `enum` declaration is a symbol and one type, whose kind is `enumeration`; two declarations are two types, as CHK-22 says of a `struct`.
* **CHK-143** A member of an `enum` block that is a case gives the type one **case**, named by that member, in the order written.
* **CHK-144** A case has an `int` value: the one it writes, or one more than the case before it, and 0 for the first.
* **CHK-145** Two cases of one enum may hold one value, so `all = 7` beside `red = 1` is a legal alias; two that hold one name is `duplicate-declaration` by CHK-28.
* **CHK-146** The written value of a case is an `int` literal, by CHK-61; any other expression there is `unsupported-yet`.
* **CHK-147** `enum.case` is an expression of the enum's type, and a name the enum has no case of is `unknown-member`.
* **CHK-148** An enum's own name is no value, by CHK-63.
* **CHK-149** `==` and `!=` over two values of one enum are the language's own, as `and` is by CHK-116: no function declares them, and each gives a `bool`.
* **CHK-150** An enum converts to no type and no type converts to it, `int` included, and it has no other operator ([why](why/checking.md#chk-150)).
* **CHK-151** A property, a method or a nested declaration in an `enum` block is `unsupported-yet`, as CHK-27 makes each on a `struct`.

```sgl
enum light_kind:
    point
    spot
    sun
```

## Builtins and the prelude

* **CHK-29** The prelude is SGL source, and each of its files is checked like the program's file; CHK-138 says which files it has.
* **CHK-30** A declaration that carries `@builtin` stands for one record of the compiler's **builtin registry**.
  The key of a `struct` is its **name**, and the key of a `fun` is its name together with its parameter types, so every overload is a record of its own ([why](why/checking.md#chk-30)).
* **CHK-31** A `@builtin` declaration no record has the key of is the normal error `unknown-builtin`.
  That is a name the registry does not hold, a name it holds as the other kind of declaration, or parameter types no overload of the name takes.
* **CHK-32** A `@builtin fun` has no body, and every other `fun` has one, or it is the normal error `expected-body`.
* **CHK-33** A `struct` without a block is **opaque**: it has no fields and no constructor.
* **CHK-34** An opaque struct carries `@builtin`, or it is the normal error `opaque-struct-needs-builtin`.
* **CHK-35** A builtin struct with a block has real fields, and they are read like the fields of any struct.
* **CHK-36** `@operator("*")` on a function names the operator it stands for, as one quoted literal; anything else is the normal error `invalid-attribute-arguments`.
* **CHK-37** An `@operator` function is found through its operator alone: its own name is in no scope ([why](why/checking.md#chk-37)).
* **CHK-38** The pass knows the attributes of the table below, each on the node kind the table names.
* **CHK-39** Any other attribute, anywhere, is `unsupported-yet`, and arguments on a known attribute that takes none are `invalid-attribute-arguments`.
* **CHK-106** `@pure` on a function says that a call of it has no [effect](evaluation.md#effects); a `@builtin` without it is assumed to have one ([why](why/checking.md#chk-106)).

| on | the known attributes |
|---|---|
| a function | `@builtin`, `@pure`, `@operator`, `@vertex`, `@pixel` |
| a struct | `@builtin`, `@vertex`, `@pixel` |
| a binding | `@inline` |
| a struct field | `@position` |

```sgl
@builtin struct float

@builtin struct vec3:
    x: float
    y: float
    z: float

@builtin @pure fun dot(a: vec3, b: vec3) -> float
@builtin @pure @operator("+") fun add(a: float, b: float) -> float
```

## Bindings

* **CHK-40** A `binding` with a block is a symbol whose members have a name and a type; the composition form is `unsupported-yet`.
* **CHK-41** `@inline` on a binding is recorded on the checked binding, since an emitter needs it.
* **CHK-42** Each entry of a function's binding list is the bare name of a binding; any other entry is `unsupported-yet`.
* **CHK-43** A binding list is checked and never passed ([why](why/checking.md#chk-43)).
* **CHK-44** `binding.member` is an expression of the member's type, inside a function whose binding list names that binding.
* **CHK-45** In any other function it is the normal error `binding-not-listed`.
* **CHK-46** A binding is no value: its bare name in an expression is `unsupported-yet`.
* **CHK-171** A buffer member is known to the host by its path, `binding.member`, and a group's constant block by the binding's name ([why](why/checking.md#chk-171)).
* **CHK-177** A texture, an image and a sampler are types of a binding member, as a buffer is, and none is a value: [bindings.md](../bindings.md) is the model.
  Two mentions of one such type are one type, and a member names each of them as its spelling does: `texture2d[float4]`, `out image2d[.rgba8_unorm]`.
* **CHK-178** A texture's argument is `float`, `int` or `uint`, one to four wide; anything else is `wrong-kind-of-name`, and a texture without its argument is too.
* **CHK-179** An image's argument is exactly one enum case naming one of sg's storage formats, `.rgba8_unorm`.
  It is the one value type argument SGL reads, until value type arguments exist in general.
* **CHK-180** A form some backend lacks is the normal error `needs-feature`, naming the feature, on every target alike:
  `texture2d_ms_array`, an image outside the portable storage formats, and a `mut` image outside the three `r32` formats.
* **CHK-181** `@unfilterable` stands on a texture member of floats, and anywhere else is `wrong-kind-of-name`.
* **CHK-182** `@non_filtering` stands on a `sampler` member, and anywhere else is `wrong-kind-of-name`.
* **CHK-183** A `sampler name:` block in a binding is a member whose type is `comparison_sampler` where it sets `compare`, and `sampler` otherwise.
  Its settings are `sg::sampler`'s fields, each an enum case or a number; an unknown setting or value is `invalid-attribute-arguments`.
* **CHK-184** A static sampler in an `@inline` binding is `wrong-kind-of-name`, since such a binding holds constants only.
* **CHK-185** A `@builtin` function alone may take a texture, an image or a sampler; for any other function each is `unsupported-yet`, as it is anywhere a value stands.
* **CHK-186** A builtin's image parameter names the texel it loads or stores instead of a format, `out image2d[float4]`, and is a pattern:
  it takes every image of that shape whose format's texel is that type, and which the shader may read where the pattern reads, or write where it writes.

```sgl
@inline binding constants:
    view_projection: mat4

@vertex fun main_vs(v: cube_vertex){constants} -> pixel_input:
    return {
        position = constants.view_projection * v.position
        normal = v.normal
        color = v.color
    }
```

## Functions

* **CHK-47** A function has typed parameters, and its return type stands behind `->`; one without returns nothing, by CHK-121.
* **CHK-48** A function with type parameters, with `self`, or with a default argument is `unsupported-yet`, and it fails as a whole.
* **CHK-49** A function whose signature holds the error type is failed.
* **CHK-50** A function body is an ordered scope: a parameter is visible from the start, and a local from the statement after its `let`.
* **CHK-51** `let name = value` introduces an immutable local of the type of `value`.
* **CHK-52** `let name : type = value` needs `value` to be of `type`, or it is the normal error `type-mismatch`.
* **CHK-53** A local **shadows** every earlier local and parameter of its name, in its own block or an enclosing one, from the statement after its `let`, as in Rust ([why](why/checking.md#chk-53)).
  The value it is given still sees the one it hides, and its type may differ.
* **CHK-54** A local or a parameter that has the name of a module-level symbol is `unsupported-yet` ([why](why/checking.md#chk-54)).
* **CHK-55** A pattern in a `let` and a `let` without a value are `unsupported-yet`; `let mut` is CHK-111.
* **CHK-56** `return value` needs `value` to be of the function's return type, or it is `type-mismatch`.
* **CHK-57** An arrow body `=> value` is `return value`.
* **CHK-58** A block body that returns a value does so on every path, by CHK-123 to CHK-125.
* **CHK-59** `assert` and a declaration inside a function are `unsupported-yet`; a call is a statement by CHK-137, and every other statement is [control flow](#control-flow).

## Expressions

* **CHK-60** A number literal with a DOT or an exponent, in decimal and without a suffix, is of the prelude's type `float`; a sign directly on it is part of it.
* **CHK-61** A decimal literal of digits alone is of the prelude's type `int`, and a sign directly on it is part of it.
  One that does not fit 32 bits is `unsupported-yet`, and so is a literal with a prefix, a suffix or a `p` exponent.
* **CHK-62** A name resolves to a local or a parameter first, and to a symbol of the module after that; one that resolves to nothing is `unknown-name`.
* **CHK-63** A name that stands for a struct, a function or a binding is no value by itself: it is `unsupported-yet`.
* **CHK-64** `value.name` is the field `name` of the struct type of `value`; a type without that field is the normal error `unknown-member`.
* **CHK-65** `(x)` is `x`.
* **CHK-152** `.name` is the case `name` of the enum the context expects, which today is the scrutinee of a `case` and nothing else.
  A leading dot where no type is expected is `unsupported-yet`, and one whose expected type is no enum, or has no such case, is `unknown-member`.
* **CHK-66** Every expression kind not named in this section is `unsupported-yet`.

## Calls and overloads

* **CHK-67** A paren call `f(x)` and a juxtaposition call `f x` are the same call.
* **CHK-68** An infix or a prefix operator is a call of the `@operator` functions of its spelling, with its operands as arguments.
* **CHK-69** Operators and functions are one mechanism: both resolve by CHK-70 to CHK-73.
* **CHK-70** A candidate **matches** when it takes as many parameters as there are arguments and each parameter type is the argument's type.
* **CHK-71** No matching candidate is the normal error `no-matching-overload`, and its detail spells the call with its argument types.
* **CHK-72** Two or more matching candidates are the normal error `ambiguous-overload` ([why](why/checking.md#chk-72)).
* **CHK-73** Exactly one matching candidate is the call's target, and its return type is the call's type.
* **CHK-74** A call of a function that is not `@builtin` resolves like any other, and it is inlined, by CHK-127 to CHK-132.
* **CHK-75** A call whose callee names a struct is a call of that struct's **constructor**, which takes one argument per field, in field order.
* **CHK-76** A constructor call always has the struct as its type; arguments that do not match the fields are `no-matching-overload`.
* **CHK-77** A splat argument `..value` in a constructor call stands for the fields of `value`, in order; `value` is of a struct type with fields, or it is `type-mismatch`.
* **CHK-78** A splat in any other call, a named argument, a call of a local, a method call and type arguments are `unsupported-yet`.
* **CHK-79** A callee that names a binding is `wrong-kind-of-name`.
* **CHK-80** `and`, `or` and `not` are no functions: CHK-116.
* **CHK-174** `x as T` is a call of the `@operator("as")` function whose one parameter is the type of `x` and whose result is `T`.
  The result takes part in the match, since the overloads of `as` differ in it; no such function is `no-matching-overload`.
* **CHK-175** `x as T` where `x` already has the type `T` is `x`.
* **CHK-176** The prelude converts between `float`, `int` and `uint` of one width, and nothing else.
  A float becomes an integer by truncating toward zero and saturating at the integer's bounds, and a NaN becomes 0.
  Between `int` and `uint` the bits stay.

```sgl
let n = normalize p.normal
let key = saturate dot(n, normalize vec3(0.45, 0.8, -0.4))
let lit = p.color * (0.25 + 0.8 * key + 0.25 * fill)
let color = float4(..lit, 1.0)
```

## Objects

* **CHK-81** An object `{ name = value, … }` as the value of a `return` converts to the function's return type, which is a struct with fields.
* **CHK-82** An object as the value of a field of struct type converts to that type in the same way.
* **CHK-83** The conversion is structural: each field of the struct is named exactly once, and each value is of its field's type.
* **CHK-84** A field left unnamed is `missing-field`, a name that is no field `unknown-field`, a field named twice `duplicate-field`, and a value of another type `type-mismatch`.
* **CHK-85** The order of the elements is free.
* **CHK-86** A splat and a shorthand element are `unsupported-yet`, and so is an object anywhere else.

## Entry points

* **CHK-87** A function that carries `@vertex` or `@pixel` is an **entry point** of that stage; one that carries both is the normal error `invalid-entry-point`.
* **CHK-88** An entry point takes exactly one parameter, of a struct type with fields.
* **CHK-89** The parameter of a `@vertex fun` is of a `@vertex struct`.
* **CHK-90** A `@vertex fun` returns a struct with exactly one field that carries `@position`, and that field is of the type `hpos4`.
* **CHK-91** A `@pixel fun` returns a `@pixel struct`.
* **CHK-92** An entry point is neither `@builtin` nor `@operator`.
* **CHK-173** `@per_instance` and `@stream(name)` are attributes of a struct field, recorded on the member; `@stream` takes one bare name.
* **CHK-93** Breaking one of CHK-88 to CHK-92 is `invalid-entry-point`, and its detail names the rule.

## The flat tree

* **CHK-94** The pass has two results: side tables over the untouched ASTs, and one **flat tree** per entry point.
* **CHK-95** The side tables give each expression its type and its **target**: a local, a parameter, a symbol, the chosen overload, a constructor, a field or a binding member.
* **CHK-96** An expression in a type position has the type it names.
* **CHK-97** A flat tree holds locals, structured control flow, member access, binding members, constructions, literals and calls of `@builtin` functions ([why](why/checking.md#chk-97)).
* **CHK-107** The pass writes the **structured form**, whose meaning is [evaluation.md](evaluation.md); `once` and a bare `break` are the core form's alone.
* **CHK-108** A flat call records whether its callee is `@pure`, so a reader of the tree needs no symbol to know whether a call has an effect.
* **CHK-98** Every flat expression has a type, and none has the error type.
* **CHK-99** Every flat node names the AST node it came from and the chain of call sites it was inlined through, outermost first.
  The chain is empty for a node of the entry point's own body.
* **CHK-100** An entry point whose signature or body reported an error has no flat tree, and neither has one that reaches such a function, by CHK-132.
* **CHK-101** A flat entry point records its stage, its name, its parameter's struct, its result struct, and the bindings of its binding list in the order written.
* **CHK-102** A returned object is a construction with one value per field, in field order.
* **CHK-103** A splat is spread into one member access per field.
  A splatted value that is no local is bound to a temporary local where its first member stands, so it is evaluated once and no earlier than written.
* **CHK-104** Every name an emitter writes comes from one **mint**, which hands out a desired name when it is free and `name_1`, `name_2`, … otherwise ([why](why/checking.md#chk-104)).
* **CHK-105** An entry point keeps its name, and every module-level name is taken in the mint before the first local is minted.

## Control flow

* **CHK-109** The body of an `if` branch, of a `while`, of a `for` and of a `loop:` is a block, and a block is a scope: a local ends where its block ends.
* **CHK-110** A local of an inner block that shadows one of an enclosing block is CHK-53's shadowing: the outer local is visible again once the block ends ([why](why/checking.md#chk-110)).
* **CHK-111** `let mut name = value` introduces a mutable local; a local without `mut`, a parameter and the variable of a `for` are immutable.
* **CHK-112** `place = value` needs `place` to be a mutable local or a member of one, at any depth, or it is the normal error `not-assignable`.
  `value` is of the type of `place`, or it is `type-mismatch`.
* **CHK-113** `place op= value` is `place = place op value`: the operator resolves by CHK-70 to CHK-73, and its result is of the type of `place`.
* **CHK-114** The condition of an `if` and of a `while` is a `bool`, or it is `type-mismatch`.
* **CHK-115** `for name in first ..< end` runs over `int`: both bounds are `int`, and `name` is an immutable `int` local of the body.
  A type on the variable is `int`; a range that is not `..<`, and anything else behind `in`, is `unsupported-yet` ([why](why/checking.md#chk-115)).
* **CHK-116** `and`, `or` and `not` are the language's own: their operands are `bool` and so is their value, and no function declares them ([why](why/checking.md#chk-116)).
* **CHK-117** In a comparison chain each operator resolves over its two neighbours by CHK-70 to CHK-73 and gives a `bool`, and the chain is a `bool`.
* **CHK-118** A `loop:` that is a statement is left by `break`, and one that is a value by `break value`.
  The values of one loop are of one type, which is the loop's; a `break` of the other kind is `type-mismatch`.
* **CHK-119** `break` and `continue` name the innermost loop around them.
* **CHK-120** `return`, `break` and `continue` are statements; one that stands as the result of a `case` arm leaves as it says, and that arm produces no value (CHK-165).
  One that stands as a value anywhere else is `unsupported-yet`.
* **CHK-135** `print value` takes a value of any type; a string is `unsupported-yet`.

```sgl
fun falloff(d: float, steps: int) -> float:
    if d <= 0.0 => return 0.0
    let mut w = 1.0
    for i in 0 ..< steps:
        w *= d
        if w < 0.125 => return 0.0
    return w
```

## Case

* **CHK-153** `case value:` evaluates `value`, the **scrutinee**, and runs the first arm whose pattern matches it.
* **CHK-154** A pattern is an expression of the scrutinee's type, and it **matches** when `scrutinee == pattern` ([why](why/checking.md#chk-154)).
* **CHK-155** The scrutinee is of a type whose `==` resolves by CHK-70 to CHK-73, which an enum's does by CHK-149; one that does not is `no-matching-overload` at the arm.
* **CHK-156** `_` is a pattern that matches anything, and the arm that carries it is the **default**.
* **CHK-157** `a or b` in a pattern is a list of patterns, and the arm matches when any of them does; the `or` of CHK-116 is not involved and its operands are no `bool`s.
* **CHK-158** A pattern is evaluated only where it is reached, so a pattern behind the one that matched is never evaluated ([evaluation](evaluation.md#case)).
* **CHK-159** A `case` is **exhaustive** when it carries a `_`, or when its scrutinee is an enum and its patterns are constant cases that together name every case of it.
* **CHK-160** A `case` that is not exhaustive is the normal error `non-exhaustive-case`; its detail names the cases nobody matched, or says that a `_` is needed ([why](why/checking.md#chk-160)).
* **CHK-161** Two constant patterns of one `case` that name one case is the normal error `duplicate-case-pattern`, at the later arm.
  Two patterns that are equal expressions are not compared: what they hold is known at run time and not here.
* **CHK-162** An arm behind a `_` never runs: it is the warning `unreachable-code`, once per `case`.
* **CHK-163** A `case` that stands as a statement has arms that produce no value.
* **CHK-164** A `case` that stands as an expression has one type, and every arm produces a value of it or exits.
* **CHK-165** An arm **exits** when its body exits by CHK-123, which is what lets `_ => return black` stand where a value is expected.
* **CHK-166** The type of a `case` expression is the one the context expects where there is one, and the type of the first arm that produces a value otherwise.
* **CHK-167** An arm whose value is of another type is `type-mismatch`, reported at the arm.
* **CHK-168** An arm of a `case` expression that neither produces a value nor exits is the normal error `missing-value-in-arm`.
* **CHK-169** A `case` statement whose every arm exits makes its own statement list exit, by CHK-123.
* **CHK-170** An arm's body is a block named after the `case`, as CHK-133 names the block of an inlined call.
  `=> value` leaves it with that value, a `=>:` block leaves it with its `yield`, and the block is EVAL-29's construct in both.

```sgl
fun shade(kind: light_kind, base: float) -> float:
    let weight = case kind:
        .point => 1.0
        .spot or .sun => 0.5
        _ => return 0.0
    return base * weight
```

## Returning

* **CHK-121** A function without `-> T` returns nothing: its `return` carries no value, and a call of it is a statement.
* **CHK-122** An arrow body without a written return type returns what its expression is: the function's return type is the type of that expression.
  A block body without `-> T` still returns nothing, by CHK-121.
* **CHK-123** A statement list **exits** when it holds a `return`, a `break` or a `continue`, an `if` with an `else` whose every branch exits, or a `loop:` that holds no `break` of its own.
  So does an exhaustive `case` (CHK-159) whose every arm exits.
* **CHK-124** A `while` and a `for` never make their list exit, whatever their condition is ([why](why/checking.md#chk-124)).
* **CHK-125** The body of a function that returns a value exits, or it is the normal error `missing-return`.
* **CHK-126** A statement that follows an exit in its list never runs: it is the warning `unreachable-code`, once per list.

This one is `missing-return`: where `x` is at least 0.5, its body ends without a `return`.

```sgl
fun grade(x: float) -> float:
    if x < 0.5 => return 0.0
```

## Calls of the program's functions

* **CHK-127** A call of a function that is no `@builtin` is defined by **substitution**: it means the callee's body, with each parameter standing for its argument ([evaluation](evaluation.md#calls)).
* **CHK-128** Every call is inlined, so no function of the program reaches a target.
* **CHK-129** Each function body is checked once, on its own, whether or not anything calls it ([why](why/checking.md#chk-129)).
* **CHK-130** A function that calls itself, directly or through others, is the normal error `recursive-call`.
  It is reported once per loop, at the call that closes it, and its detail names the loop: `a -> b -> a`.
* **CHK-131** Bindings are an effect: a call needs every binding of the callee's list in the caller's list, or it is `binding-not-listed` at the call.
  So what a function reads is listed by whoever calls it, up to the entry point ([why](why/checking.md#chk-131)).
* **CHK-132** An entry point has a flat tree when its own body and the body of every function it reaches checked without an error, recursion included.
* **CHK-133** An inlined call is a block named after its callee, and two inlines of one function share no local: each gets its names from the mint.
* **CHK-134** A `mut` parameter, a lambda, a function as a value and a nested function are `unsupported-yet`.

## Inferred results and dropped values

* **CHK-135** A function whose return type is inferred by CHK-122 has its body checked as part of compiling it: for such a function the body belongs to the signature ([why](why/checking.md#chk-135)).
* **CHK-136** So needing such a function from inside its own body, directly or through other inferred functions, is `dependency-cycle` by CHK-18.
  One written return type on the loop makes it the `recursive-call` of CHK-130.
  A call does not need an overload whose parameter types cannot take it, since parameters are known before a result is: an overload set stays usable from inside one of its inferred members.
* **CHK-139** An arrow body whose expression is nothing, which is a call of a function that returns nothing, is `type-mismatch`: the body of such a function is its result.
* **CHK-137** A call that stands as a statement is evaluated and its value dropped, whatever it calls; any other expression as a statement is `unsupported-yet` ([why](why/checking.md#chk-137)).
  In the flat tree it is an `eval` of the call, and a call of a function that returns nothing is the block of CHK-133 as a statement.

```sgl
fun make_mvp(model: mat4){frame} => frame.proj * frame.view * model
```

## The two files of the prelude

* **CHK-138** The prelude has two files, in this order: `builtins.sgl`, which the builtin registry generates, and `core.sgl`, which is written by hand ([why](why/checking.md#chk-138)).
* **CHK-140** The text of `builtins.sgl` that is checked is generated in memory, and the committed file is byte for byte the same, so a diagnostic's line and column are right in it.
* **CHK-141** Every record of the registry carries its declaration as SGL source, and that text goes through the same parser as any other: the registry has no second signature language.

## Diagnostic kinds

Every kind below is a normal error by [DIAG-4](../syntax/diagnostics.md#rules), except `unreachable-code`, which is a warning.
A diagnostic of this pass has a kind, a file, a byte span in that file, and a detail.

| kind | reported by |
|---|---|
| `unsupported-yet` | CHK-8 |
| `duplicate-declaration` | CHK-12, CHK-28 |
| `dependency-cycle` | CHK-18, CHK-136 |
| `unknown-name` | CHK-24, CHK-62 |
| `wrong-kind-of-name` | CHK-24, CHK-79 |
| `missing-type` | CHK-26 |
| `unknown-builtin` | CHK-31 |
| `expected-body` | CHK-32 |
| `opaque-struct-needs-builtin` | CHK-34 |
| `invalid-attribute-arguments` | CHK-36, CHK-39 |
| `binding-not-listed` | CHK-45, CHK-131 |
| `type-mismatch` | CHK-52, CHK-56, CHK-77, CHK-84, CHK-112 to CHK-118, CHK-121, CHK-139, CHK-167 |
| `not-assignable` | CHK-112 |
| `missing-return` | CHK-125 |
| `unreachable-code` | CHK-126, CHK-162 |
| `recursive-call` | CHK-130 |
| `unknown-member` | CHK-64, CHK-147, CHK-152 |
| `no-matching-overload` | CHK-71, CHK-76, CHK-155 |
| `non-exhaustive-case` | CHK-160 |
| `duplicate-case-pattern` | CHK-161 |
| `missing-value-in-arm` | CHK-168 |
| `needs-feature` | CHK-180 |
| `ambiguous-overload` | CHK-72 |
| `missing-field`, `unknown-field`, `duplicate-field` | CHK-84 |
| `invalid-entry-point` | CHK-87, CHK-93 |

## Open

* Whether `@builtin` is allowed outside the prelude; today it is.
* Whether a builtin's declaration is checked against its record beyond the key; today its result type and its attributes are not.
* Whether a local may shadow a module-level name ([scopes](../incubator/scopes.md)); shadowing a local or a parameter is CHK-53.
* Whether a body is checked once or where it is inlined, once a generic makes the two differ ([why](why/checking.md#chk-129)).
* `true` and `false`, which are names nothing declares yet.
* Whether a second function with the parameter types of another is an error where it is declared.
* Whether a pattern may bind a name, which is the pattern language of [patterns](../incubator/patterns.md) and the thing that would make exhaustiveness a real analysis.
* Whether an enum reaches `int` through a cast, and what an `int` that names no case then is ([enum futures](../incubator/enum-futures.md)).
* Where a leading dot is resolved beyond a `case` scrutinee: a parameter, a field and a return type each expect a type too.
* Whether the bindings of a flat entry point are the ones its list names or the ones its body reads.
* How a splatted value reads in the emitted text once a target can take the vector whole.
