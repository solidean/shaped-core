# Literal Types

*Incubator: not normative.*

## The idea

**A literal stays a literal, by type, throughout an expression.**
`1` is not an `int` that might convert: it has a literal type, and so has `1 + 2`.

Only where a different type is needed do coercion rules apply, and they apply to literal types alone.
So `(..lit, 1) as rgba8` should work directly, with no `1.0`: the `1` is asked to be a `float` and can be.

**Binding a literal to a variable coerces it to its default type**: `1` becomes an `int`, and `1.0` a `float`.

```sgl sketch
let a = 1              // int
let b = 1.0            // float
let c : float = 1      // the literal is asked to be a float
let d = (..lit, 1)     // does the 1 stay a literal inside the tuple?
```

**This needs to propagate through structural types**, which is where the semantics become muddy.
A tuple that holds a literal has a type that holds a literal type, and what that means when the tuple is stored, passed or compared is not worked out.

**Where the call model already stands.**
A number literal converts, at no cost, to any numeric type that holds it, and the candidate keeping it at its default type wins a tie ([CHK-253](../semantics/checking.md#calls-and-overloads)).
Literal types would refine that rather than replace it.

**Folding what is literal alone.**
A subtree of number literals and operators, with no call of a function, is folded in the front end into a new literal, and the language guarantees it.
Anything touching a float literal becomes a float literal.
`1 / 3` is then an error rather than a silent `0`: it is written `1.0 / 3`, `1 / 3.0` or `(1 as int) / 3`.
Afterwards "what is the type of `1 + 2`" stops being a question, since no builtin operator ever sees a call of literals alone.
The folding runs in 64-bit arithmetic today, and extended precision is the intended refinement.

## What it touches

* The type system: literal types for integers and floats, and the types built from them.
* Overload resolution: a literal argument fits several parameter types, and the ranking among them needs a rule.
* [Structural types](structural-types.md): whether a structural type may hold a literal type, and when it collapses to the default.
* [Types as values](types-as-values.md): evaluation at compile time is what gives `1 + 2` a value while it is still a literal.
* [Vector and format types](vector-and-format-types.md): a constructor such as `vec3(0, 1, 0)` is the everyday case.

## Already fixed by the syntax

* A number literal keeps its spelling, its suffix and its sign in the AST, and nothing is converted there.
* `-3` is one literal, so a negative literal needs no rule of its own.
* A suffix such as `f32` names a type outright, and such a literal is not of a literal type at all.

## Open

* Whether a literal type survives inside a tuple or an object that is bound to a name.
* What a generic function sees when it is called with a literal.
* Whether a literal that does not fit the type asked of it is an error at the literal or at the use.
* The tracer avoids all of it: its shaders write `1.0` where a float is meant.
