# Why: form tree

The reasons behind the rules of [forms.md](../forms.md).
Nothing here is normative.

## FORM-2

The form tree is to SGL what the reader's output is to a Lisp: a generic reading of the text, before anything knows what a function or a struct is.
`struct`, `fun`, `if` and `let` are all the same shape, leading keywords, some expressions and maybe a block, so one rule reads them all.
The AST then interprets the forms, and that is where a declaration becomes a declaration.
This buys three things.
The syntax feels coherent, because there is one way to read a line and not forty productions.
Errors stay small, because the generic reader always produces a form and the AST rejects a form, never a file.
And tools such as the formatter, the highlighter and a future node editor work on forms and never need the full compiler.
Handing the keyword table in as data keeps the parser free of the language, and lets an extension add a keyword without touching it.

## FORM-15

Named arguments are `name = value` everywhere: in a call, in a tuple, in an object literal and in type arguments.
The alternative, `name: value`, is already taken, because a colon inside a parenthesis is ascription, as in a parameter list `(x: int)`.
With one meaning per mark, `(x: int = 3)` reads without a lookup: `x`, of type `int`, is `3`.
An assignment directly inside a paren group has no other use, so it is free to mean a name.

## FORM-20

Member access needs the dot fused on both sides, which separates it from a leading-dot form and keeps `a.b` tight at level 14.
Because it is decided by spans, `1.max(2)`, `t.0` and `scene.lights[2].color` need no special cases.

## FORM-22

Type arguments in angle brackets are the best-known parsing problem of C++ and its descendants: `a < b > c` is a comparison chain or a template, and only name lookup can tell.
Square brackets are already a group, so `buffer[float]` parses with no lookahead and no lookup.
It is the same form as a subscript, and it stays one node in the AST, until name lookup knows what the head is ([AST-14](ast.md#ast-14)).
`[]` also signals intent: these arguments can be deduced.
A compile-time function called with `()` works on types too, and gets no deduction.

## FORM-23

`.point` is a bare member name that is resolved against the type the context expects, which makes enum values and named fields short without importing them.
It is told apart from member access by spacing alone: the dot of `.point` is not fused to anything on its left.
Digits are allowed so that a tuple field can be named: `(.0 = 7, .1 = 8)`.
There is no `::` and no other scope operator ([TOK-19](tokens.md#tok-19)).

## FORM-26

Shader code is dense with small function calls: `length v`, `normalize n`, `cross a b`, `dot n l`.
Juxtaposition removes a layer of parentheses from exactly those, and mathematical code reads closer to the mathematics.
It binds tighter than every infix operator so that the common case needs no thought: `foo a + bar b` is `foo(a) + bar(b)`.
The price is that a complex argument needs parentheses or a local, which is usually an improvement.
The spacing rule is what makes it unambiguous: `foo -a` is a call with a negative argument, and `foo - a` is a subtraction.
`f(x)` and `f (x)` differ for the same reason, the first is a call and the second applies `f` to a tuple.
Juxtaposition calls are an experiment that is accepted from the start, and they may be taken back if they turn out to confuse more than they help.

## FORM-31

A keyword form takes whole expressions, so `return a + 2`, `assert a == b, "msg"` and `print "total: $total"` need no parentheses.
This is the counterweight to tight application.
A function call by juxtaposition binds tightly, because its arguments are usually small.
A keyword binds loosely, because what follows `return` or `if` is usually a whole expression.
`=>` and assignment are looser still, so `fun f(x: int) -> int => x + 1` and `let x : int = 10` split at the right place.
`for i in 0..<n:` needs no loop grammar: it is `for` plus the one expression `i in 0..<n`.
`assert` and `print` are keywords and not functions for a reason of their own: they are compiled out of production code, and that should be visible.
