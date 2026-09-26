# Bindings as an Effect System

*Incubator: not normative.*

## The idea

**Each `binding` is a binding group.**

```sgl sketch
binding frame:
    view: mat4
    proj: mat4
    light_dir: vec3

binding instance:
    model: mat4
    tex_color: texture_2d[rgba8]
```

* Members that are not resources are grouped into a single implicit constant buffer.
* Storage buffers and separate constant buffers are separate.
* The third binding group is reserved for the toolchain ([shader-logging.md](shader-logging.md)).

**A function declares which bindings it wants to use, and in which order**, in `{...}` after its parameters.

```sgl sketch
fun make_mvp(model: mat4){frame} => frame.proj * frame.view * model

@vertex fun my_vs(v: basic_vertex){frame, instance} -> ...:
    let mvp = make_mvp instance.model
```

This is like an effect system: it is checked that every function called inside has its bindings satisfied.
`make_mvp` has `frame` as an implicit binding parameter, and `my_vs` provides it because it declares `frame` itself.

The reason bindings are annotated on functions is that a file can have a lot more bindings and variants than any one shader uses.
Modules can have a lot of them too.
**Only the bindings of the entry point decide what the pipeline layout needs.**

**Inline constants** could simply be an `@inline` annotation on a binding.
Such a binding would only allow constant entries.

**Static samplers** can be declared inline, next to the bindings.
Most samplers in most shaders are static, so this is expected to be used often.
A sampler is effectively a special constant that is initialized, so its body is `setting = value` lines:

```sgl sketch
sampler bilinear:
    filter = .linear
    address = .clamp
```

**A library declares "I need this binding" without dictating the whole binding group.**
`binding name:` is also legal inside a function scope, and that is the mechanism to provide bindings for library functions.

```sgl sketch
module A

binding sky:
    skymap: texture_cube

fun sample_sky(dir: vec3){sky} => ...
```

And in another file:

```sgl sketch
binding frame:
    // lots of stuff
    fancy_sky: texture_cube // different name!

fun foo(v: vec3){frame}:
    use A
    return A.sample_sky v // error! needs binding "sky"

    binding sky:
        skymap => frame.fancy_sky // this is property syntax
    return A.sample_sky v // ok! structurally matching binding with name "sky" is available
```

The member of the local `binding sky` is a property ([members-and-properties.md](members-and-properties.md)) that forwards to a resource of `frame`.
`foo` itself declares only `frame`, so the library's `sky` never dictates a binding group of its own to the caller.
A binding satisfies a requirement when it has the required name and matches structurally.

**A local binding is usually all-property, and its properties may reach local variables.**

```sgl sketch
fun animate(v: basic_vertex){frame}:
    let t = v.pos.x
    binding timing:
        time => t + 1.5
    return wave_library.displace v.pos
```

A local binding is replaced where it is used, so it need not reference a global binding at all.
That is of no use for a texture, and it is definitely wanted for constants.

**The composition short form** builds a binding out of others.

```sgl sketch
binding foo = bar
binding foo = (bar, baz)
```

Bindings are structural, so this is the common form for a library function that needs only a subset of what the caller has bound.
It is usable at file level as well, to consolidate bind groups.

**Two steps, far apart.**
Binding resolution is done close to name lookup and type checking: which binding a name such as `frame.proj` or `sky` refers to.
Binding verification is a very late step: that every function has its bindings satisfied, which needs the inlined call graph.

## What it touches

* The AST phase: the binding list of a function, `binding` inside a function body, and the composition short form.
  How they are read is normative: [AST-66](../syntax/ast.md#functions) and [AST-73 to AST-76](../syntax/ast.md#bindings-and-samplers).
* Name resolution and type checking: binding resolution, and the structural match of a local binding against a required one.
* A late verification pass over the inlined program, and the diagnostic for an unsatisfied binding.
* Layout: the implicit constant buffer, the numbering of binding groups, and the reserved third group.
* `sg` and the generated host code ([host-code-generation.md](host-code-generation.md)): the pipeline layout follows from the entry point alone.
* [Scopes](scopes.md): a `binding` inside a function lives in an ordered scope, which is why the first call in the example fails and the second does not.

## Already fixed by the syntax

* A function's binding list is a fused curly list after its mandatory parameter list, in a fixed order ([AST-66](../syntax/ast.md#functions)).
* A fused curly list anywhere else is reserved as `with_bindings` ([AST-33](../syntax/ast.md#types)).
* A `binding` holds fields without defaults and properties, and no methods ([AST-85](../syntax/ast.md#members)).
* `binding` is a keyword, and a `binding` declaration is a keyword form with a block.
* `=>` in a member line is the same computes-as operator a function body uses.
* An attribute such as `@inline` attaches to the declaration by position, with no grammar of its own.

## Open

* A call-site spelling that mirrors the declaration, as a shorter way to rebind: `a.sample_sky(v){sky = frame_sky}`; the AST reserves the node for it.
* What a binding entry other than a bare name means, such as `{sky as other}` or `{sky = other}`.
* What the composition short form does with two members of one name.
* What "structurally matching" means exactly: names and types of all members, a superset, or member order as well.
* Whether the order of the binding list assigns the binding group indices, and what then keeps the third group free.
* Whether a function must list the bindings its callees need, or only the ones it names itself.
* Whether a local binding may mix forwarding properties with members of its own.
* How `@inline` is spelled and limited, and whether it is an attribute or a keyword ([keywords.md](../keywords.md)).
* Which settings a sampler has, and how they map onto `sg`'s sampler description.
* How storage buffers and separate constant buffers are written as members.
* [bindings.md](../bindings.md) is the place this lands once it is specified.
