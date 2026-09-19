# Why: operators

The reasons behind the rules of [operators.md](../operators.md).
Nothing here is normative.

## OP-2

Few people know whether `and` binds tighter than `or`, or how `&`, `^` and `|` rank in C, and those who know still add parentheses for their readers.
A precedence nobody remembers is a source of bugs and not a convenience.
So each of these families is one level, and mixing inside a level is an error ([OP-15](#op-15)).
The ladder has fewer steps to learn, and the parentheses that careful authors write anyway become the rule.
`:`, `->`, `as` and `in` are one level because they all say something *about* a value, its type, its conversion, its membership, and they chain left to right.

## OP-3

`=>` reads as "computes as", and it separates a head from the expression that defines it.
It must be looser than a keyword form, so that `fun f(x: int) -> int => x + 1` splits into the whole `fun` header and the whole body.
It must be tighter than assignment, so that `let g = fun (x) => x + 1` assigns the lambda and not half of it.
It is also the one-line body of control flow, `if done => return`, which is why the colon can be kept for blocks alone ([GRP-9](groups.md#grp-9)).

## OP-4

In C, `a & mask == 0` means `a & (mask == 0)`.
It is a historical accident, its authors have said so, and every language that copied the table copied the bug.
Here the bit-like operators bind tighter than comparisons, so the line means what it says.

## OP-5

Taking the level from the first character is Scala's rule.
The tokenizer and the form parser never learn a list of operators, so a new operator, and later one a user declares, fits in without changing either.
A reader can place an operator never seen before: `+.` adds, `*~` multiplies.
An operator the language does not define is still read at its level and reported, which is better than a parse that falls apart.
The two exceptions are decided by the *last* character and by a closed list: an operator that ends in `=` assigns, unless it is one of the six comparisons.

## OP-11

`^` is exclusive or, as in every language a shader author is likely to come from.
An exponent operator would want `^` too, or `**`, and both have a confusing precedence against prefix minus: is `-x ** 2` positive?
`pow(x, 2)` has no such question.
`!` is reserved and negation is the word `not`, since `and` and `or` are words already and `!x` is easy to miss in a condition.
`?` is reserved with no plan attached.

## OP-15

`a and b or c` has a meaning in every language, and a different reader guesses a different one.
It is parsed, left to right, and it is a normal error: the fix is one pair of parentheses, and the program still runs while it is being written.
`not` follows the same thought.
It is only allowed where it cannot be misread, on the last operand of a run, as in `a and not b`.
`not a and b` could be `(not a) and b` or `not (a and b)`, so it is an error.
Comparison chains are kept together in one run for a feature: `0 <= i < n` means what it means in mathematics.
A chain is only readable when it is monotone, so `a < b > c` is an error, even though its pairwise meaning is well defined.
`a != b != c` is an error for a sharper reason: it looks like "all different", and pairwise it says nothing about `a` and `c`.

## OP-22

Spacing already carries meaning for a human reader: `a - b` is a subtraction and `-b` is a negation.
Making that the rule gives three things.
First, juxtaposition calls become unambiguous: `foo -a` applies, `foo - a` subtracts ([FORM-26](forms.md#form-26)).
Second, prefix and postfix operators are told apart without a table, which is what user-declared operators will need.
Third, `a-b` is an error and not a subtraction.
That keeps the spelling free, so a dash inside identifiers could be allowed later without changing the meaning of any program that compiles today ([TOK-16](tokens.md#tok-16)).
It reads as infix after the error, because that is what the author meant.
`- a` is not a negation, because allowing it would make `foo - a` ambiguous again.

## OP-28

`0..<4` is how everyone writes a range, and a rule that asks for `0 ..< 4` would be broken in every first program.
The spacing rule protects a spelling: `a-b` stays free so that a dash inside identifiers could be allowed later.
No identifier can contain `..`, so that spelling was never at risk, and the exemption costs nothing.

## OP-29

SGL has no pointers, so nobody needs `a->b`.
`->` and `=>` look like operators, and one rule for every arrow-shaped token is easier to hold than a list of exceptions.
They have no prefix or postfix reading, so only the spelling that is fused on both sides is reported.

## OP-30

`..` already says "and the rest" in the two range operators, so the splat borrows a mark the reader knows.
The form parser only reads it, as a prefix operator like `-`, because it does not know what a list is.
Why it is a prefix, and why it must be a whole element, is in [AST-28](ast.md#ast-28).

## OP-31

A half-open range wants the spelling `a..`: `for i in 1..:` with a `break` inside reads like the ranges that have an end.
A postfix splat would have taken exactly that spelling.
So the postfix `..` stays reserved, and it reports `reserved-operator` like every other postfix operator until the range is decided.
