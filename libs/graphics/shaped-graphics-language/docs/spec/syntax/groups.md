# Group tokens

The grouping phase turns the tokens of the line tree into **group tokens**.
It matches parentheses, folds lines into runs, and takes comments, whitespace and attributes out of the run.
The form parser then sees a short flat run, in which a parenthesis is one group token with children.
Back to the [phases](_index.md); the reasons are in [why/groups.md](why/groups.md).

## Group tokens

* **GRP-1** A group token is a token, or a **paren group**: an opener, its closer, and the elements between them as children.
* **GRP-2** The three kinds of paren group are round `( )`, square `[ ]` and curly `{ }`.
* **GRP-3** Comments and whitespace are not group tokens; they stay attached to the group tokens next to them, so nothing is lost.
* **GRP-4** Attributes are not group tokens; they are attached by the rules [below](#attributes).
* **GRP-5** A group token stores whether it is [fused](tokens.md#fused) ([why](why/groups.md#grp-5)).
* **GRP-6** The grouping phase parses nothing: every rule here is decided by token kinds and positions alone.

## How children are read

* **GRP-7** How the children of a line are read is decided by how the line ends ([why](why/groups.md#grp-7)).

| the line ends | its children are |
|---|---|
| in the block colon | a **block**: each child is a statement of its own |
| with an open string | string content |
| with a paren group still open | **element lines** of the innermost open paren group |
| in any other way | **continuation lines**, folded into the run of the line |

* **GRP-8** The block colon wins over an open paren group: the children are a block, and the paren group is still closed by the next sibling ([why](why/groups.md#grp-8)).

```sgl
let total = base
    + offset
    + bias

if total > 0:
    print total

let v = vec3(
    1.0, 2.0
    3.0
)

apply(fun (x: int):
    print x
)
```

## The block colon

* **GRP-9** The **block colon** is a `:` that is the last token of its line, not counting a trailing comment and trailing attributes ([why](why/groups.md#grp-9)).
* **GRP-10** Every other `:` is ascription.
* **GRP-11** A block colon whose line has no children is the normal error `empty-block`.
* **GRP-12** A comment line as the only child is the intended empty body, and `empty-block` is not reported ([why](why/groups.md#grp-12)).
* **GRP-13** A continuation line may carry the block colon, and the block is then the children of that continuation line.

```sgl
while not done: // the block colon, then a trailing comment
    step()

fun on_resize(size: vec2):
    // nothing to do yet
```

The line below has a block colon and no children, so it reports `empty-block`.

```sgl error
if ready:
let x = 1
```

A condition over two lines, first with GRP-13 and then in the form the formatter writes, which is read by [GRP-40](#element-lines).

```sgl
if width > 0
    and height > 0:
        resize()

if (
    width > 0
    and height > 0
):
    resize()
```

## Element lines

* **GRP-14** Each child line of an open paren group is a complete run of elements, separated by commas, unless GRP-40 applies to it.
* **GRP-15** No element spans two sibling lines except by GRP-40, so a comma at the end of an element line is optional ([why](why/groups.md#grp-15)).
* **GRP-16** An element line has children of its own by GRP-7: it may continue, leave a paren group open, or carry a block.
* **GRP-40** An element line whose first token is an infix operator continues the last element of the element line above it ([why](why/groups.md#grp-40)).
* **GRP-41** For GRP-40 an infix operator is `and`, `or`, `as`, `in`, `:`, `->`, `=>`, or an operator token that is not fused to the token after it.
* **GRP-42** An element line that starts with a prefix operator, which is fused to the token after it, starts a new element.

```sgl
let m = mat2(
    1.0, 0.0,
    0.0, 1.0
)

let p = make_point(
    x = left
        + margin
    y = top
)

if (
    width > 0
    and height > 0
    and width + height
    + margin > 100
):
    resize()

let offsets = (
    a
    - b
    -c
)
```

| element lines | elements |
|---|---|
| the four lines of the condition | one: `width > 0 and height > 0 and width + height + margin > 100` |
| `a`, `- b`, `-c` | two: `a - b` and `-c` |

## Closing

* **GRP-17** A line that ends with something open obliges its next sibling to start with a closer ([why](why/groups.md#grp-17)).
* **GRP-18** The expected closer is the closing quote when a string is open, and otherwise the closer of the innermost open paren group.
* **GRP-19** What is still open after that first token stays open, and the sibling may end with something open again and oblige its own next sibling.
* **GRP-20** When the sibling does not start with the expected closer, or there is no sibling, everything open closes where the children end.
* **GRP-21** GRP-20 reports one normal error, `missing-string-end` for a string and `missing-closer` otherwise, and the sibling is read as if nothing were open.
* **GRP-22** A closer that matches nothing open on its line and is not the expected first token is the normal error `unmatched-closer`, and it is skipped.
* **GRP-23** A paren group never closes in a child line.

```sgl
let items = [foo(
    1
    2
) + bar(
    3
)]
```

| line | starts with | ends with open |
|---|---|---|
| `let items = [foo(` | | `[` and `(` |
| `) + bar(` | `)`, the innermost | `[` and `(` |
| `)]` | `)`, the innermost | nothing |

Below, the `)` is a child line, so it matches nothing and reports `unmatched-closer`.
Then `let y = 1` is not a closer, which reports `missing-closer`.

```sgl error
let x = foo(
    1
    2
    )
let y = 1
```

## Continuation lines

* **GRP-24** The group tokens of a continuation line are appended to the run of its parent.
* **GRP-25** The first group token of a continuation line counts as fused to the token before it only when it is a DOT ([why](why/groups.md#grp-25)).
* **GRP-26** A continuation line whose own children are continuation lines is the normal error `nested-continuation`, and the tokens are still appended in source order ([why](why/groups.md#grp-26)).

```sgl
let shade = material
    .albedo
    .scale(0.5)
```

The third line below continues a continuation, so it reports `nested-continuation`.

```sgl error
print 10
    * 20
        + 10
    * 30
```

## Attributes

* **GRP-27** An **attribute** is a symbol that starts with `@`.
* **GRP-28** An attribute takes arguments only as a fused round group: `@range(0, 1)`.
* **GRP-29** A round group after whitespace is not the arguments of the attribute, and it is the warning `spaced-attribute-arguments`.
* **GRP-30** The arguments are grouped like any paren group, and the form parser reads them like any paren list ([FORM-42](forms.md#the-parser)).
* **GRP-31** Where an attribute attaches is decided by its position, by the first rule below that applies ([why](why/groups.md#grp-31)).

| rule | position | attaches to |
|---|---|---|
| **GRP-32** | directly after `->`, `:`, `=>`, `as` or `in` | that operand |
| **GRP-33** | inside a paren group | the element it appears in |
| **GRP-34** | on a line that holds only attributes | the next sibling line that does not hold only attributes |
| **GRP-35** | at the end of a line, with nothing but a comment after it | the top-level form of the line |
| **GRP-36** | anywhere else | the top-level form of the line |

* **GRP-37** Blank lines and comment lines may stand between a line of only attributes and the line it attaches to.
* **GRP-38** A line of only attributes with no line to attach to is the normal error `unattached-attribute`.
* **GRP-39** An attribute placed by GRP-36 that does not lead its line is the normal error `misplaced-attribute`.

```sgl
@range(0, 1)
// how strongly the fog tints

const fog_bias = 0.5

const fog_scale = 2.0 @range(0, 4)

@vertex fun main(@builtin id: uint) -> @position vec4:
    return project(id)
```

| attribute | attaches to | by |
|---|---|---|
| `@range(0, 1)` | the line `const fog_bias = 0.5` | GRP-34 |
| `@range(0, 4)` | the form `const fog_scale = 2.0` | GRP-35 |
| `@vertex` | the form `fun main …` | GRP-36 |
| `@builtin` | the element `id: uint` | GRP-33 |
| `@position` | the operand `vec4` | GRP-32 |

The attribute below sits in the middle of the line, so it reports `misplaced-attribute`.

```sgl error
fun @vertex main():
    return
```

## Open

* How an opener that is closed by a closer of another kind, as in `foo(]`, recovers.
* Whether a paren group left open at the end of a line inside an open string counts as open.
* Whether the attribute order of GRP-32 to GRP-36 holds when two rules apply to one attribute, for example a trailing attribute inside an element line.
