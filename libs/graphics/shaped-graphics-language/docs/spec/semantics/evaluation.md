# Evaluation

*Tracer: deliberately thin.*

What a flat tree means: the abstract machine that runs the **structured form** of a [flat tree](checking.md#the-flat-tree).
The source cannot say control flow to the check pass yet, so these rules are about the tree, and the tree is what inlining will write.
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

## Locals and places

* **EVAL-8** A local holds one value, and every local of an entry point is a different local, whatever list declares it.
* **EVAL-9** `let` declares an immutable local and gives it its value; nothing assigns it afterwards.
* **EVAL-10** `var` declares a mutable local; without a value it holds nothing, and reading it before an assignment is an error of the program.
* **EVAL-11** A declaration that runs again, in a later iteration of a loop, starts the local afresh.
* **EVAL-12** A **place** is a mutable local, or a member of a place.
* **EVAL-13** `place = value` evaluates `value` and then stores it; the members of the place it does not name keep their values.
* **EVAL-14** A place holds no expression that needs evaluating, so an assignment evaluates its value and nothing else.

## Expressions

* **EVAL-15** Operands and arguments are evaluated **left to right, each exactly once** ([why](why/evaluation.md#eval-15)).
* **EVAL-16** Evaluation is as if every expression were sequenced into single steps: no two operands overlap, and nothing is evaluated twice or skipped, except by EVAL-18.
* **EVAL-17** `not x` evaluates `x`, and a member access evaluates its object.
* **EVAL-18** `a and b` evaluates `b` only when `a` is true, and `a or b` evaluates `b` only when `a` is false.
* **EVAL-19** A construction evaluates one argument per field, in field order.
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
* **EVAL-22** The only statement with an effect on the behaviour of a run is `print`, which appends its value to the trace.
* **EVAL-23** A call of a function that is not `@pure` appends its result to the trace at the step it runs, which makes its place among the prints observable.
* **EVAL-24** "Has no effect" licenses nothing but skipping work nobody could observe; it never changes what a program means ([why](why/evaluation.md#eval-24)).

## Blocks and exits

* **EVAL-25** `block $b { … }` runs its statements in order, and it is a statement or an expression.
* **EVAL-26** `leave $b` ends the block `$b` from any depth inside it: through `if`s, loops and other blocks, and out of the middle of an expression.
* **EVAL-27** `leave $b value` evaluates `value` first, and a block expression then IS that value.
* **EVAL-28** A block expression whose statements end without a leave has no value, which is an error of the program.
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

## Errors of the program

* **EVAL-41** A run that reads a `var` holding nothing, or ends a block expression or the root block without a value, has no behaviour.
* **EVAL-42** The as-if rule promises nothing about such a run ([why](why/evaluation.md#eval-42)).
* **EVAL-43** The machine reports each as a status of its own, and a tree that is ill typed or malformed as a type error; it never asserts.
* **EVAL-44** A run is bounded by a fuel count, one unit per statement, per expression node and per iteration, and running out is a status as well.

## Open

* Which of the errors of EVAL-41 the check pass will refuse once the source reaches it: definite assignment and a value on every path.
* Division, and what an `int` division by zero is.
* Whether `float` arithmetic is exact across targets; the machine computes in `f32`, and a target may fuse or reorder.
* `switch`, which joins with `case` and captures a `break` the way a loop does.
* A place that holds an index, whose index expression EVAL-14 then has to order.
