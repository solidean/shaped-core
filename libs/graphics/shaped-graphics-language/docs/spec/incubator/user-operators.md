# User-Declared Operators

*Incubator: not normative.*

## The idea

The tokenizer does not list the operators.
Any run of operator characters is one operator token, so a program may spell an operator the language never heard of — and today that is a normal error.

That error is the extension point.
Later, a user may **declare** an operator, choosing any valid operator spelling, and a small fixed set of rules decides how it parses:

* **Its position comes from how it is spaced**, which is already syntax:
  fused on the right only is a prefix operator, fused on the left only is a postfix operator, spaced on both sides is an infix operator.
* **Its precedence comes from its first character**, as it does for the builtin operators:
  one starting with `*` binds like multiplication, one starting with `+` like addition, and so on.

So no declaration ever changes how a line *parses* — only whether it *resolves*.
That is the property worth protecting: a file parses the same whether or not the module declaring its operators has been found yet.

Operators already resolve to calls with special function names, so a declared operator is an ordinary function with an unusual name.

**A first step needs no new syntax: the `@operator` attribute.**

```sgl sketch
@builtin @operator("*") fun transform_position(m: mat4, p: pos3) -> hpos4
```

Such a function is hidden from symbol resolution: its own name is documentation only, and lookup finds it through the operator.
Operator overloading and function overloading are then one mechanism.
It is easy to change later, and the prelude uses it from the start.

## What it touches

* Name resolution: operator spellings become names that can be declared, imported and looked up.
* The diagnostics: "unknown operator" becomes "unresolved operator", reported one phase later.
* Postfix operators, which the syntax reserves and nothing uses yet.

## Already fixed by the syntax

* Operator tokens are max-munched runs; precedence is by first character.
* Operator spacing is syntax, and an operator fused on both sides is a normal error.
* `!` alone and `?` are reserved spellings.
* Mixing different operators of one level without parentheses is a normal error, which a declared operator inherits.

## Open

* The declaration syntax.
* Whether a user operator may share a level with builtin ones in the same run, or always demands parentheses.
* Which first characters a user operator may start with, and whether `=`-ending spellings can be declared as assignments.
* Unicode operator characters such as `×` and `·`, which are symbol characters today.
