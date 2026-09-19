# Member Functions and Properties

*Incubator: not normative.*

## The idea

Enums and structs can have member functions and properties.

```sgl sketch
struct vec3:
    x: float
    y: float
    z: float
    length => (x * x + y * y + z * z).sqrt()
    with_length(self, l: float) => self * (l / self.length)
    set_zero(mut self):
        self.x = 0
        self.y = 0
        self.z = 0
```

* **A property** is a name followed by `=>` and an expression, such as `length`.
  It is read like a field, `v.length`, and computed on every read.
* **Properties are read-only.**
  A property is a member function with a non-`mut` `self`, and there is no setter.
* **A member function** names its receiver as its first parameter: `self` reads, `mut self` may write.
* **A member function without `self` is static.**
  That is what "named constructors" are made of.

```sgl sketch
struct vec3:
    ...
    unit_x() => vec3(1, 0, 0)

let d = vec3.unit_x().with_length 2
```

`self` probably wants to be a keyword as well.

The same property syntax is what a local binding uses to forward a member, `skymap => frame.fancy_sky` ([binding-effects.md](binding-effects.md)).

## What it touches

* The keyword table: `self`.
* The AST phase: a member line of a `struct` or `enum` block is a field, a property or a member function, told apart by its form.
* Name resolution: `v.length` and `v.with_length(2)` look into the type of `v`, and `vec3.unit_x()` looks into the type itself.
* Mutability: `mut self` needs a mutable receiver at the call.
* [Structural types](structural-types.md): the synthesized constructor takes the fields only, never properties or member functions.
* [Scopes](scopes.md): a struct is an unordered scope, so members may refer to each other in any order.

## Already fixed by the syntax

* `name => expr` is a computes-as form, and `name(params) => expr` is a call form on its left side: both parse today without a keyword.
* A member line that ends in the block colon takes a block, which gives `set_zero(mut self):` its body.
* Member access is a form, a fused DOT and a name, so a property read and a field read are one form.
* `mut` is a keyword already.

## Open

* Whether a member function may optionally carry `fun`.
* Whether members are in scope unqualified inside a body: `length` above reads `x`, not `self.x`.
* Whether a property may name `self` at all, given that it has no parameter list to declare it in.
* Whether `self` is a keyword or an ordinary parameter name with a special first position.
* Whether binding groups may carry member functions too, beyond the forwarding properties.
* Whether a property may be declared outside the type, as an extension in another module.
* How a static member function and the synthesized constructor function of the same struct share one name space.
