# Member Functions and Properties

*Incubator: not normative.*

## The idea

Enums and structs can have methods and properties.
How a member line is read is normative now: [ast.md](../syntax/ast.md#members), AST-77 to AST-86.

```sgl sketch
struct vec3:
    x: float
    y: float
    z: float
    length => (x * x + y * y + z * z).sqrt()
    fun with_length(self, l: float) => self * (l / self.length)
    fun set_zero(mut self):
        self.x = 0
        self.y = 0
        self.z = 0
```

* **A property** is a name followed by `=>` and an expression, such as `length`.
  It is read like a field, `v.length`, and computed on every read.
* **A longer property takes a block**, `name =>:`, and that block is a value block: it has a value only through `yield` ([AST-107](../syntax/ast.md#value-blocks-and-yield)).
  A `return` there would leave a `fun`, and a property is none.
* **Properties are read-only.**
  A property is a method with a non-`mut` `self`, and there is no setter.
* **`fun` is mandatory on a method, and only a property is keyword-free** ([AST-82](../syntax/ast.md#members)).
  A property is a computed field, and a method is a function like any other.
* **A method** names its receiver as its first parameter: `self` reads, `mut self` may write.
* **A method without `self` is static.**
  That is what "named constructors" are made of.

```sgl sketch
struct vec3:
    ...
    fun unit_x() => vec3(1, 0, 0)

let d = vec3.unit_x().with_length 2
```

```sgl sketch
struct ray:
    origin: pos3
    dir: vec3
    inv_dir =>:
        let d = self.dir
        yield vec3(1 / d.x, 1 / d.y, 1 / d.z)
```

`self` is a reserved name and no keyword, so that `self.x = 0` stays an assignment ([AST-13](../syntax/ast.md#atoms)).

The same property syntax is what a local binding uses to forward a member, `skymap => frame.fancy_sky` ([binding-effects.md](binding-effects.md)).

## What it touches

* The [reserved names](../keywords.md#reserved-names): `self`.
* The AST phase: a member line of a `struct` or `enum` block is a field, a property or a method, told apart by its shape.
* Name resolution: `v.length` and `v.with_length(2)` look into the type of `v`, and `vec3.unit_x()` looks into the type itself.
* Mutability: `mut self` needs a mutable receiver at the call.
* [Structural types](structural-types.md): the synthesized constructor takes the fields only, never properties or methods.
* [Scopes](scopes.md): a struct is an unordered scope, so members may refer to each other in any order.

## Already fixed by the syntax

* `name => expr` is a computes-as form, so a property parses without a keyword.
* After `=>:` the block is the right side of the `=>`, and `yield` is a keyword form like `return`.
* A method is a `fun` keyword form, the same one a function at file level is, and the block colon gives `fun set_zero(mut self):` its body.
* Member access is a form, a fused DOT and a name, so a property read and a field read are one form.
* `mut` is a keyword already.

## Open

* Whether members are in scope unqualified inside a body: `length` above reads `x`, not `self.x`.
* Whether a property may name `self` at all, given that it has no parameter list to declare it in.
* Whether a property may be declared outside the type, as an extension in another module.
* How a static method and the synthesized constructor function of the same struct share one name space.
