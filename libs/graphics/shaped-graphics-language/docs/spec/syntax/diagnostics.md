# Diagnostics

Every phase is total: any input gives a tree, and what is wrong with the input is a list of diagnostics beside it.
This file defines what a diagnostic is and lists every kind the syntax phases report, the AST phase among them.
Back to the [phases](_index.md).

## Rules

* **DIAG-1** A diagnostic has a **kind**, a byte span in the source, and a message.
* **DIAG-2** A kind has a stable name in kebab-case, such as `undelimited-string`.
* **DIAG-3** A kind has a default severity: warning, normal error or fatal error.
* **DIAG-4** A **normal error** is a construct the language forbids and for which one reasonable reading exists; the tree carries that reading.
* **DIAG-5** A development build continues after a normal error, and a production build treats it as an error.
* **DIAG-6** A **fatal error** is a construct for which no reasonable reading exists.
* **DIAG-7** A **warning** is a construct the language allows and a reader is likely to misread; a lint is a warning.
* **DIAG-8** A diagnostic never changes the tree of a line outside the reach of [LINE-23](line-tree.md#locality).
* **DIAG-9** The syntax phases define no fatal kind: every kind below is a normal error unless the table says otherwise.

## Catalogue

The kinds of the phases up to the form tree.
Each kind names the rule that reports it, and has one example below the table.

| kind | reported by | the reading that is kept |
|---|---|---|
| `tab-in-indentation` | [LINE-9](line-tree.md#indentation) | the tab advances to the next multiple of 4 |
| `unknown-character` | [TOK-29](tokens.md#unknown-bytes) | the bytes are one error token |
| `double-colon` | [TOK-19](tokens.md#punctuation) | the token stays a DOUBLE_COLON |
| `unknown-escape` | [STR-4](strings-and-comments.md#one-line-strings) | the character after the backslash |
| `undelimited-string` | [STR-6](strings-and-comments.md#one-line-strings) | the string closes at the end of the line |
| `missing-string-end` | [STR-12](strings-and-comments.md#multi-line-strings) | the string closes where its children end |
| `underindented-string-content` | [STR-18](strings-and-comments.md#content) | the line loses the indentation it has |
| `stray-dollar` | [STR-28](strings-and-comments.md#interpolation) | the `$` stands for itself |
| `reserved-string-opener` | [STR-30](strings-and-comments.md#reserved-openers) | an opening quote |
| `empty-block` | [GRP-11](groups.md#the-block-colon) | an empty composite |
| `missing-closer` | [GRP-21](groups.md#closing) | everything open closes where the children end |
| `unmatched-closer` | [GRP-22](groups.md#closing) | the closer is skipped |
| `nested-continuation` | [GRP-26](groups.md#continuation-lines) | the tokens are appended in source order |
| `spaced-attribute-arguments`, a warning | [GRP-29](groups.md#attributes) | an attribute without arguments, then a paren literal |
| `unattached-attribute` | [GRP-38](groups.md#attributes) | the attribute attaches to nothing |
| `misplaced-attribute` | [GRP-39](groups.md#attributes) | the attribute attaches to the top-level form of the line |
| `semicolon-in-parens` | [FORM-17](forms.md#paren-literals) | the `;` separates elements as a comma does |
| `expected-expression` | [FORM-40](forms.md#broken-runs) | a missing form stands for the operand |
| `unexpected-token` | [FORM-41](forms.md#broken-runs) | an error form that keeps the group tokens in source order |
| `underscore-in-number` | [NUM-12](numbers.md#inside-the-symbols) | each `_` reads as `'` |
| `malformed-number` | [NUM-19](numbers.md#inside-the-symbols) | a number literal without a value |
| `bare-range` | [OP-8](operators.md#the-operator-table) | a range operator |
| `unknown-operator` | [OP-10](operators.md#the-operator-table) | an operator of the level its spelling gives |
| `reserved-operator` | [OP-12](operators.md#the-operator-table) | an operator of the level its spelling gives |
| `mixed-operators` | [OP-15](operators.md#mixing) | the run reads left to right |
| `misplaced-not` | [OP-16](operators.md#mixing) | the `not` binds to its operand |
| `non-monotone-comparison` | [OP-20](operators.md#mixing) | the pairwise comparisons |
| `chained-range` | [OP-21](operators.md#mixing) | the run reads left to right |
| `operator-needs-spaces` | [OP-22](operators.md#spacing) and [OP-29](operators.md#spacing) | an infix operator |

## Examples

`tab-in-indentation`: the second line is indented with a tab.

```sgl error
if ready:
	step()
```

`unknown-character`: a `$` outside a string.

```sgl error
let x = $ + 1
```

`double-colon`: written `color.red`.

```sgl error
let c = color::red
```

`unknown-escape`: `\q` is no escape.

```sgl error
print "a \q b"
```

`undelimited-string`: the quote does not close on its line.

```sgl error
print "hello
```

`missing-string-end`: the sibling does not start with the closing quote.

```sgl error
print "
    hello
print "next"
```

`underindented-string-content`: the content line has 2 columns where 4 are taken off.

```sgl error
let text = "
  short
"
```

`stray-dollar`: a `$` that starts no interpolation.

```sgl error
print "cost in $ is unknown"
```

`reserved-string-opener`: `"""` is reserved.

```sgl error
let raw = """
    text
"""
```

`empty-block`: a block colon and no children.

```sgl error
if ready:
step()
```

`missing-closer`: the sibling does not start with `)`.

```sgl error
let v = vec2(
    1.0
    2.0
let w = 1
```

`unmatched-closer`: no `]` is open.

```sgl error
let v = a + b]
```

`nested-continuation`: a continuation line continues again.

```sgl error
let sum = a
    + b
        + c
```

`spaced-attribute-arguments`: the round group is not fused to the attribute.

```sgl error
@range (0, 1)
const bias = 0.5
```

`unattached-attribute`: no line follows.

```sgl error
fun f():
    return
    @inline
```

`misplaced-attribute`: the attribute is in the middle of the line.

```sgl error
fun @vertex main():
    return
```

`semicolon-in-parens`: a `;` inside a round group.

```sgl error
let p = (1; 2)
```

`expected-expression`: the `+` has no right side.

```sgl error
let sum = a +
```

`unexpected-token`: the run starts with no keyword, so its comma has no place.

```sgl error
a , b
```

`underscore-in-number`: written `1'000`.

```sgl error
let big = 1_000
```

`malformed-number`: a digit starts a symbol that is no number.

```sgl error
let x = 10a7
```

`bare-range`: `..` says nothing about its end.

```sgl error
let r = 0 .. 4
```

`unknown-operator`: the language defines no `<>`.

```sgl error
let d = a <> b
```

`reserved-operator`: written `not ready`.

```sgl error
let waiting = !ready
```

`mixed-operators`: `and` and `or` in one run.

```sgl error
let either = a and b or c
```

`misplaced-not`: the `not` is not on the last operand.

```sgl error
let odd = not a and b
```

`non-monotone-comparison`: the chain goes up and then down.

```sgl error
let peak = a < b > c
```

`chained-range`: two range operators in one run.

```sgl error
let r = a ..< b ..< c
```

`operator-needs-spaces`: the `-` is fused on both sides.

```sgl error
let d = a-b
```

`operator-needs-spaces`: `->` owes the same spaces, and it is written `a -> b`.

```sgl error
let f = a->b
```

## The kinds of the AST phase

The [AST phase](ast.md) reports these kinds: the form tree is fine, and the language has no reading for the form in its place.
Each is a normal error unless its line says otherwise, and the node is kept as [AST-5](ast.md#the-phase) says.
An example of an AST kind is an `sgl sketch`, since examples are checked through the form tree only.

| kind | what it means |
|---|---|
| `expected-declaration` | a form at file level or in a function body position that is no declaration and no statement |
| `expected-member` | a line of a `struct`, `enum`, `binding` or `sampler` block, or an element of a `struct_type`, that is no member |
| `expected-case-arm` | a statement of a `case` block that is no `pattern => result` |
| `expected-name` | a declaration, an import or a named argument that has no identifier where its name stands |
| `reserved-name` | a declaration, a field, an enum case or a parameter named by a reserved name such as `void` ([AST-141](ast.md#atoms)) |
| `expected-pattern` | a `let` whose target is no name, no `_` and no round list of patterns |
| `expected-parameter` | an element of a parameter list, or the left side of an arrow lambda, that is no parameter |
| `expected-body` | a construct that needs a body and has none, or has `=` where `=>` or a block stands |
| `declaration-not-allowed-here` | a declaration in a place its row does not allow: a `let` at file level, a `sampler` in a function body |
| `misplaced-module` | a second `module`, or one that is not the first declaration of its file |
| `member-not-allowed-here` | a member its owner does not allow: a method in a `binding`, a field in an `enum`, a case in a `struct` |
| `default-not-allowed-here` | a default on a field whose owner allows none: a member of a `binding` |
| `missing-parameter-list` | a signature without `()` |
| `signature-out-of-order` | the lists of a signature in another order than `[…]`, `(…)`, `{…}` |
| `duplicate-signature-list` | a list of a signature that stands twice |
| `stray-else` | an `else` or an `else if` that pairs with no `if` |
| `mixed-struct-type` | a curly paren literal that holds both `name: type` and `name = value` elements |
| `misplaced-splat` | a splat that is not a whole element of a paren group |
| `misplaced-attribute-on-expression` | an attribute on an expression that stands in no type position |
| `statement-in-expression` | an assignment, a `let`, an `if` or a declaration where a value is expected |
| `unexpected-keyword` | keywords that head nothing together, such as `mut` without `let` |
| `too-many-arguments` | a keyword form that holds more expressions than it takes: `return a, b`, `continue x` |
| `for-takes-name-in-range` | a `for` that is not `for name in expression` or `for name : type in expression` |
| `assert-takes-condition-and-message` | an `assert` that is not one condition, or one condition and one message |
| `print-takes-one-message` | a `print` that is not exactly one message |
| `unsupported-syntax` | a spelling that is reserved and has no meaning yet: `f(x){…}`, and an expression that owns a block |
| `yield-in-function` | a `yield` that finds a `fun` body before any value block; it is written `return` |
| `yield-in-loop` | a `yield` with a `loop` between it and its value block; it is written `break value` |
| `return-in-lambda` | a `return` in the block of an arrow lambda; it is written `yield` |
| `redundant-return` | a `return` that is the whole one-line body of an arrow lambda; `x => return x` is written `x => x` |
| `jump-without-target` | a `yield` with neither a value block nor a `fun` around it, a `return` with no `fun` around it, a `break` or a `continue` with no loop around it |
| `expected-object-element` | an element of an object literal that is no name, no `name = value` and no splat |
| `redundant-yield`, a warning | a `yield` that is the whole one-line body of an arm, an arrow lambda or a property; the `=>` already hands the value on |
| `no-effect`, a warning | an expression statement that has no effect by [AST-60](ast.md#expression-statements) |

`expected-case-arm`: the second line of the block is no arm.

```sgl sketch
let area = case shape:
    .square => 1.0
    finish()
```

`missing-parameter-list`: written `fun half_pi() => 1.5708`.

```sgl sketch
fun half_pi => 1.5708
```

`signature-out-of-order`: the bindings stand before the parameters.

```sgl sketch
fun shade{frame}(n: vec3) -> vec3
```

`stray-else`: the `else` follows a `let`.

```sgl sketch
let x = 1
else:
    fail()
```

`member-not-allowed-here`: a `binding` allows no method.

```sgl sketch
binding frame:
    view: mat4
    fun inverse_view() => inverse view
```

`misplaced-splat`: the splat is an operand of `+`.

```sgl sketch
let sum = 1 + ..rest
```

`statement-in-expression`: a `let` where an argument is expected.

```sgl sketch
let total = sum(let x, 2)
```

`too-many-arguments`: `return` takes at most one value.

```sgl sketch
fun pair() -> (int, int):
    return 1, 2
```

`yield-in-loop`: the `loop` stands between the `yield` and the lambda block, and it is left with `break guess`.

```sgl sketch
let solve = start =>:
    let mut guess = start
    loop:
        guess = refine guess
        if converged guess => yield guess
```

`jump-without-target`: no `fun` is around the `return`, and no loop is around the `break`.

```sgl sketch
struct ray:
    dir: vec3
    inv_dir =>:
        return 1 / dir

fun finish():
    break
```

`no-effect`: the sum is computed and dropped.

```sgl sketch
fun update(state: particle):
    state.age + 1
```

## Open

* The names of the two lints: a code line that aligns with no sibling, and whitespace after the quote that opens a multi-line string.
* How a project changes the severity of a kind.
* Whether `reserved-string-opener` and `reserved-operator` stay two kinds or become one kind `reserved-syntax`.
* Whether any syntax phase needs a fatal kind at all.
