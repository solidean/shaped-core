# Ordered and Unordered Scopes

*Incubator: not normative.*

## The idea

Some constructs create a new scope: structs, functions and so on.
Scopes come in two kinds.

* **Ordered scopes**, like a function.
  An identifier only becomes usable and visible after it is declared.
* **Unordered scopes**, like a struct or the root scope.
  A function declared further down can be used further up.

```sgl sketch
fun shade(n: vec3) => lambert n      // fine: the root scope is unordered

fun lambert(n: vec3):
    let a = k * 2                    // error: k is not visible yet
    let k = 0.5
    return a
```

**A nested function may use the variables of the surrounding function scope.**
In a general-purpose language that would be a capture.
Here it is not, because everything inlines: the nested function is substituted where those variables are live ([function model](function-model.md)).

```sgl sketch
fun falloff(d: float):
    let radius = 4.0
    fun scaled(x: float) => x / radius   // radius is no capture
    return 1 - scaled d
```

**`use module as name` is legal inside a function body**, not only at file level.
The name it introduces lives in that function's scope, and it follows the ordered rule like any other declaration there.

**Notation** applies as if during name lookup ([notation.md](../notation.md)).
A notation is importable from a module like any other name.
It applies to the names of types, functions and variables, and never to the names of modules.

## What it touches

* Name resolution: two lookup disciplines, chosen by the kind of scope a name is looked up in.
* The AST phase: it records declarations in source order and builds no scope ([AST-94, AST-95](../syntax/ast.md#what-the-ast-records-and-does-not-check)).
* Diagnostics: a use before the declaration in an ordered scope, which can name the declaration further down.
* Notation: the replacement table that is in effect is itself a matter of scope, since a `use` may bring notations in.

## Already fixed by the syntax

* A block is the children of a line that ends in the block colon, so the extent of a scope is given by indentation alone.
* `use` and `notation` are statements, so both may stand wherever a statement may.
* The form tree keeps every spelling as written, and parsing never needs a notation.
* `.` is the only accessor, for members and for modules alike.

## Open

* The full list of constructs that open a scope, and the kind of each: `binding`, `enum`, `case` arms, loops and plain blocks are not classified.
* Whether a nested function is visible before its own declaration inside an ordered scope.
* Whether a local name may shadow a module-level name, and whether that is silent, a warning or an error; shadowing another local is settled by CHK-53 of [checking](../semantics/checking.md).
* Whether a nested function sees a variable that is declared after it in the surrounding function.
* How a cycle between declarations of an unordered scope is reported, for example two constants that initialize each other.
* Whether a notation brought in by a `use` inside a function applies only from that line on.
