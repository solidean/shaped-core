# Why: legalization

*Appendix: informative.*

The reasons behind the rules of [legalization.md](../legalization.md).
Nothing here is normative.

## LEGAL-4

A `once` is a loop in every target, because no target has a block one can break out of.
In the C-like targets it is `do { … } while (false)`, where `continue` jumps to the test, finds it false, and ends the once.
In WGSL it is `loop { … break; }`, where `continue` jumps to the top and never reaches the `break`, so the shader spins.
Neither is "the next iteration of the loop around it", and the two are wrong in different ways, which is the worst kind of wrong.
So the clause is part of what core means, and a `continue` that would cross a `once` takes a flag like any other crossing exit.

## LEGAL-6

C promises that `&&` skips its right operand, and HLSL did not until its 2021 language version: before that both sides always ran.
WGSL and MSL short-circuit.
Relying on the newest HLSL would make the meaning of a shader depend on a flag of the compiler that reads the text, which [EMIT-4](../emitting.md#targets) rules out.
An operand without an effect makes the question moot: whether it ran cannot be observed, so every target is right.
An operand with one becomes an `if`, which every target has always agreed on.

## LEGAL-10

WGSL has no `do … while`, so its `once` is a `loop` that must be left by hand, and forgetting the final `break;` turns running once into running forever.
The C-like form stops by itself, and a `break;` in front of `} while (false);` would only be noise.
So the trailing `break` is no statement of the core tree: the WGSL writer adds it, and leaves it out again where the body already ends in an exit.

## LEGAL-33

A `once` is not free where it stands, and it is not free around it either.
Every `once` is one more construct that a leave or a continue from inside may have to cross, and each crossing is a flag and a test.
Guard clauses are what inlined code is made of: `if not valid => return 0` at the top of a function is a leave in tail position.
Taking those first means the common shape costs nothing at all, and X5 is left with the exits that really are in the middle of something.
Run the other way round, every inlined function with an early return would be a `do { … } while (false)` in the text a reader gets.

## LEGAL-36

`break value` is how a program writes a search, and a search is a loop inside a block: the block is the value, and the loop is what runs.
Without the rule every such loop costs a `once` around it and a flag that crosses the loop, which is X4 and X5 for a construct the source wrote as one `break`.
Behind a loop that ends its block nothing runs, so leaving the block from inside the loop and leaving the loop are the same exit.
The rule says that to X3, and the text is the loop the author wrote.

## LEGAL-45

A `switch` is what a reader expects and what a target's compiler turns into a jump table, and it is available only when every label is a constant the target can write.
An if-else chain is what the language actually means — CHK-154 defines an arm by `scrutinee == pattern` — and it works for every pattern there is.
Taking the chain as the definition and the `switch` as the optimization means the compiler never has to recover intent it threw away: the tree holds the `case`, and this pass reads it.
The chain is also the cheaper form for exits, by LEGAL-48, which inverts the usual expectation and is worth knowing.

## LEGAL-50

This one is measured rather than argued.
A `continue` inside a `switch` inside a loop was put through Dawn in all three shapes it can take, and accepted in each.
They are: inside a `for`, inside a plain `loop`, and inside a `loop` that has a `continuing` block.
The C-like targets have always taken it as the enclosing loop's.
That is what separates a `switch` from a `once`: LEGAL-4 forbids the crossing because a `once` IS a loop in every target, and a `switch` is not.
