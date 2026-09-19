# Why: AST

The reasons behind the rules of [ast.md](../ast.md).
Nothing here is normative.

The phase as a whole exists so that the syntactic half of the compiler stays a function of one file's bytes.
A highlighter or a formatter stops after the forms, and everything up to and including the AST can be cached per file and computed in parallel.

## AST-2

Looking a name up needs the other files of the program, the prelude and every `use`, so a phase that does it is no longer a function of one file.
The price is that the AST cannot know whether `vec3` is a type or `frame` is a binding.
So it does not pretend to: `buffer[float]` and `weights[3]` are one node, `index`, and the phase that knows what `buffer` is picks the reading.
Guessing would be worse than waiting, because a guess by spelling, such as "a name that ends in a digit is a type", is wrong in ways no diagnostic can explain.
Keeping both readings open costs one node kind and saves a second tree.

## AST-7

[Notation](../../notation.md) replaces parts of a name, `\phi` by `φ`, and it applies during name lookup.
Two spellings that a notation makes equal must intern to one identifier, so interning before notation would intern the wrong thing.
The phase that applies notation is therefore the one that interns, and until then a name is where it stands in the source.
That is also all a formatter or a rename tool wants from it.

## AST-9

This is the designer's bet: SGL never needs a "type" kind beside the "value" kind.
Everything is a value, and some positions, the right side of `:`, `->` and `as`, ask for a value that reads as a type.
`(int, int)` is a tuple of two types, and in a type position it reads as the type of a pair.
A function may return a type, since every function inlines and the call has reduced to a plain type before any code survives.
For the AST this removes a whole grammar: there is no type syntax to parse, no place where `a[b]` must be decided early, and no second set of nodes.
It only marks the positions, and the partial evaluator of a later phase does the rest.
The idea, and what could still end the bet, is in [types as values](../../incubator/types-as-values.md).

## AST-13

`self` has to mean the receiver, so it cannot be an ordinary name that a `let` may take.
It cannot be a keyword of the form parser either.
A keyword that leads a line starts a keyword form, so `self.x = 0` would read as the keyword `self` with an argument, and not as an assignment.
A reserved name gets both: the form parser sees an identifier, and the AST gives that identifier its one meaning.

## AST-14

Square brackets were chosen for type arguments so that parsing needs no lookup ([FORM-22](forms.md#form-22)).
The same holds one phase later.
`texture2d[rgba8]` applies a type, `weights[3]` reads an element, and `table[float]` could be either until `table` is known.
One `index` node with arguments serves both, and resolution relabels nothing: it only decides how the node is evaluated.

## AST-15

An operator is a function with a special name, so `a + b` and `add(a, b)` are the same thing to the type checker, to overload resolution and to inlining.
With one `call` node those phases have one case and not four.
The spelling is kept, because a formatter must write back what the author wrote, and a diagnostic should quote `a + b` and not `+(a, b)`.
`and` and `or` fit in as well: they type as normal functions, and the flag reminds the emitter that the right operand is evaluated only when needed.

## AST-21

`0 <= i < n` means `0 <= i and i < n` with `i` evaluated once ([OP-17](../operators.md#mixing)).
Expanding it in the AST would put `i` into the tree twice, and a later phase would have to find out that the two are one evaluation.
When `i` is `next_sample()` that is the difference between a correct program and a wrong one.
So the chain stays a node with each operand once, and lowering it is left to the phase that can introduce a temporary.
A single comparison has no middle operand, so it needs none of this and is a plain call.

## AST-28

The splat spreads the elements of a value into a list: `(..normal, 0)` is a `vec4` worth of elements.
It is a prefix for three reasons.
The postfix spelling `a..` stays free for a half-open range, `for i in 1..:` with a `break` inside, which reads exactly like the ranges that have an end.
`{..defaults, roughness = 0.5}` is the record-update idiom of several languages, and it is recognized at a glance.
And a prefix cannot hide: at the end of a long element, `compute_tangent_frame(n, uv).normal..`, two dots are easy to miss, and at its start they are the first thing read.
It is limited to a whole element because spreading only means something inside a list.

## AST-40

A jump has no value, which is exactly what lets it stand wherever a value is expected: its type is the bottom type.
That makes `_ => return false` an ordinary `case` arm.
It is what "parse, don't validate" looks like at the top of a function: bind the good case and leave on the bad one, in one expression.

```sgl
fun shade(hit: hit_info) -> vec3:
    let material = case hit.material:
        .none => return black
        m => m
    return material.albedo
```

With jumps as statements the arm would need a block, and the `case` could not be the right side of a `let`.
`loop` yields through `break value` for the same reason, and a `loop` without a `break` is of the bottom type too.

## AST-48

`if x = 1` is the classic bug, and a language with a bottom type and expression-valued `case` has no need for the idiom that causes it.
As a statement, an assignment has one place, the start of a line.
That frees the same spelling inside a parenthesis, where `name = value` is a named argument and can never be mistaken for a side effect ([FORM-15](forms.md#form-15)).

## AST-67

Every signature scans the same way: a name, then `(`.
A function without parameters that could drop the `()` would look like a property, and inside a struct the two must be told apart by shape alone.
It also keeps `[…]`, `(…)` and `{…}` in one fixed order with the middle one always present, so the eye finds the bindings of a function without reading the brackets before them.

## AST-74

Bindings match structurally, so a binding group is, to a library, only a set of members.
A library function usually needs a small subset of what the application binds, and the short form says so in one line: `binding lighting = (frame, sky)`.
At file level the same line consolidates several small bind groups into the one the pipeline layout gets.
A block would work too, with one forwarding property per member, and nobody wants to write that for the common case.

## AST-75

A library declares the binding it needs and cannot know what the application calls its resources.
The caller supplies it with a local `binding` of the required name, and since bindings match structurally, that is all it takes.

```sgl
fun shade_sky(v: basic_vertex){frame} -> vec3:
    let t = v.pos.x
    binding timing:
        time => t + 1.5
    binding sky:
        skymap => frame.fancy_sky
    return sky_library.sample_sky v.normal
```

A local binding is replaced where it is used, so it need not refer to a global binding at all.
That is why its members are usually all properties, and why a property may reach a local variable such as `t`.
For a texture that makes no sense, and for a constant it is exactly what is wanted.

## AST-82

A property is a computed field: it is read as `v.length`, it takes no arguments, and it sits among the fields it is computed from.
Writing it without a keyword makes the declaration look like what it is.
A method is a function, and a function starts with `fun` everywhere else in the language.
With `fun` mandatory, the first token of a member line says what the line is, and `name(params) => …` never has to be told apart from a call.

## AST-87

Attributes are how tools and targets extend the language without changing it.
A reflection exporter, a UI generator and a target backend each define their own, and the parser of a file cannot know which of them will read it.
So the AST keeps them all, and the phase that owns the table of known attributes judges them.
An unknown one is a warning, since a typo should be seen and a foreign tool's attribute should not stop the build.
Keying that table by name *and* node kind lets one word serve twice: `@vertex struct` is a vertex layout and `@vertex fun` is the vertex stage.
The set of places an attribute may stand is kept additive for the same reason, and `@unroll` on a loop is the first attribute that needs a statement.
