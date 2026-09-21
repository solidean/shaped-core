# Why: numbers

The reasons behind the rules of [numbers.md](../numbers.md).
Nothing here is normative.

## NUM-1

The tokenizer has no number token ([TOK-35](tokens.md#tok-35)), so something has to put `1`, `.` and `5` back together.
The form phase is the first one that knows which tokens are fused and where an operand may start, and those two facts are all the assembly needs.
A symbol that starts with a digit is always a number, never an identifier, so `10a7` gets one clear `malformed-number` and not a puzzling name lookup failure.

## NUM-4

Three spellings start with a number and a dot, and all three are wanted.
`1.` is a float, because shader authors write it all day and refusing it would be pedantry.
`1.max(2)` is a member access on the integer `1`, because members on literals are useful, as in `2.0.sqrt()` or `90.degrees`.
`1..<4` is a range.
One rule decides: look at what is fused after the dot.
Nothing, or a symbol that starts with a digit, makes it a float.
A symbol that starts with a letter makes it a member.
A second dot never gets this far, because two dots are an operator token already.
The one spelling this gives up is `1.f32` and `1.e5`, which read as members; they are written `1.0f32` and `1e5`.

## NUM-7

`t.0.1` is the second field of the first field of a tuple.
If a symbol after a fused dot could start a number, `0.1` would be assembled into a float and the access would break.
The rule is one-sided on purpose: a number may absorb a dot to its right, and never one to its left.

## NUM-9

`-3` is one literal so that the smallest integer of a type can be written, and so that a negative constant is a constant without any folding.
It follows from the spacing rule: a `-` fused to the right only is a prefix.
`a - 3` subtracts, `a -3` applies `a` to `-3`, and `a-3` is an error.

## NUM-12

`1_000` is what several languages write, and `_` is a symbol character, so the tokenizer accepts it without complaint.
SGL uses `'`, as C++ does, so that `_` keeps one role inside symbols: joining the words of a name.
Because the intent of `1_000` is obvious, it is a normal error with a fix, and the value is what the author meant.
