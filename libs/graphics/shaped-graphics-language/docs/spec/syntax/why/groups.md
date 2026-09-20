# Why: group tokens

The reasons behind the rules of [groups.md](../groups.md).
Nothing here is normative.

The phase as a whole exists to make the form parser small.
Comments, whitespace and attributes are gone from its input, a parenthesis is one item with children, and a statement over several lines is one flat run.
What is left is usually a handful of group tokens, so each parse is cheap, needs almost no backtracking, and recursion into a parenthesis is plain child recursion.

## GRP-5

For a token, being fused is a fact about two byte spans.
A group token cannot derive it, because the grouping phase changes it in one place: the leading dot of a continuation line.
Storing it keeps the form parser free of any knowledge of lines.

## GRP-7

A line's children are one of four things, and the end of the line says which.
That is the only place the decision *can* be made without parsing, since the end of the line is where a reader looks too: a colon, an open quote, an open parenthesis, or nothing.
The fourth case, continuation, is the default, because a statement that is too long is the common reason to indent without a colon.

## GRP-8

A lambda with a body is passed inside a call: the line ends in a colon, and the call's parenthesis is still open.
The children have to be the body, since that is what the colon asks for, and the parenthesis is closed by the sibling as always.
Without this rule a multi-line lambda could not be an argument at all.

## GRP-9

The colon does two jobs, ascription and blocks, and position tells them apart without parsing: a block colon ends its line.
That is why a one-line body cannot use a colon.
`if done: return` would make the meaning of a colon depend on what follows it.
One-line bodies use `=>`, which reads as "computes as": `if done => return`.
A trailing comment and trailing attributes do not count, because they are not part of the statement.

## GRP-12

Python needs `pass` because an empty block is a syntax error there.
Here an empty block is a normal error, which catches the real mistake: a header whose body was never written or was dedented by accident.
When the emptiness is meant, the author says why in an indented comment, and that comment is the body.
So the marker for "intentionally empty" carries information, and the language is one keyword smaller.

## GRP-15

An element spans two sibling lines only when the second starts with an infix operator ([GRP-40](#grp-40)).
So the end of an element line is the end of an element, and what follows it says so by its first token.
A comma there adds nothing, and leaving it optional removes the trailing-comma diff noise that lists suffer from elsewhere.
An element that is too long continues on an indented line below it, like any statement.
That keeps a missing comma from silently gluing two lines into one call, which is what would happen if lines of a list were joined first.

## GRP-40

No valid element starts with an infix operator, since it has no left operand, so the rule takes nothing away from GRP-15.
The reach is one element back, inside the same paren group, so an error stays as local as before.
A prefix operator is fused to its operand and an infix operator is not, so `-c` still starts an element and `- c` continues one.
It is the spelling every formatter produces for a long condition: the parenthesis opens the line, and each `and` leads its own line.
Without the rule that condition needs its operator lines indented one level deeper, which no one writes by hand.

## GRP-17

A parenthesis may stay open across lines, and the language still promises that no error escapes its indentation.
Both hold because the *only* place that may close it is the first token of the next sibling, which is exactly the reach [LINE-23](line-tree.md#line-23) allows.
A closer in a child line would let a child affect its parent.
A closer further down would let a line affect a line beyond its sibling.
If the closer is missing, the damage is one error, and the sibling is read as the statement it most likely is.
The obligation can be handed on, as in `) + bar(`, so long expressions need no special rule.
The shape it enforces is the one formatters produce anyway: the closer sits alone at the start of a line, at the indentation of the opener's line.

## GRP-25

A method chain is the one construct that wants to break *before* a fused token.
`material` on one line and `.albedo` on the next must read as member access, yet by spans the dot is not fused to anything.
So the first token of a continuation line counts as fused when it is a dot, and only then.
An operator at the start of a continuation line keeps its spacing meaning: `+ offset` is an infix plus, not a prefix.

## GRP-26

A continuation of a continuation suggests a structure that does not exist.
All the tokens are appended to one run, so deeper indentation cannot mean "binds tighter", and a reader will assume that it does.
It is parsed anyway, in source order, and reported.
Indentation below a continuation line is fine when it means something: element lines of an open parenthesis, or a block.

## GRP-31

Attributes attach by position so that the grouping phase can place them without parsing, and so that the form parser never sees them.
The positions are the ones people write: a line above a declaration, the end of a line, inside a parameter list, and right after `->` or `:` for a return value or a type.
The last of these is the way to annotate a return value, such as `-> @position vec4`.
An attribute in the middle of a line still attaches, to the whole line, and is reported, because the formatter moves it to the front.
So `fun @vertex main` and `@vertex fun main` mean the same, and only the second is kept.
The form parser never sees the attribute, and it does read its arguments, as the paren list they are ([FORM-42](../forms.md#the-parser)).
