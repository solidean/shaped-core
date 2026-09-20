# Why: evaluation

*Tracer: deliberately thin.*

The reasons behind the rules of [evaluation.md](../evaluation.md).
Nothing here is normative.

## EVAL-5

SGL compiles to text that another compiler optimizes ([compilation model](../../incubator/compilation-model.md)), so it cannot promise an instruction sequence, and should not try.
What it can promise is what a run shows: the result and the prints.
Stating the meaning on the structured form, and everything after it as "behaves the same", keeps the rule in one place.
The legalizer, a future constant folder and the target's own optimizer all answer to that one sentence.
It is also what makes the meaning testable: two runs are compared by two values, and no tree is compared with a tree.

## EVAL-15

HLSL, WGSL and MSL do not agree on the order of operands, and HLSL leaves the order of function arguments to the compiler.
A shader with two calls that have effects in one expression would then mean something else on every target, and the difference would be found late.
Left to right is the order the text is read in, so it is the one a reader assumes before looking it up.
Once matters as much as the order: an inlined function names its parameter several times, and an argument with an effect must still run once.
The price is a pin now and then ([LEGAL-16](../legalization.md#expressions)), and only where an effect makes the order observable.

## EVAL-24

An effect analysis that changed meaning would make a program's behaviour depend on how clever the compiler is this month.
So purity is only ever used in one direction: to leave out a pin, a `var` or an `if` that nobody could have noticed.
A function the prelude forgot to mark `@pure` costs a pin, and never a wrong picture.
That is also why everything unmarked is assumed to have an effect.

## EVAL-29

A `return` inside an inlined function, a `yield` in a `case` arm and a `break value` in a `loop:` all say the same thing: stop here, this is the value.
They differ in which construct they stop, and a label says that better than three kinds of jump.
One construct means one set of rules to legalize and one thing to get wrong.
It is also why the structured form has no `return`: the body of a function is a block like any other, and inlining a call is wrapping that block.

## EVAL-35

Every C-like target evaluates the end of a `for` before every iteration.
That is invisible while the end is a constant, and it is a different loop when the body assigns what the end reads.
A range is a value in SGL ([ranges](../../incubator/ranges-and-iteration.md)), and a value is evaluated once, so the language takes that side.
The legalizer pays with one `let` in front of the loop, and only when the end could change.

## EVAL-42

A target gives a `var` without a value whatever it likes: WGSL zeroes it, and HLSL leaves it undefined.
Promising either would mean initializing every such `var`, which the legalizer writes by the dozen and always assigns before it reads.
So such a run has no behaviour, and the check pass is what will keep a program from having one.
The machine still says which of the errors it met, since a legalizer that reads its own result `var` too early is a bug a status finds at once.
