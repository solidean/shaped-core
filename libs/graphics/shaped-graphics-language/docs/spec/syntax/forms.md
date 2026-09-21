# Form tree

The form phase reads each run of [group tokens](groups.md) into a **form**.
The form tree is generic: it knows operators, parentheses, application and keywords, and nothing about what a declaration or a statement is.
Back to the [phases](_index.md); the reasons are in [why/forms.md](why/forms.md).

## The parser

* **FORM-1** The form parser reads one run of group tokens into one form.
* **FORM-2** The keyword table is data handed to the form parser, and it is the only thing the parser knows about the language ([why](why/forms.md#form-2)).
* **FORM-3** How tightly each construct binds is the precedence ladder in [operators.md](operators.md#the-precedence-ladder).
* **FORM-4** Every form may carry attributes, attached by the [grouping phase](groups.md#attributes).
* **FORM-42** The fused round group of an attribute is read like any paren group: its elements are forms, and `name = value` is a named argument, as in `@slider(min = 0)`.

| form | is |
|---|---|
| number literal | a number, assembled by [numbers.md](numbers.md) |
| quoted literal | a quote open, its body and interpolations, and a quote close |
| hash literal | a symbol that starts with `#` |
| paren literal | a paren group that is not applied to anything: round, square or curly |
| identifier | a symbol that is none of the other forms |
| wildcard | the symbol `_` |
| leading-dot form | a DOT that is not fused, and the name fused to it |
| member access | a form, a fused DOT and a name |
| call | a form and a paren group fused to it |
| application | a head and its inline arguments |
| prefix or postfix operator | an operator and its one operand |
| operator run | the operands and operators of one precedence level |
| keyword form | leading keywords, arguments and an optional block |
| composite | the forms of a block, in order |
| missing form | the place of an operand that is not there |
| error form | the group tokens of a run that could not be read |

## Atoms

* **FORM-5** A **keyword** is a symbol that the keyword table lists ([keywords](../keywords.md)).
* **FORM-6** A symbol that starts with a digit is a number literal, or a part of one.
* **FORM-7** A symbol that starts with `#` is a **hash literal**.
* **FORM-8** A symbol that is none of these, not an attribute and not the wildcard is an **identifier**.
* **FORM-9** An identifier keeps its spelling as written; [notation](../notation.md) applies later.

```sgl
let tint = #ff00bb
let _ = compute(tint)
```

## Paren literals

* **FORM-10** A paren group is **applied** when it is fused and the group token before it is a symbol, a quoted literal or a paren group.
* **FORM-11** A paren group that is not applied is a **paren literal**.
* **FORM-12** The elements of a paren group are separated by commas.
* **FORM-13** A comma after the last element is allowed.
* **FORM-14** A comma at the end of an element line is optional ([GRP-15](groups.md#element-lines)).
* **FORM-15** A named argument is written `name = value`, in every position and in all three kinds of paren group ([why](why/forms.md#form-15)).
* **FORM-16** A `:` inside a paren group is always ascription.
* **FORM-17** A `;` inside a paren group is reserved, and it is the normal error `semicolon-in-parens`.

```sgl
let a = (1, 2, 3)
let b = [1, 2, 3,]
let c = {x = 1, y = 2}
let d = (
    1,
    2
    3
)
let e = make_light(color = #fff, 2.0, falloff = 0.5)
```

## Postfix forms

* **FORM-18** An applied paren group makes a **call** of the form before it.
* **FORM-19** Any number of applied paren groups may follow one another, of any kind.
* **FORM-20** A fused DOT followed by a fused symbol makes a **member access** of the form before it ([why](why/forms.md#form-20)).
* **FORM-21** The name of a member access may be digits: `t.0`.
* **FORM-22** Type arguments are an applied square group, the same form as a subscript ([why](why/forms.md#form-22)).

```sgl
let s = scene.lights[2].color
let first = pair.0
fun sum[T](values: span[T]) -> T:
    return fold(values)
let samples : array[vec3, 16]
```

| source | reads as |
|---|---|
| `f(x).y[i]` | call with `[i]` of: member `y` of: call with `(x)` of `f` |
| `f(1)(2)[3]` | three calls in a row; a later phase may reject it |
| `buffer[float]` | call with `[float]` of `buffer`; the AST reads it as `index`, type arguments or a subscript ([AST-14](ast.md#atoms)) |

## Leading-dot forms

* **FORM-23** A DOT that is not fused, with a symbol fused to it, is a **leading-dot form** ([why](why/forms.md#form-23)).
* **FORM-24** The name of a leading-dot form may be digits.
* **FORM-25** A leading-dot form may be the name of a named argument.

```sgl
let kind = .point
let t = (.0 = 7, .1 = 8)
```

The first DOT of a continuation line is fused by [GRP-25](groups.md#continuation-lines), so it is member access there.

## Application

* **FORM-26** A head followed by operands that are not fused to it is an **application**: `f a b` reads as `f(a, b)` ([why](why/forms.md#form-26)).
* **FORM-27** Application binds tighter than every infix operator.
* **FORM-28** An inline argument is a prefix operator form, a postfix form or an atom; anything looser needs parentheses.
* **FORM-29** `f(x)` is a call with a paren group, and `f (x)` is `f` applied to a paren literal.

```sgl
let v = cross a b
let x = length v
let y = foo a + bar b
let z = scale (a + b) 2.0
```

| source | reads as |
|---|---|
| `cross a b` | `cross(a, b)` |
| `foo a + bar b` | `foo(a) + bar(b)` |
| `foo -a` | `foo(-a)` |
| `foo - a` | `foo` minus `a` |
| `f (x, y)` | `f` applied to one tuple |

## Keyword forms

* **FORM-30** A run that starts with one or more keywords is a **keyword form**, and those keywords are its head.
* **FORM-31** The arguments of a keyword form are whole expressions, of level 5 and tighter, separated by commas ([why](why/forms.md#form-31)).
* **FORM-32** A keyword in operand position starts a nested keyword form that takes the rest of the run.
* **FORM-33** The word operators `and`, `or`, `not`, `as` and `in` never start a keyword form.
* **FORM-34** A block belongs to the rightmost form of its line.
* **FORM-35** After a `=>` with nothing to its right, the block is the right side of that `=>`.

```sgl
let mut count = 0
print "total: $count"
assert count == 0, "count starts at zero"
if count > 0:
    return count + 2
else if count < 0:
    return 0
```

| source | reads as |
|---|---|
| `let mut count = 0` | assignment of `0` to: keyword form `let mut` with `count` |
| `let x : int = 10` | assignment of `10` to: keyword form `let` with `x : int` |
| `assert count == 0, "zero"` | keyword form `assert` with two arguments |
| `fun f(x: int) -> int => x + 1` | `=>` of: keyword form `fun` with `f(x: int) -> int`, and: `x + 1` |
| `if done => return` | `=>` of: keyword form `if` with `done`, and: keyword form `return` |
| `yield r * r * pi` | keyword form `yield` with `r * r * pi` |
| `let g = fun (x) => x + 1` | assignment to `let g` of: `=>` of: keyword form `fun` with `(x)`, and: `x + 1` |
| `x : (int) -> int` | `:` of `x` and: `->` of `(int)` and `int` |
| `let k = case kind:` | assignment to `let k` of: keyword form `case` with `kind` and the block |
| `for i in range:` | keyword form `for` with the one argument `i in range`, and the block |

A block as the right side of `=>`, which yields its value with `yield` ([AST-107](ast.md#value-blocks-and-yield)).

```sgl
let area = case shape:
    .square => shape.side * shape.side
    .circle =>:
        let r = shape.radius
        yield r * r * pi
```

## Composites and sequences

* **FORM-36** A block is a **composite**: the forms of its child statements, in order.
* **FORM-37** A file is the composite of its top-level lines.
* **FORM-38** Every child statement of a block is parsed on its own, so an error in one never changes another.
* **FORM-39** A `;` outside a paren group separates two forms on one line.

```sgl
let a = 1; let b = 2
```

## Broken runs

* **FORM-40** Where an operand is expected and nothing usable stands, the tree holds a **missing form** in its place, and it is the normal error `expected-expression`.
* **FORM-41** A group token that the ladder cannot place in its run is the normal error `unexpected-token`, and the run is an error form that keeps its group tokens in source order.

| source | reads as |
|---|---|
| `let sum = a +` | assignment to `let sum` of: `a` plus a missing form |
| `a , b` | an error form of `a`, `,` and `b`; the comma and `b` are each reported |
| `assert a, b` | keyword form `assert` with two arguments |

The right side of `+` is not there, so the line below reports `expected-expression`.

```sgl error
let sum = a +
```

The run below starts with no keyword, so its comma has no place and it reports `unexpected-token`.

```sgl error
a , b
```

## Open

* How a DOT that is fused on neither side, as in `a . b`, or only on the left, as in `a. b`, is read.
* Whether an empty element, as in `(1,, 2)`, is an error.
* Whether juxtaposition calls stay in the language: they are accepted from the start as an experiment.
* Partial assignment with `.=`.
