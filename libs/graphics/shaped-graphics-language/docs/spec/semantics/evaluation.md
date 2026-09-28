# Evaluation

*Tracer: deliberately thin.*

What a flat tree means: the abstract machine that runs the **structured form** of a [flat tree](checking.md#the-flat-tree).
The rules are about the tree, and [Calls](#calls) and [From the source](#from-the-source) say which tree a program is.
`sgl::check::interpret` is this machine, and the tests run every rule below through it.
Back to the [semantics](_index.md); the reasons are in [why/evaluation.md](why/evaluation.md).

A listing here is the dump of a flat tree, shortened: a label is `$name`, and a type follows ` : `.

## The machine

* **EVAL-1** The machine runs one entry point on one value of its parameter and one value per listed binding.
* **EVAL-2** A run has a **result**, which is the value the entry point returns, and a **trace**, which is every printed value in the order the prints ran.
* **EVAL-3** The result and the trace are the whole **behaviour** of a run; nothing else about a run can be observed.
* **EVAL-4** Two trees **behave the same** when every input gives them the same result and the same trace.
* **EVAL-5** An implementation may produce anything that behaves the same as the structured form: this is the **as-if rule** ([why](why/evaluation.md#eval-5)).
* **EVAL-6** A `float` is a 32-bit IEEE number, an `int` is 32 bits, signed, and its arithmetic wraps, and a `bool` is true or false.
* **EVAL-7** A struct value is one value per field, in field order.
* **EVAL-64** An enum value is the `int` value of one of its type's cases, and `==` and `!=` over two of them compare those `int`s.
  `bool` is the exception: it is a `@builtin` enum (CHK-218), and its value is `false` or `true`, compared by the prelude's `==`.
* **EVAL-74** A `void` value is the one value of its type: a struct value of no fields, and a field of type `void` adds nothing to a struct value.

## Locals and places

* **EVAL-8** A local holds one value, and every local of an entry point is a different local, whatever list declares it.
* **EVAL-9** `let` declares an immutable local and gives it its value; nothing assigns it afterwards.
* **EVAL-10** `var` declares a mutable local; without a value it holds nothing, and reading it before an assignment is an error of the program.
* **EVAL-11** A declaration that runs again, in a later iteration of a loop, starts the local afresh.
* **EVAL-12** A **place** is a mutable local, a member of a place, or an element of a `mut` buffer.
* **EVAL-13** `place = value` evaluates `value` and then stores it; the members of the place it does not name keep their values.
* **EVAL-14** The one expression a place holds is a buffer element's index: it is evaluated once, before the value, and the store goes to the element it named.

## Expressions

* **EVAL-15** Operands and arguments are evaluated **left to right, each exactly once** ([why](why/evaluation.md#eval-15)).
* **EVAL-16** Evaluation is as if every expression were sequenced into single steps: no two operands overlap, and nothing is evaluated twice or skipped, except by EVAL-18.
* **EVAL-17** `not x` evaluates `x`, and a member access evaluates its object.
* **EVAL-18** `a and b` evaluates `b` only when `a` is true, and `a or b` evaluates `b` only when `a` is false.
* **EVAL-19** A construction evaluates its arguments as any call does, by EVAL-80, and so does a literal converted to a struct, which is a call by CHK-81.
  Its value holds one per field, in field order (CHK-102).
* **EVAL-20** A read of a local gives the value it holds at that step, so a read to the left of a write sees the old value.

```raw
(var x : float = (lit 1.0))
(print (call add (local x) (block $v
    (assign (local x) = (lit 10.0))
    (leave $v (local x)) : float)))
```

This prints 11: the left operand is read while `x` is 1, and the block to its right runs after it.

## Effects

* **EVAL-21** An expression **has an effect** when it holds a call of a function that is not `@pure`, or a block that holds a write or a `print`.
* **EVAL-22** The only statement with an effect on the behaviour of a run is `print`, which appends its value to the trace; an `eval` has the effects of its expression and none of its own (EVAL-61).
* **EVAL-23** A call of a function that is not `@pure` appends its result to the trace at the step it runs, which makes its place among the prints observable.
* **EVAL-24** "Has no effect" licenses nothing but skipping work nobody could observe; it never changes what a program means ([why](why/evaluation.md#eval-24)).

## Blocks and exits

* **EVAL-25** `block $b { … }` runs its statements in order, and it is a statement or an expression.
* **EVAL-26** `leave $b` ends the block `$b` from any depth inside it: through `if`s, loops and other blocks, and out of the middle of an expression.
* **EVAL-27** `leave $b value` evaluates `value` first, and a block expression then IS that value.
* **EVAL-28** A block expression whose statements end without a leave has no value, which is an error of the program.
  A `void` block is the exception: it is `void`'s one value however it ends (EVAL-74).
* **EVAL-29** An inlined call, a value block of a `case` arm or a lambda, and a `loop:` with a value are all this one construct ([why](why/evaluation.md#eval-29)).
* **EVAL-30** The body of the entry point is the **root block**, and `leave $root value` returns `value` from the function.
* **EVAL-31** A root block whose statements end without a leave returns nothing, which is an error of the program.
* **EVAL-32** `if c { … } else { … }` evaluates `c` once and runs one of its two lists.

```raw
(let shade : float = (block $search
    (for $rows i in (lit 0) ..< (lit 8)
      (if (call less (local weight) (lit 0.125))
        (then
          (leave $search (local weight)))))
    (leave $search (lit 0.0)) : float))
```

## Loops

* **EVAL-33** `loop $l { … }` runs its statements again and again until something leaves it.
* **EVAL-34** `while $l c { … }` evaluates `c` before every iteration and ends when it is false.
* **EVAL-35** `for $l i in first ..< end { … }` evaluates `first` and then `end`, **once**, before the first iteration ([why](why/evaluation.md#eval-35)).
* **EVAL-36** It runs once per `int` from `first` up to and excluding `end`, and `i` is an immutable local that holds that `int`.
* **EVAL-37** `leave $l` ends the loop `$l`, from any depth inside it.
* **EVAL-38** `continue $l` ends the current iteration of `$l` from any depth inside it, and the loop goes on as after any other iteration.
* **EVAL-39** So after a `continue`, a `while` evaluates its condition again, and a `for` takes its next `int`.
* **EVAL-40** A `leave` or a `continue` names a block or a loop that encloses it; a tree where it does not is malformed.

## Case

* **EVAL-65** `case v { … default { … } }` evaluates `v` once and holds it, then tries its arms in order.
* **EVAL-66** An arm is tried by evaluating its patterns in order and comparing each with the held value; the arm is selected at the first that is equal.
* **EVAL-67** A pattern behind the one that selected an arm is not evaluated, and neither is any pattern of an arm behind that one.
* **EVAL-68** So the patterns that run are exactly those up to and including the one that matched, in the order written.
  A pattern with an effect has it where it is reached ([why](why/evaluation.md#eval-68)).
* **EVAL-69** The `default` arm is selected when no other was, and every `case` of the machine has one.
* **EVAL-70** The statements of the selected arm run, and those of every other arm do not.
* **EVAL-71** A `case` is a statement, and a `case` of the source that is a value is the block of EVAL-73.

```raw
(let weight : float = (block $case
    (case (local kind)
      (arm ((lit 0))
        (leave $case (lit 1.0)))
      (default
        (leave $case (lit 0.0)))) : float))
```

## Calls

* **EVAL-45** A call of a function of the program IS the callee's body as `block $callee { … }`, where the call stood.
  It is an expression of the callee's return type, and a statement for a callee that returns `void`.
* **EVAL-46** The arguments are evaluated left to right, each exactly once, before any statement of the body.
* **EVAL-47** A parameter is a value: the callee cannot change it, and the caller's later writes do not reach it.
* **EVAL-48** An argument that is a literal or an immutable local stands wherever its parameter is read.
  Every other argument is bound by a `let` named after its parameter, at the top of the block, in the order EVAL-80 evaluates them.
* **EVAL-49** `return value` of the callee is `leave $callee value`, and its bare `return` is `leave $callee`.
* **EVAL-50** The entry point's own `return value` is `leave $root value`, which a tree of the check pass spells `return`.
* **EVAL-51** Inlining moves nothing: the block stands where the call stood, so EVAL-15 alone says when its statements run ([why](why/evaluation.md#eval-51)).
* **EVAL-52** Every local of an inlined body is a local of its own by EVAL-8, so two calls of one function share none.
* **EVAL-53** A callee that returns a value leaves its block with one on every path ([CHK-125](checking.md#returning)), so a call never meets EVAL-28.
* **EVAL-80** A call evaluates its written arguments left to right, in the order they are written ([why](why/evaluation.md#eval-80)).
  Then it evaluates the default of each parameter left unfilled, in parameter order.
  Its parameters are bound from those values, so it is as if every argument were a `let` in that order and the call read locals alone.
* **EVAL-81** A default is evaluated where its call stands, once per call that leaves its parameter unfilled, and it reads the values bound to the parameters before it.
* **EVAL-82** A call of a builtin evaluates its arguments by EVAL-80 as a call of the program does, and nothing about a target's own order of arguments reaches the program.

```sgl
fun grade(x: float, limit: float) -> float:
    if x < limit => return 0.0
    return x * 4.0

fun graded(a: float) -> float:
    let limit = 0.5
    return grade(a * 2.0, limit)
```

`a * 2.0` is bound, and `limit` stands for itself:

```raw
(let limit : float = (lit 0.5))
(return (block $grade
    (let x : float = (call multiply (local a) (lit 2.0)))
    (if (call less (local x) (local limit))
      (then
        (leave $grade (lit 0.0))))
    (leave $grade (call multiply (local x) (lit 4.0))) : float))
```

## From the source

* **EVAL-54** `let` is `let`, `let mut` is `var`, and `place op= value` is `place = place op value`.
  Its second read of the place goes through the index EVAL-14 evaluated, and evaluates nothing again.
* **EVAL-55** An `if` / `else if` / `else` chain is an `if` whose `else` holds the rest of the chain.
* **EVAL-56** `while`, `for … in first ..< end` and `loop:` are the loops of the same names; `break` is `leave $loop` and `continue` is `continue $loop`, of the innermost loop.
* **EVAL-57** A `loop:` that is a value is `block $loop_value { loop $loop { … } }`, and its `break value` is `leave $loop_value value`.
* **EVAL-58** `a < b <= c` is `a < b and b <= c` with `b` evaluated once: each inner operand is bound to a local where it first stands, and read from it after that.
* **EVAL-59** So a chain stops at its first comparison that is false, and evaluates no operand behind it.
* **EVAL-72** A `.name` pattern is the `int` value of its case, an arm with several patterns holds them in the order written, and the `_` arm is the `default`.
* **EVAL-73** A `case` of the source that is a value is `block $case { case … }`, whose arms leave it with their values, as EVAL-57 does for a `loop:`.
  An arm that exits instead leaves nothing, and the block is EVAL-29's construct either way.
* **EVAL-60** `print value` is `print`.
* **EVAL-61** `eval value` evaluates `value` and drops it: what the evaluation prints and records happens, in its place, and the value goes nowhere.
* **EVAL-62** A call that stands as a statement is `eval` of the call ([CHK-137](checking.md#inferred-results-and-dropped-values)).
  A call of a function that returns `void` is its block as a statement.
* **EVAL-63** What a builtin function computes is the evaluator of its registry record ([why](why/evaluation.md#eval-63)).
  An evaluator is given the scalars of its arguments and gives the scalars of its result.
  The machine checks the number and the kind of both against the record's parameter and result types, so an ill-typed call is a type error by EVAL-43 and never reaches an evaluator.

## Checks

* **EVAL-75** `check` runs its body, then reads its condition: a false one is recorded with the value of every node that ran, and the run goes on.
  A node that did not run, the right side of an `and` whose left side was false, is recorded as not evaluated.
* **EVAL-76** An `assert` is a `check` that stops the run where it is false.
* **EVAL-77** A run that reaches the end of a function returning `void` ends with `void`'s value, which is how a test and a compute entry point end.
* **EVAL-78** A test passes when its run ends normally, having run at least one check and found none false.
  A false check or `assert`, a run out of fuel, a read of a `var` nothing assigned, and a run that ran no check each fail it.
  A failure is reported as `test-failed`.
* **EVAL-79** A false check is reported narrowed: through `and`, `or`, `not` and a chain to the parts that were false, a comparison with the values of its operands.
  A `not` of a comparison that held reports that comparison's values.

## Errors of the program

* **EVAL-41** A run that reads a `var` holding nothing, or ends a block expression or the root block without a value, has no behaviour.
* **EVAL-42** The as-if rule promises nothing about such a run ([why](why/evaluation.md#eval-42)).
* **EVAL-43** The machine reports each as a status of its own, and a tree that is ill typed or malformed as a type error; it never asserts.
* **EVAL-44** A run is bounded by a fuel count, one unit per statement, per expression node and per iteration, and running out is a status as well.

## Open

* Definite assignment: a `let` without a value is what would let a program read a `var` that holds nothing, and the check pass carries none yet.
* What an `int` division by zero is, which is why `int` has no `/` yet.
* Whether `float` arithmetic is exact across targets; the machine computes in `f32`, and a target may fuse or reorder.
* Whether a pattern that has an effect is worth the ordering EVAL-68 has to promise, which a pattern language would make sharper ([patterns](../incubator/patterns.md)).
