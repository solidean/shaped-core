# Stage Interfaces

*Incubator: not normative.*

## The idea

**An entry point is a function with a stage attribute**: `@vertex fun`, `@pixel fun`.
The stage names are the ones of the [terminology](../terminology.md): "pixel", not "fragment".

**What flows between two stages is an ordinary type.**
The vertex stage may return an anonymous struct type, written in place:

```sgl sketch
@vertex fun my_vs(v: basic_vertex){frame, instance} -> {
    @position pos: hpos4
    normal: vec3
    uv: vec2
}:
    let mvp = make_mvp instance.model
    return {
        pos = mvp * v.pos
        normal = (mvp as mat3) * v.normal
        uv = v.uv
    }

@pixel fun my_ps(
    pos: hpos4
    normal: vec3
    uv: vec2
){frame, instance} -> pixel:
    ...
```

The members of that type need to match the parameters of `my_ps`.
A declared struct works just as well as the anonymous type.

**`@position` is a semantic**, and it is needed on exactly one output of the vertex stage.
The other members carry none.

**A struct annotated with a stage attribute describes the edge of the pipeline.**

```sgl sketch
@vertex struct basic_vertex:
    pos: pos3
    normal: vec3
    uv: vec2

@pixel struct pixel:
    color: rgba8
    normal: vec4f16
```

* `@vertex struct` makes the struct a valid vertex input.
* `@pixel struct` defines the framebuffer format, and member order is target order.
  Its member types are format types ([vector-and-format-types.md](vector-and-format-types.md)).

Both are what the tooling mirrors into the host language ([host-code-generation.md](host-code-generation.md)).

**Attributes are an open set.**
An attribute is validated per pair of name and node kind, in a later phase, and an unknown one is a warning.
`@unroll` is an example of one that would go on a statement rather than a declaration.
So the set of places an attribute may stand stays additive.

## What it touches

* The AST phase: attributes stay attached to their node and are not interpreted there.
* A later validation phase: the table of known attributes per node kind, and the warning for an unknown one.
* Type checking across entry points: the output type of one stage against the parameters of the next.
* [Structural types](structural-types.md): the returned `{...}` literal converts to the anonymous return type because the members line up.
* The transpiler: semantics, locations and render target indices of every target language follow from names and member order.
* [Binding effects](binding-effects.md): the binding lists of the entry points decide the pipeline layout.

## Already fixed by the syntax

* An attribute is a symbol that starts with `@`, with optional arguments in a fused round group.
* Attributes attach by position, in the grouping phase, and the form parser never sees them.
  That is why `@position` may lead a member line inside a curly group and `@vertex` may lead a `fun` or a `struct` alike.
* Every form may carry attributes, so a statement can carry one as well as a declaration.
* A curly group over several lines takes one element per line, which is what lets the anonymous return type read like a struct body.
* `->` stands only in function types and return types.

## Open

* How two entry points are paired: by a pipeline declaration, by the host, or by matching types alone.
* Whether matching is by name, by order or by both, and whether the pixel stage may take fewer members than the vertex stage returns.
* Whether the pixel stage may take one struct parameter instead of one parameter per member.
* The full list of semantics besides `@position`, and how interpolation qualifiers are spelled.
* Depth output, and a pixel stage without color targets.
* The stages beyond vertex and pixel, several of which the terminology still marks as undecided.
* Where the line runs between an unknown attribute, a warning, and a known attribute on a node kind that does not accept it.
  The draft [ast.md](../syntax/ast.md) calls the second a normal error.
