# AST

The AST phase reads the [form tree](forms.md) of one file into **declarations**, **statements** and **expressions**.
The semantics the AST feeds are specified elsewhere, and name lookup is not part of this phase.
Back to the [phases](_index.md); the reasons are in [why/ast.md](why/ast.md).

## The phase

* **AST-1** The AST phase reads the form tree of one file into one AST, and it reads nothing else.
* **AST-2** The AST phase is **name-free**: it never looks a name up ([why](why/ast.md#ast-2)).
* **AST-3** Where only name lookup can choose between two readings, the AST holds one neutral node that carries both.
* **AST-4** The AST phase is total: any form tree gives an AST and a list of [diagnostics](diagnostics.md).
* **AST-5** A form that has no reading in its place is an `invalid` node that keeps its form, it is a normal error, and every other node of the file is read as if it were not there.
* **AST-6** Every AST node names the form it is read from.
* **AST-7** A name in the AST is the byte span of its identifier, with its spelling as written ([why](why/ast.md#ast-7)).
* **AST-8** An AST node is of one of three families: expression, statement or declaration.

| source | what name lookup decides later | the neutral node |
|---|---|---|
| `vec3` | a type, a function or a variable | `name` |
| `buffer[float]` and `weights[3]` | type arguments or a subscript | `index` |
| `frame.proj` | a member of a binding, a module or a value | `member` |
| `vec3(1, 2, 3)` | a constructor or a function call | `call` |

## Expressions

* **AST-9** There is one family of expressions, and a type is written as an expression of that family ([why](why/ast.md#ast-9)).
* **AST-10** A **type position** is the right side of `:`, of `->` or of `as`, and the right side of the `=` of a `type` alias.
* **AST-11** The AST records that an expression stands in a type position, and it reads that expression like any other.

| node | is read from |
|---|---|
| `literal` | a number literal, a quoted literal or a hash literal |
| `name` | an identifier |
| `self_ref` | the identifier `self` |
| `void_ref` | the identifier `void` |
| `wildcard` | `_` |
| `leading_dot` | a leading-dot form: `.point`, `.0` |
| `member` | a member access: `a.b` |
| `index` | an applied square group: `a[i]` |
| `call` | `f(x)`, `f x`, `a + b`, `-x`, `not x` |
| `tuple`, `array`, `object` | a round, square or curly paren literal |
| `comparison_chain` | a run of two or more comparisons: `0 <= i < n` |
| `cast` | `x as t` |
| `membership` | `x in r` |
| `ascription` | `x : t` |
| `range` | `a ..< b`, `a ..= b` |
| `lambda` | `parameters => body`, and a `fun` without a name |
| `case` | `case value:` and a block of arms |
| `loop` | `loop:` and a block |
| `return`, `break`, `continue`, `yield` | the keyword forms of those names |
| `struct_type` | a curly paren literal whose elements are all `name: type` |
| `function_type` | `(a, b) -> c` |
| `qualified_type` | `mut` or `out` in a type position: `mut buffer[float]` |
| `with_bindings` | a curly group applied to an expression; reserved |
| `invalid` | a form with no reading in its place |

### Atoms

* **AST-12** A quoted literal keeps its form, and its pieces and interpolations are reached through that form.
* **AST-13** `self` is a **reserved name**: the identifier `self` reads as `self_ref` wherever it stands, and it is no keyword ([why](why/ast.md#ast-13)).
* **AST-141** No declaration, field or enum case may be named by a reserved name, and no parameter may be named `void`; each is the normal error `reserved-name`.
  A parameter named `self` is the receiver of a method, which is what the name is reserved for.
* **AST-137** `void` is a reserved name too: the identifier `void` reads as `void_ref` wherever it stands, the type in a type position and its value elsewhere ([why](why/ast.md#ast-137)).
* **AST-14** An applied square group reads as `index`, which is a subscript or type arguments, and its elements are arguments ([why](why/ast.md#ast-14)).

```sgl
let tint = #ff00bb
let label = "frame $index"
let kind = .point
let first = pair.0
let weight = weights[3]
let samples : array[vec3, 16]
self.count = 0
```

### Calls

* **AST-15** Every application of a function is one node, `call`: a callee, its arguments and a **call spelling** ([why](why/ast.md#ast-15)).
* **AST-16** The call spellings are paren `f(x)`, juxtaposition `f x`, infix `a + b` and prefix `-x`.
* **AST-17** The callee of an infix or a prefix call is the operator token.
* **AST-18** A left-associative operator run reads as nested calls: `a - b + c` is the call `+` of the call `-` and `c`.
* **AST-19** `and` and `or` read as infix calls that are marked short-circuiting, and `not` reads as a prefix call.
* **AST-20** One comparison reads as an infix call.
* **AST-21** A run of two or more comparisons reads as `comparison_chain`: each operand once, in order, with the operator tokens between them ([why](why/ast.md#ast-21)).
* **AST-22** `as` reads as `cast`, `in` as `membership`, `:` in expression position as `ascription`, and `..<` and `..=` as `range`; none of them is a call.

```sgl
let a = cross(n, l)
let b = cross n l
let c = a - b + c
let d = -a
let e = ready and not hidden
let f = 0 <= i < n
let g = i as float
let h = (x : float) * 0.5
let k = x in 0..<1
```

| source | reads as |
|---|---|
| `cross(n, l)` | `call`, paren, callee `cross` |
| `cross n l` | `call`, juxtaposition, callee `cross` |
| `a - b + c` | `call`, infix, callee `+`, of: `call`, infix, callee `-`; and `c` |
| `-a` | `call`, prefix, callee `-` |
| `ready and not hidden` | `call`, infix, short-circuiting, callee `and`, of `ready` and: `call`, prefix, callee `not` |
| `i < n` | `call`, infix, callee `<` |
| `0 <= i < n` | `comparison_chain` of `0`, `i`, `n` with `<=` and `<` |
| `i as float` | `cast`, with `float` in a type position |

### Lists

* **AST-23** An element of a paren group reads as an **argument**: an optional name, a value, and whether it is a splat.
* **AST-24** An assignment directly inside a paren group is a named argument, and its left side, an identifier or a leading-dot form, is the name.
* **AST-25** A paren literal reads as `tuple` when it is round, `array` when it is square and `object` when it is curly.
* **AST-26** `(x)` is `x` and no tuple, `()` is the empty tuple, and `(x,)`, `(a = 1)` and `(..v)` are each a tuple of one element.
* **AST-27** An element of an `object` that is one bare identifier is the **object shorthand** for `a = a`, and the AST records the shorthand without expanding it.
* **AST-28** The **splat** is the prefix operator `..`, and it must be a whole element of a paren group: `(..normal, 0)` ([why](why/ast.md#ast-28)).
* **AST-29** A splat is recorded on its argument, and a splat that is not a whole element of a paren group is a normal error.
* **AST-97** An element of an `object` is the object shorthand, a named argument or a splat.
* **AST-98** Any other element of an `object` is a normal error, and it is kept as a positional argument.

```sgl
let unit = ()
let same = (x)
let one = (x,)
let pair = (x, y)
let named = (x = 1, y = 2)
let light = make_light(color = #fff, 2.0, falloff = 0.5)
let short = {albedo, roughness = 0.5}
let wide = (..normal, 0)
let rough = {..defaults, roughness = 0.5}
let all = max(..values)
```

| source | reads as |
|---|---|
| `(x)` | `x` |
| `(x,)` | `tuple` of one argument |
| `(a = 1)` | `tuple` of one argument with the name `a` |
| `(x = 1, y = 2)` | `tuple` of two arguments with the names `x` and `y` |
| `{albedo, roughness = 0.5}` | `object` of the shorthand `albedo` and the argument `roughness` |
| `(..normal, 0)` | `tuple` of the splat of `normal` and `0` |
| `{..defaults, roughness = 0.5}` | `object` of the splat of `defaults` and the argument `roughness` |

The splat below is an operand of `+` and no whole element, so the AST reports a normal error.

```sgl sketch
let sum = 1 + ..rest
```

`1 + 2` is no shorthand, no named argument and no splat, so the AST reports a normal error and keeps it as a positional argument.

```sgl sketch
let v = {1 + 2}
```

### Types

* **AST-30** A curly paren literal whose elements are all of the shape `name: type`, each with an optional `= default`, reads as `struct_type`, and its elements are [fields](#members).
* **AST-31** A curly paren literal that holds both `name: type` and `name = value` elements is a normal error.
* **AST-32** An `->` that is not the return type of a [signature](#functions) reads as `function_type`: the parameter types on its left and the result type on its right.
* **AST-99** The AST reads an `->` as the form tree groups it, and it regroups nothing ([OP-32](operators.md#the-precedence-ladder)).
* **AST-100** `x : (int) -> int` is an `ascription` whose type is a `function_type`, and `a -> b -> c` is a `function_type` whose result is the `function_type` `b -> c`.
* **AST-33** A curly group applied to an expression reads as `with_bindings`, which is reserved: the node is kept, and it is a normal error that says the construct is not supported yet.
* **AST-128** `mut` and `out` in a type position read as `qualified_type`, which records the word and the type it qualifies: `mut buffer[float]`, `out image_2d[.rgba8_unorm]`.
* **AST-129** A `qualified_type` says what a shader does with a resource, and [bindings.md](../bindings.md) is what the words mean.
  The AST checks neither the word against the type nor the type against anything.
* **AST-130** `mut` or `out` outside a type position is read as it is elsewhere, so `mut` keeps AST-45 and `out` in an expression is a normal error.
* **AST-135** `sampler` alone in a type position reads as the name `sampler`: the keyword denotes the sampler type there, `smp: sampler`.

```sgl
type blend = (vec3, vec3) -> vec3
type curried = (float) -> (float) -> float
let ease : (float) -> float = smooth_step

fun project(v: basic_vertex) -> {@position pos: hpos4, uv: vec2}:
    return {pos = mvp * v.pos, uv = v.uv}

fun unproject(p: hpos4) -> {
    pos: pos3
    depth: float
}:
    return {pos = p.xyz / p.w, depth = p.z}
```

`{pos: hpos4, tint = #fff}` mixes the two shapes, so the AST reports a normal error.

```sgl sketch
let v : {pos: hpos4, tint = #fff}
```

The curly group is applied to a call, so the AST reports that `with_bindings` is not supported yet.

```sgl sketch
let color = sample_sky(dir){sky = frame_sky}
```

### Lambdas, `case` and `loop`

* **AST-34** An `=>` in expression position reads as `lambda`, the **arrow lambda**: its parameters on the left and its body on the right.
* **AST-35** The parameters of an arrow lambda are one name, `_`, or a round list of [parameters](#functions) whose types may be left out.
* **AST-101** A `fun` whose [signature](#functions) has no name, in expression position, reads as `lambda` too, the **anonymous function**: `fun (x) => x + 1` ([why](why/ast.md#ast-101)).
* **AST-102** The signature of an anonymous function follows AST-66 to AST-71 without the name: its parameters are mandatory, and its lists stand in the same order.
* **AST-103** An anonymous function may take type parameters, bindings and a return type, and it may be left by `return`; an arrow lambda allows none of these.
* **AST-104** A `lambda` records which of the two spellings it has.
* **AST-105** A `fun` with a name in expression position is a normal error, by AST-5.
* **AST-36** `case value:` reads as `case`, and each statement of its block is an **arm**: `pattern => result`.
* **AST-37** The pattern of an arm is an expression, and its result is a [body](#statements), AST-127 among them.
* **AST-38** A statement of a `case` block that is no arm is a normal error.
* **AST-39** `loop:` reads as `loop`, an expression whose value is that of the `break` that leaves it.

```sgl
let twice = x => x * 2
let sum = fold(values, (a, b) => a + b)
let area = case shape:
    .square => shape.side * shape.side
    .circle or .disc => shape.radius * shape.radius * pi
    _ => 0.0
let root = loop:
    guess = refine guess
    if converged guess => break guess
let clamped = fun (x: float) -> float:
    if x < 0 => return 0.0
    return min(x, 1.0)
let first = fun [T](values: span[T]) => values[0]
let lit = fun (n: vec3){frame} => dot(n, frame.sun_dir)
```

| source | reads as |
|---|---|
| `x => x * 2` | `lambda`, arrow, the parameter `x` |
| `(a, b) => a + b` | `lambda`, arrow, two parameters without types |
| `fun (x: float) -> float:` | `lambda`, anonymous function, one parameter, the return type `float`, a block |
| `fun [T](values: span[T]) => values[0]` | `lambda`, anonymous function, the type parameter `T` |
| `fun (n: vec3){frame} => …` | `lambda`, anonymous function, the binding entry `frame` |

### Value blocks and `yield`

**`yield` is experimental**: the rules of this section that name it may change ([why](why/ast.md#ast-106)).

* **AST-106** A block has no implicit value: its last expression is a statement like every other ([why](why/ast.md#ast-106)).
* **AST-107** A **value block** is a block that is the body of a `case` arm, of an arrow lambda or of a property: `=>:` and a block.
* **AST-108** `yield expression` gives its value to the nearest enclosing value block, and it leaves that block.
* **AST-109** The blocks of `if`, `for` and `while` between a `yield` and its value block are transparent, as they are between a `break` and its loop.
* **AST-118** A `yield` with a `loop` between it and its value block is the normal error `yield-in-loop`; it is written `break value` ([why](why/ast.md#ast-118)).
* **AST-110** A `yield` that finds the body of a `fun`, named or anonymous, before any value block is the normal error `yield-in-function`; it is written `return`.
* **AST-111** The right side of an `=>` that is an expression is the value of that body, and a `yield` that is this whole expression is the warning `redundant-yield` ([why](why/ast.md#ast-111)).
* **AST-119** A redundant `yield` keeps its node, and its value is read as if the keyword were not there.

```sgl
let area = case shape:
    .square => shape.side * shape.side
    .circle =>:
        let r = shape.radius
        yield r * r * pi
    _ => 0.0

let bright = map(colors, c =>:
    let l = luminance c
    if l > 1 => yield c / l
    yield c
)

struct ray:
    origin: pos3
    dir: vec3
    inv_dir =>:
        let d = self.dir
        yield vec3(1 / d.x, 1 / d.y, 1 / d.z)
```

| source | the value block it belongs to |
|---|---|
| `yield r * r * pi` | the body of the arm `.circle` |
| `yield c / l` | the body of the arrow lambda, through the `if` |
| `yield vec3(…)` | the body of the property `inv_dir` |

The body below is that of a `fun`, so the AST reports `yield-in-function`; it is written `return x * x`.

```sgl sketch
fun square(x: float) -> float:
    yield x * x
```

The `yield` below has a `loop` between it and the arm, so the AST reports `yield-in-loop`; it is written `break guess`, and the arm hands the value of the `loop` on with `yield loop:`.

```sgl sketch
let root = case method:
    .newton =>:
        loop:
            guess = refine guess
            if converged guess => yield guess
    _ => 0.0
```

Each `=>` below already says where the value is, so the AST reports `redundant-yield` three times and reads `0.0`, `x * 2` and `w * h`.

```sgl sketch
let area = case shape:
    _ => yield 0.0
let twice = x => yield x * 2

struct rect:
    w: float
    h: float
    area => yield w * h
```

### Jumps

* **AST-40** `return`, `break`, `continue` and `yield` are expressions, the **jumps** ([why](why/ast.md#ast-40)).
* **AST-41** `return` and `break` take at most one value, `yield` takes exactly one, and `continue` takes none; a `yield` without a value is the normal error `expected-expression`.
* **AST-112** `return` leaves the nearest enclosing `fun`, named or anonymous, through every value block and every loop between ([why](why/ast.md#ast-112)).
* **AST-113** A `return` in the block of an arrow lambda, or in a `case` arm inside one, is the normal error `return-in-lambda`; it is written `yield`.
* **AST-120** A `return` that is the whole one-line body of an arrow lambda, `x => return x`, is the normal error `redundant-return`; it is written `x => x` ([why](why/ast.md#ast-120)).
* **AST-121** `break` and `continue` name the nearest enclosing `loop`, `for` or `while` of their function, through every `if` block and every `case` arm between, and never through a lambda.
* **AST-122** A jump with no target is the normal error `jump-without-target`, and it keeps its node ([why](why/ast.md#ast-122)).
* **AST-123** A jump has no target when it is a `yield` with neither a value block nor a `fun` around it, a `return` with no `fun` around it, or a `break` or a `continue` with no loop by AST-121.

```sgl
fun shade(hit: hit_info) -> vec3:
    if not hit.is_valid => return sky_color
    let weight = case hit.kind:
        .diffuse => 1.0
        .mirror => 0.5
        _ => return black
    return hit.color * weight
```

The `return` of the arm `_` stands in a `case` inside a `fun`, so it leaves `shade`.
The one below stands in the block of an arrow lambda, so the AST reports `return-in-lambda`; it is written `yield 0.0`, or the lambda is written `fun (x):`.

```sgl sketch
let safe = map(values, x =>:
    if x < 0 => return 0.0
    yield sqrt x
)
```

The `return` below is the whole body of an arrow lambda, so the AST reports `redundant-return`; it is written `x => x * 2`.

```sgl sketch
let twice = x => return x * 2
```

None of the jumps below has a target, so the AST reports `jump-without-target` for each.
The `yield` and the `return` stand at file level, where no `fun` is around them, and the `break` stands in a lambda, which the `for` around it does not reach into.

```sgl sketch
const k = yield 1

struct ray:
    dir: vec3
    inv_dir =>:
        return 1 / dir

fun first_hit(rays: span[ray]):
    for r in rays:
        visit(r, h =>:
            break
        )
```

## Statements

* **AST-42** A **body** is a block, or the right side of an `=>`: `if done => return`, `fun f() => x`.
* **AST-117** The right side of an `=>` is an expression, or a block after `=>:`, and AST-107 says which of those blocks are value blocks.
* **AST-127** `=>` binds tighter than assignment ([OP-3](operators.md#the-precedence-ladder)), so a one-line body that assigns arrives as an assignment whose left side is the `=>`.
  It reads as that body being the assignment, in a statement and in a `case` arm alike.

```sgl
fun tally(kind: light_kind, hits: int) -> float:
    let mut total = 0.0
    if hits > 0 => total = 1.0
    case kind:
        .point => total += 1.0
        _ => total *= 0.5
    return total
```

* **AST-43** A statement is one row of the table below.

| statement | source |
|---|---|
| `let` | `let pattern`, `let pattern : type`, each with an optional `= expression`; and the same after `let mut` |
| assignment | `target = expression`, and `target op= expression` for every assignment operator |
| `if` | `if condition` with a body, then any number of `else if condition`, then at most one `else`, each with a body |
| `for` | `for name in expression` or `for _ in expression`, each with an optional `: type` after the variable, with a body |
| `while` | `while condition` with a body |
| `assert` | `assert condition` or `assert condition, message` |
| `print` | `print message` |
| a declaration | every [declaration](#declarations) that its table allows inside a function body |
| an expression | any expression |

### `let` and assignment

* **AST-44** A **pattern** is a name, `_`, or a round list of patterns.
* **AST-45** `mut` applies to the whole pattern.
* **AST-46** The type of a `let` stands in a type position.
* **AST-47** `let` is a statement only, and a `let` at file level is a normal error.
* **AST-48** An assignment is a statement and never an expression ([why](why/ast.md#ast-48)).
* **AST-49** An assignment in operand position is a normal error, except directly inside a paren group, where it is a named argument by AST-24.

```sgl
fun blend(base: vec3, top: vec3, mask: float) -> vec3:
    let result
    let scale : float
    let bias = 0.5
    let weight : float = mask * bias
    let (u, v) = (base.x, top.y)
    let mut total = vec3(u, v, 0)
    total += top * weight
    total.z = 1
    return total
```

### `if` and `else`

* **AST-50** `if`, `else if` and `else` are sibling forms, and the AST chains them into one `if` statement.
* **AST-51** An `else` or an `else if` pairs with the form directly above it among its siblings, which must be an `if` or an `else if`.
* **AST-52** Blank lines and comment lines are no forms, so they may stand between the two.
* **AST-53** An `else` that pairs with nothing is a normal error, and it reads as an `if` statement with a missing condition, so its body is still read.

```sgl
if count > 0:
    step()

// nothing left to do
else if count == 0 => finish()
else:
    fail()
```

The `else` below follows a `let`, so the AST reports a normal error and still reads `fail()`.

```sgl sketch
let x = 1
else:
    fail()
```

### Loops

* **AST-54** A `for` takes exactly one argument, a `membership` whose left side is one name or `_`, and a body.
* **AST-124** The variable of a `for` may carry a type, which stands in a type position: `for i : int in 0..<n:`.
* **AST-55** No other shape of `for` exists, a pattern on the left side among them, and one is a normal error.
* **AST-56** A `while` takes exactly one condition and a body.

```sgl
for i in 0..<count:
    total += weight i

for i : int in 0..<count:
    total += weight i

for _ in 0..<bounces:
    trace_next()

while not done => step()

@unroll
for tap in 0..<4:
    blur += sample_tap tap
```

### `assert` and `print`

* **AST-57** `assert` takes one condition, or one condition and one message, and nothing more.
* **AST-58** `print` takes exactly one message.
* **AST-59** Any other number of arguments is a normal error.

```sgl
assert count >= 0
assert count < limit, "count $count exceeds $limit"
print "total: $total"
```

`print` takes one message, so the AST reports a normal error for the second argument; it is written with interpolation.

```sgl sketch
print "total:", total
```

### Expression statements

* **AST-60** An expression statement has an effect when it is a paren or a juxtaposition `call`, a jump, a `case`, a `loop`, a `with_bindings` or an `invalid`.
* **AST-61** Any other expression statement is the warning `no-effect`, and it is still read: an infix or a prefix `call`, a name, a literal, a `member`, a `tuple`.
* **AST-114** The last statement of a block is no exception to AST-61, since a block has no implicit value ([AST-106](#value-blocks-and-yield)).
* **AST-140** Inside a `test` body, and not inside a function nested in one, AST-61 does not report: a line of type `bool` is a check there, and only the check pass knows a line's type.

```sgl
fun update(state: particle):
    advance state
    state.bounds.clamp()
    case state.kind:
        .dead => return
        _ => emit state
```

The lines `state.age` and `state.age + 1` below compute a value and drop it, so the AST reports `no-effect` for each, the last line of the block too.

```sgl sketch
fun update(state: particle):
    state.age
    state.age + 1
```

## Declarations

* **AST-62** A declaration is one row of the table below, and the table says where it may stand.
* **AST-63** A declaration in a place its row does not allow is a normal error, and it is still read.

| declaration | source | at file level | inside a function body |
|---|---|---|---|
| module | `module name` | yes | no |
| import | `use module`, `use module as name` | yes | yes |
| function | `fun` and a [signature](#functions) | yes | yes |
| struct | `struct name:` and a block of [members](#members) | yes | yes |
| enum | `enum name:` and a block of members | yes | yes |
| alias | `type name = expression`, with the expression in a type position | yes | yes |
| constant | `const name = expression`, `const name : type = expression` | yes | yes |
| binding | `binding name:` and a block of members | yes | yes |
| binding composition | `binding name = other`, `binding name = (a, b)` | yes | yes |
| sampler | `sampler name:` and a block of settings | yes | no |
| pipeline | `pipeline name:` and a block of settings, or `pipeline name = (a, b)` | yes | no |
| notation | `notation a => b` | yes | yes |
| test | `test:` and a block, or `test expression` | yes | yes |
| `let` | see [statements](#let-and-assignment) | no | yes |

* **AST-64** A file holds at most one `module` declaration, and it stands before every other declaration of the file.
* **AST-65** The type of a constant stands in a type position.
* **AST-138** `test expression` reads as a `test` whose block holds the one expression statement `expression`; a `test` with both, or with neither, is a normal error.
* **AST-139** A `test` stands in a struct and an enum as well, and inside another `test` is `declaration-not-allowed-here`.
  No jump crosses a `test`'s body, so a `return` in it is `jump-without-target`.

```sgl
fun square(x: float) -> float => x * x

test square 3.0 == 9.0

test:
    let x = 10
    x * x > 50
```

```sgl
module example

use brdf_library as brdf
notation \phi => φ

type color = vec3
const pi = 3.14159
const max_lights : int = 16

fun falloff(d: float) -> float:
    use math_library
    const radius = 4.0
    fun scaled(x: float) => x / radius
    return 1 - scaled d
```

### Functions

* **AST-66** A **signature** is what follows `fun`: a name, then type parameters `[…]`, parameters `(…)` and bindings `{…}`, each fused to what is before it, each at most once, and in this order.
* **AST-142** The name of a signature may be a type's name, a DOT and a name, `fun ray.inverted`: that function is an **extension**, and the type it names is its **extended type**.
* **AST-67** The parameters are mandatory, and they may be empty: `fun f()` ([why](why/ast.md#ast-67)).
* **AST-143** An extension without parameters is a property of its extended type, as AST-81 reads one: `fun ray.inverted => …` ([why](why/ast.md#ast-143)).
  It may carry `-> type` before its body, and type parameters or bindings there are `missing-parameter-list`.
* **AST-68** A **parameter** is a [field](#members), and a type parameter is a parameter whose type may be left out.
* **AST-144** A parameter or a field whose name is a leading-dot form, `.level: float`, is **named-only**: the AST records the mark, and the name without its dot.
  A named-only mark on a binding member, on a member of a `struct_type` or on `self` is the normal error `named-only-not-allowed-here`, and the member is still read.
* **AST-69** An element of the bindings is a **binding entry**, and the AST keeps it as an expression.
* **AST-70** After the signature stands an optional `->` with the return type, which is a type position.
* **AST-71** Then stands at most one body: `=>` and an expression, or a block.
* **AST-72** A function without a body is a valid node, a function that is **signature-only**, and a later phase judges it.

```sgl
fun pi_half() => 1.5708

fun sum[T](values: span[T]) -> T:
    return fold(values)

fun make_mvp(model: mat4){frame} => frame.proj * frame.view * model

fun sample_sky(dir: vec3){sky} -> vec3

@pixel fun my_ps(
    pos: hpos4
    uv: vec2
){frame, instance} -> pixel:
    return shade(pos, uv)
```

| source | reads as |
|---|---|
| `fun pi_half() => 1.5708` | no parameters, no return type, the body `1.5708` |
| `fun sum[T](values: span[T]) -> T:` | the type parameter `T`, one parameter, the return type `T`, a block |
| `fun make_mvp(model: mat4){frame} => …` | one parameter, the binding entry `frame`, an expression body |
| `fun sample_sky(dir: vec3){sky} -> vec3` | signature-only |

The signature below has no parameters, so the AST reports a normal error; it is written `fun half_pi() => 1.5708`.

```sgl sketch
fun half_pi => 1.5708
```

### Bindings and samplers

* **AST-73** A `binding` with a block declares a binding group by its members.
* **AST-74** A `binding` with `=` is the **composition short form**: its right side is one name or a round list of names, and it composes the bindings they name ([why](why/ast.md#ast-74)).
* **AST-75** A `binding` may stand inside a function body, and its properties may use the local variables declared above it ([why](why/ast.md#ast-75)).
* **AST-76** The block of a `sampler` holds **settings**, one `name = value` per line, and each reads as a named argument.

```sgl
binding frame:
    view: mat4
    proj: mat4
    fancy_sky: texture_cube

binding timing:
    time: float

binding lit = (frame, timing)

sampler bilinear:
    filter = .linear
    address = .clamp

fun shade_sky(v: basic_vertex){frame} -> vec3:
    let t = v.pos.x
    binding timing:
        time => t + 1.5
    binding sky:
        skymap => frame.fancy_sky
    return sky_library.sample_sky v.normal
```

### Pipelines

* **AST-131** A `pipeline` with a block declares a pipeline by its **settings**, one `path = value` per line.
  The path is a name or a member chain, and the value is an expression.
* **AST-132** The name of a `pipeline` is optional, so `pipeline:` is a pipeline without one ([pipelines](../pipelines.md) names it).
* **AST-133** A `pipeline` with `=` is the **short form**: its right side is a round list, whose elements are the pipeline's entry points.
  A block under the list holds settings, as AST-131's block does; the block hangs off the list, the rightmost form of the line (FORM-34).
* **AST-134** A setting whose left side is neither a name nor a member chain is the normal error `expected-name`, and a line that is no `=` is `expected-member`.
  Both are still read.

```sgl
pipeline:
    vertex = main_vs
    pixel = main_ps
    cull = .back
    color_targets.albedo.blend = .alpha

pipeline shadow = (shadow_vs, shadow_ps)
```

| source | reads as |
|---|---|
| `pipeline:` | a pipeline without a name |
| `color_targets.albedo.blend = .alpha` | a setting whose path is a member chain |
| `pipeline shadow = (shadow_vs, shadow_ps)` | the short form, with two entry points |

## Members

* **AST-77** A **member** is a statement of the block of a `struct`, an `enum` or a `binding`, or an element of a `struct_type`.
* **AST-78** The shape of a member decides what it reads as, by the table below.

| source | reads as |
|---|---|
| `name: type` or `.name: type`, with an optional `= default` | a **field** |
| `name => expression` | a **property** |
| `fun` and a signature | a **method** |
| one bare name, or `name = value`, inside an `enum` | a **case** |
| any other declaration, with its keyword | a nested declaration |

* **AST-79** A field is one kind of node for struct fields, binding members, parameters and the members of a `struct_type`: a name, a type, an optional default and attributes.
* **AST-80** The type of a field stands in a type position.
* **AST-81** A property is read-only and has no parameter list, and its body is an expression or, after `=>:`, a value block.
* **AST-82** A method starts with `fun`, and a property does not ([why](why/ast.md#ast-82)).
* **AST-83** A method whose first parameter is `self` or `mut self`, written without a type, is an instance method, and that parameter is its **receiver**.
* **AST-84** A method without a receiver is static.
* **AST-85** The **owner** of a member is the declaration or the `struct_type` it stands in, and the owner decides which members are allowed.

| owner | field | default of a field | property | method | case | nested declaration |
|---|---|---|---|---|---|---|
| `struct` | yes | yes | yes | yes | no | yes |
| `enum` | no | | yes | yes | yes | yes |
| `binding` | yes | no | yes | no | no | no |
| `struct_type` | yes | yes | no | no | no | no |

* **AST-86** A member that its owner does not allow is a normal error, and it is still read.
* **AST-136** A `sampler` declaration stands at file scope or in a `binding`, and anywhere else is `declaration-not-allowed-here`; in a binding it is a member, the group's static sampler.
* **AST-125** A `struct` line without a block is an **opaque struct**: it has no member that can be named, which a block without members does not say ([why](why/ast.md#ast-125)).
* **AST-126** The AST accepts an opaque struct wherever a `struct` stands, and a later phase allows it for a small set of `@builtin` types only.
* **AST-115** A case may carry a value, which is an expression: `red = 1`.
* **AST-116** The default of a field may name the fields that stand before it, and the AST checks nothing about it ([why](why/ast.md#ast-116)).

```sgl
struct particle:
    pos: pos3
    velocity: vec3
    age: float = 0.0
    lifetime: float = age + 10.0
    speed => self.velocity.length
    energy =>:
        let v = self.speed
        yield 0.5 * v * v
    fun advanced(self, dt: float) => particle(self.pos + self.velocity * dt, self.velocity, self.age + dt)
    fun reset(mut self):
        self.velocity = vec3(0, 0, 0)
        self.age = 0.0
    fun at_rest(pos: pos3) => particle(pos, vec3(0, 0, 0))

enum light_kind:
    point
    spot
    sun
    is_local => self != .sun
    fun fallback() => light_kind.point

enum channel_mask:
    red = 1
    green = 2
    blue = 4
    all = 7
```

| source | reads as |
|---|---|
| `age: float = 0.0` | a field with a default |
| `lifetime: float = age + 10.0` | a field whose default names the field `age` before it |
| `energy =>:` | a property whose body is a value block |
| `speed => self.velocity.length` | a property |
| `fun advanced(self, dt: float) => …` | an instance method whose receiver is read-only |
| `fun reset(mut self):` | an instance method whose receiver is mutable |
| `fun at_rest(pos: pos3) => …` | a static method |
| `sun` | a case |
| `blue = 4` | a case with the value `4` |

A `binding` allows no method, so the AST reports a normal error for `fun inverse_view`.

```sgl sketch
binding frame:
    view: mat4
    fun inverse_view() => inverse view
```

## Attributes

* **AST-87** The set of attributes is open: the AST keeps every attribute on the node it is written on, and it judges none by its name ([why](why/ast.md#ast-87)).
* **AST-88** An attribute in the AST is the span of its name and its arguments, each read as an argument by AST-23: positional, named or a splat.
* **AST-89** An attribute may stand on a declaration, a member, a parameter, a binding entry, a statement and an expression in a type position.
* **AST-90** An attribute on any other expression is a normal error, and the attribute is kept.
* **AST-91** A later phase validates every attribute against its name and the kind of its node ([CHK-38 and CHK-39](../semantics/checking.md#builtins-and-the-prelude)).
* **AST-92** One name may mean different things on different kinds of node: `@vertex struct` is a vertex, and `@vertex fun` is the vertex stage.

```sgl
@vertex struct basic_vertex:
    pos: pos3
    @range(0, 1) weight: float
    @slider(min = 0, max = 4, ..ui_defaults) gain: float

@vertex fun my_vs(@builtin id: uint, v: basic_vertex){@slot(0) frame} -> @position hpos4:
    @unroll
    for i in 0..<4:
        accumulate i
    return project(id, v)
```

| attribute | stands on |
|---|---|
| `@vertex`, first | the declaration `struct basic_vertex` |
| `@range(0, 1)` | the member `weight`, with two positional arguments |
| `@slider(…)` | the member `gain`, with two named arguments and a splat |
| `@vertex`, second | the declaration `fun my_vs` |
| `@builtin` | the parameter `id` |
| `@slot(0)` | the binding entry `frame` |
| `@position` | the return type, an expression in a type position |
| `@unroll` | the statement `for` |

`@fast` stands on an argument of a call, so the AST reports a normal error.

```sgl sketch
let d = dot(@fast n, l)
```

## What the AST records and does not check

* **AST-93** The AST records the binding entries of every signature, and it does not check that a binding is declared, used or satisfied.
* **AST-94** The AST records the declarations and statements of every block and of the file in source order.
* **AST-95** Which constructs open a scope, and whether a name is visible before its declaration there, is decided by name lookup.
* **AST-96** The AST records every type position, and it does not check that the expression there is a type.

The ideas these records serve are in the [incubator](../incubator/_index.md):
[binding effects](../incubator/binding-effects.md), [scopes](../incubator/scopes.md) and [types as values](../incubator/types-as-values.md).

## Open

* `yield` is experimental: its rules, the target rules above all, may change ([why](why/ast.md#ast-106)).
* The half-open range `a..`, as in `for i in 1..:`; the postfix `..` is reserved for it ([OP-31](operators.md#the-operator-table)).
* What a binding entry other than a bare name means: `name as other` and `name = other` are kept and have no meaning yet.
* Whether members are in scope unqualified inside a property or a method, so that `length` reads `x` and not `self.x`.
* Whether `true` and `false` are keywords or constants of the prelude; until then the AST reads them as ordinary `name` nodes.
* A pattern as the variable of a `for`, which waits for custom iterators.
* Whether single-quoted and backquoted literals are reported until their semantics exist.
* Partial assignment, `x .= {.1 = 8}`, is an idea only.
