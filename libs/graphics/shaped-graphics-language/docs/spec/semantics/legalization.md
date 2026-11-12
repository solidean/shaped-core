# Legalization

*Appendix: informative.*

How the compiler takes the [structured form](evaluation.md) of a flat tree to the **core form**, which every target prints one to one.
Nothing here is normative: the [as-if rule](evaluation.md#the-machine) is, and these rules are one way to honour it.
`sgl::check::legalize` implements them, `sgl::check::find_core_violation` is the definition of core, and a randomized test runs both forms of many programs and compares.
Back to the [semantics](_index.md); the reasons are in [why/legalization.md](why/legalization.md).

The worked examples are WGSL, since it is the strictest target: no `do … while`, no `goto`, no uninitialized variable, and no resource in a local.

## The core form

* **LEGAL-1** The core form is the flat trees that hold no block and no `leave`: an expression holds no statement, and every exit is `break`, `continue` or `return`.
* **LEGAL-2** `once { … }` runs its statements once, and a `break` directly inside it ends it.
* **LEGAL-3** `break` ends the innermost enclosing `once`, loop or `switch`.
* **LEGAL-4** `continue` names the innermost enclosing loop, and no `once` stands between the two ([why](why/legalization.md#legal-4)).
* **LEGAL-50** A `switch` may stand between them: every target takes a `continue` inside one as the enclosing loop's, WGSL included ([why](why/legalization.md#legal-50)).
* **LEGAL-5** `return` is legal at any depth: it is the exit of the root block.
* **LEGAL-54** `discard` is legal at any depth, and no pass moves anything across it: it ends the invocation, not a construct around it.
* **LEGAL-6** The right operand of `and` / `or` has no effect ([why](why/legalization.md#legal-6)).
* **LEGAL-7** The `end` of a `for` has no effect and reads no mutable local and no buffer element, since a target evaluates it before every iteration.
  It calls no builtin that takes derivatives either: after a divergent `break`, that re-test runs in part of a quad.
* **LEGAL-55** An assignment whose place has an index with an effect has a value without one, since a target may write the value's effect first (EVAL-14).
* **LEGAL-8** A tree that is core already is left as it is, but for the let rules L1 and L2.
* **LEGAL-9** An emitter refuses a tree that is not core with the error `not-core`, whose detail names the first violation; it never asserts.
* **LEGAL-53** (V0) A core tree holds no `check`: each goes with its body, first, since no target writes a check or an `assert` (EVAL-75).
* **LEGAL-63** (V0) Asked to, legalization drops every `print` with its values too, which is how a text is written without diagnostics.
* **LEGAL-64** (V0) Asked to, it lowers each `assert` instead: its body, then `if not condition`, a print of its message and the end of the invocation.
* **LEGAL-70** Asked to, the last step writes each `print` of the finished core tree as statements over the reserved binding `sg_debug`, for every target ([the contract](../../reserved-bindings.md)).
  Each value is bound once, ahead of the reservation, and the site table is built here, so what folding removed has no site and no route can differ.
  Left unasked, a print stays a statement, which only the interpreter runs.
  A compute, a geometry or a ray-tracing stage that returns nothing returns, and a pixel stage discards.
  A vertex and a tessellation evaluation stage return their result at a NaN position, which culls every primitive the vertex is part of.
  A tessellation control stage returns its edge factors at 0, which culls the patch.
  An any hit returns `ignore`, and an intersection a report whose `is_hit` is false.
  An `assert` before the last barrier, derivative or subgroup operation sets LEGAL-65's flag instead, and its invocation ends where a guard's does.
  The tree then prints nothing; it is the one way it may behave otherwise than its input, and only when the caller asked.
* **LEGAL-65** (G1) Asked to, legalization first guards each operation that may have no value: the robustness profile `abort`, `report` or `define` (LEGAL-68).
  That is an integer `/` or `%` whose divisor is no literal that cannot fail (EVAL-84), and an array index that is no literal (EVAL-90).
  A binding array's own index is one, whatever element it names, a mark staying on the index: by its `N`, or where it is unbounded by CHK-491's length.
  The guard is a block in the operation's place that binds the operands, tests them, and only then runs the operation, so every later rule keeps its order and its short circuits.
  A failed test ends the invocation as an `assert` of LEGAL-64 does, and `report` first prints the operation and its operands as that `assert`'s message.
  Where a barrier, a derivative or a subgroup operation may still follow, every invocation must reach it, so the end waits:
  the guard sets a flag and replaces the failing operand with one that cannot fail, a divisor of 1 or an index of 0.
  The invocation then ends after the last statement of the entry point's body that holds such an operation, and what it writes until then is skipped (LEGAL-67).
  Until then, a loop that holds none of those operations is left at the top of each iteration once the flag is set, so a safe operand never keeps it running long.
  A loop that holds one has a trip count every invocation of the workgroup shares, so its guards trip in all of them or in none.
  It guards the maths builtins of EVAL-87 too, each by the test the interpreter asks: `pow`, `asin`, `acos`, `atan2`, `smoothstep` and `clamp`, a scalar beside a vector read as every lane of it.
  So is a matrix's `row` or `column` whose index is outside the matrix.
  So is workgroup memory read before any invocation stored it (EVAL-92), by LEGAL-69.
  Wherever the input has a value, the tree behaves as the input does; where it has none, the tree ends the invocation instead.
* **LEGAL-66** (G1) Where the target reads a buffer's length (EMIT-195), those profiles also guard a buffer index and a `bytes` access, by the length of CHK-460.
  A buffer index outside `0 ..< values.length` ends the invocation as an array index does (EVAL-90).
  So does a `bytes` load or store whose offset is no multiple of 4, or whose last word reaches past the bound memory (EVAL-97).
  The memory itself is never bound to a local, so only the offset and a stored value are, in their order.
  An element of a binding array names its memory for the length and again for the access, so its index is pinned in front of both, and evaluated once.
  An access through one is a block of its own: a read gives the element, a store evaluates its place and then its value, and an atomic its place and then its operands.
  MSL reads no length, so a metal text guards neither.
* **LEGAL-67** (G1) Once a guard may set that flag, every write up to the flag's test is skipped where it is set, so a safe operand's result reaches no memory.
  A write is a store to a buffer element or to workgroup memory, and a call with an effect, an atomic or an image store, that stands as a statement.
  A barrier, a derivative and a subgroup operation are never skipped, since every invocation must reach them.
  An atomic whose result a `let` binds runs only where the flag is clear, and the `let` is 0 where it is set.
* **LEGAL-68** (G1) The profile `define` runs each guard of LEGAL-65 and LEGAL-66 without ending anything: a failed test replaces the operand by its safe value, and the operation runs on it.
  A divisor of 1, so `x / 0` is `x`, `x % 0` is 0 and `INT_MIN / -1` is `INT_MIN`, which are WGSL's values; an index or an offset of 0.
  A maths builtin of EVAL-87 runs on the operands its deferred guard gives: `pow` on a base of 1, `asin` and `acos` on 0, `atan2` on an `x` of 1.
  `smoothstep` runs on the edges 0 and 1, and `clamp` on a `high` of `low`.
  It prints nothing and needs no flag, since no invocation ends, and the tree has a value wherever the input has one.
  Memory too short for the access has no index or offset that is safe, so the access does not run.
  A read and a `bytes` load are zero of their type, a store is dropped, and an atomic gives zero and changes nothing.
  Its operands still run, in their order, as WGSL's bounded access evaluates them.
* **LEGAL-69** (G1) Those profiles shadow a compute, task or mesh stage's workgroup memory with a bit per scalar, set where one is stored and tested where one is read (EVAL-92).
  A scalar is a leaf of a member's type, counted in the order the members are listed: a `float3` is three, an atomic one, whatever its width.
  A store and an atomic set the bits of the scalars they reach once they ran; every read, an atomic's but a store's included, tests them first.
  A uniform load tests them after its barrier, which is what makes another invocation's store visible to it.
  A failed test ends the invocation as a guard does, and `report` names the place with its indices: ``workgroup memory `tile.values[2]` is read before any invocation stored it``.
  A swizzle tests each field it reads, and each index of the place is pinned once, so the test and the access read the same element.
  A read that races a store, with no barrier between, may or may not be reported.
  The shadow takes a word per 32 scalars beside the memory it shadows, in CHK-294's 16384 bytes.
  Where it would not fit no read is tested, and the compile warns `workgroup-unshadowed`, naming the bytes it left out.
  The profile `define` shadows nothing and zeroes workgroup memory before the body instead, WGSL's value (LEGAL-71).
* **LEGAL-71** The last step writes the shadow and its prologue as statements of the finished core tree, so no route knows of either.
  A read of the shadow is an atomic `load` of its word, and a mark an atomic `bit_or`, each on a place no source can name, which the target declares `sgl_written` (EMIT-229).
  Which bits a range of scalars takes is arithmetic in the tree, a whole word its own case, since no target shifts by 32.
  Ahead of the body, each invocation stores 0 to the words its index in the workgroup strides to.
  Under `define` it zeroes workgroup memory instead: an array member element by element in that stride, any other member in invocation 0, and an atomic by its store.
  A `workgroup_barrier` follows either, so no mark lands before a clear and no read before the zero.
  A target whose workgroup memory starts at zero, as WebGPU's does, gets no prologue.
* **LEGAL-52** (V1) A core tree holds no `void` local, and no `let`, `var`, assignment, `return` or read of a `void` value: what evaluating one does stays as an `eval`, and the value itself is gone.
  It follows every rule that introduces a local for a result, since a `void` result is one of them; only the let rules come after it.

## The table

One row per core construct, one column per target; GLSL has no emitter yet, and its column is what it will print.

| core | HLSL | WGSL | MSL | GLSL |
|---|---|---|---|---|
| `once { … }` | `do { … } while (false);` | `loop { … break; }` | `do { … } while (false);` | `do { … } while (false);` |
| `loop { … }` | `while (true) { … }` | `loop { … }` | `while (true) { … }` | `while (true) { … }` |
| `while c { … }` | `while (c) { … }` | `while c { … }` | `while (c) { … }` | `while (c) { … }` |
| `for i in a ..< b { … }` | `for (int i = a; i < b; ++i) { … }` | `for (var i: i32 = a; i < b; i++) { … }` | `for (int i = a; i < b; ++i) { … }` | `for (int i = a; i < b; ++i) { … }` |
| `switch v { [a, b] { … } … default { … } }` | `switch (v) { case a: case b: … break; default: … break; }` | `switch v { case a, b: { … } default: { … } }` | as HLSL | as HLSL |
| `if c { … } else { … }` | `if (c) { … } else { … }` | `if c { … } else { … }` | `if (c) { … } else { … }` | `if (c) { … } else { … }` |
| `let x : T = v` | `const T x = v;` | `let x: T = v;` | `const T x = v;` | `const T x = v;` |
| `var x : T = v` | `T x = v;` | `var x: T = v;` | `T x = v;` | `T x = v;` |
| `var x : T` | `T x;` | `var x: T;`, which is zeroed | `T x;` | `T x;` |
| `place = v` | `place = v;` | `place = v;` | `place = v;` | `place = v;` |
| `break` | `break;` | `break;` | `break;` | `break;` |
| `continue` | `continue;` | `continue;` | `continue;` | `continue;` |
| `return v` | `return v;` | `return v;` | `return v;` | `return v;` |
| `a and b`, `a or b`, `not a` | `a && b`, `a \|\| b`, `!a` | `a && b`, `a \|\| b`, `!a` | `a && b`, `a \|\| b`, `!a` | `a && b`, `a \|\| b`, `!a` |
| `<`, `<=`, `>`, `>=`, `==`, `!=` | `a < b`, … | `a < b`, … | `a < b`, … | `a < b`, … |
| `-a` | `-a` | `-a` | `-a` | `-a` |
| `int`, `bool` | `int`, `bool` | `i32`, `bool` | `int`, `bool` | `int`, `bool` |

* **LEGAL-10** WGSL's `once` ends in a `break;` of its own, and the C-like targets' does not: `do { … } while (false)` already stops ([why](why/legalization.md#legal-10)).
* **LEGAL-11** An `if` that is the whole `else` of another is written `else if`.
* **LEGAL-12** `&&` and `||` never stand bare inside each other: the inner one is parenthesized, since WGSL refuses the mix.
* **LEGAL-13** A `print` has no row: no target writes one yet, and an entry point that holds one is `unsupported`.
* **LEGAL-42** `eval v` is core when `v` is: it is `v;` in the C-like targets and `_ = v;` in WGSL ([EMIT-75](emitting.md#the-text)).

## Case

* **LEGAL-44** The core form has `switch v { [a, b] { … } … default { … } }`, whose `default` is mandatory.
  An arm's values are literals a target can write as a label: an `int` literal, or an enum case, which is one under a name.
* **LEGAL-45** A `case` becomes a `switch` when every pattern of every arm is such a literal, and a chain of `if` otherwise ([why](why/legalization.md#legal-45)).
  It runs between two passes of the expression rules: the first takes a `case` out of the block expression that held it, and the second lowers the conditions the chain writes.
* **LEGAL-46** (C1) The chain binds the scrutinee to a `let`, and each arm is an `if` whose condition is its patterns compared with `==` and joined by `or`; the `default` arm is the last `else`.
  It costs one `let`, and none where the scrutinee is an immutable local, which each arm reads again.
  A `bool` pattern is the scrutinee itself or its `not`, never `== true`.
* **LEGAL-47** A pattern that has an effect is carried by the second pass: its block moves in front of the `if` that tests it.
  That `if` stands inside the branch that reaches it, so the pattern runs only where EVAL-68 says it runs.
* **LEGAL-48** The chain captures no `break`, so an exit that crosses a `case` in that form costs nothing, and only the `switch` form is a construct X5 crosses.
* **LEGAL-49** X6 extends to a `switch` that is the last statement of its block: a `leave` of that block, directly inside an arm, is that arm's `break`.
  So a `case` expression costs the one `var` of E1 and nothing else.

A `case` over an enum, which LEGAL-45 takes to a `switch` and LEGAL-49 leaves without a flag:

```raw
(let weight : float = (block $case
    (case (local kind)
      (arm ((lit 0))
        (leave $case (lit 1.0)))
      (default
        (leave $case (lit 0.0)))) : float))
```

```wgsl
var case_result: f32;
switch kind {
    case light_kind_point: {
        case_result = 1.0;
    }
    default: {
        case_result = 0.0;
    }
}
let weight: f32 = case_result;
```

## Expressions

The rules run cheapest first, and each costs what its line says.

* **LEGAL-14** (E1) A block expression moves in front of the statement that holds it, and its value is a `var` without a value that its leaves assign.
  It costs one `var`.
* **LEGAL-15** (E1) A block whose only leave is its last statement needs no `var`: its statements move, and its value stays where the block stood.
  It costs nothing.
* **LEGAL-16** (E2) Before a block moves, every operand to its LEFT in evaluation order is **pinned** into a `let`.
  It costs one `let` per pinned operand.
* **LEGAL-17** (E2) An operand is left unpinned when it has no effect AND the moved statements assign no local it reads and store to no buffer it reads an element of.
* **LEGAL-18** So a literal, an immutable local and a member of one are never pinned, and a read of a `var` that the block assigns always is.
* **LEGAL-51** (E2) An assignment to a buffer element lowers the element's index before its value, and pins the index into a `let` when the value's moved statements could change it (EVAL-14).
  An index with an effect is pinned too wherever the value has one, in an element of a local or of workgroup memory as well, which is what LEGAL-55 asks.
* **LEGAL-19** (E3) `and` / `or` whose right operand moved statements, or has an effect, is an `if` over a `var`; any other stays `&&` / `||`.
  It costs one `var` and one `if`.
* **LEGAL-20** (E4) A `while` whose condition moved statements is a `loop` that starts with them and with `if not c { break }`.
  It costs one `if`.
* **LEGAL-21** The test stands at the top of the body, so a `continue` still meets the condition before the next iteration.
* **LEGAL-22** The `end` of a `for` that LEGAL-7 refuses is pinned in front of the loop, and then `first` is pinned too when it has an effect, so the two keep their order.
* **LEGAL-43** The value of an `eval` is lowered like the value of any statement, by E1 to E3.
  An `eval` whose value was a block is dropped once the block has moved: what is left of it is a read of a local or a literal, and evaluating that is nothing.
  It costs nothing.

A read of `x` to the left of a block that assigns `x`:

```raw
(var x : float = (lit 1.0))
(let y : float = (call add (local x) (block $v
    (assign (local x) = (lit 10.0))
    (if (local c)
      (then
        (leave $v (lit 20.0))))
    (leave $v (lit 30.0)) : float)))
```

```wgsl
var x: f32 = 1.0;
let x_before: f32 = x;
var v_result: f32;
x = 10.0;
if c {
    v_result = 20.0;
} else {
    v_result = 30.0;
}
let y: f32 = x_before + v_result;
```

An `and` whose right side is a block, and a `while` whose condition is one:

```wgsl
var and_result: bool = a;
if and_result {
    samples = samples + 1;
    and_result = samples < 8;
}

loop {
    budget = budget - 1;
    if !(0 < budget) {
        break;
    }
    total = total + budget;
}
```

## Exits

* **LEGAL-23** (X1) `leave $root value` is `return value`, from any depth.
  It costs nothing.
* **LEGAL-24** (X2) A block whose leaves all stand in **tail position** disappears: its label goes, and its statements stand where it stood.
  It costs nothing.
* **LEGAL-25** A leave is in tail position when it is the last statement of the block, or of a branch of an `if` that is.
* **LEGAL-26** Statements that follow an `if` whose one branch always exits move into the other branch, which is what puts a guard clause in tail position.
* **LEGAL-27** X2 gives up where both branches can reach what follows and one of them holds a leave, since it would have to write those statements twice.
* **LEGAL-28** (X3) A `leave $l` of a loop, with no other `once` or loop in between, is `break`.
  It costs nothing.
* **LEGAL-29** (X4) Any other block is a `once`, and a `leave` directly inside it is `break`.
  It costs one construct.
* **LEGAL-30** (X5) A `leave` or a `continue` that would cross a `once` or a loop sets a `bool` **flag** named after its target, and breaks out of what it is in.
* **LEGAL-31** After each crossed construct the flag is tested, and the exit is repeated with `break`, or performed where the target is reached.
  It costs one `bool` per target, one test per crossed construct.
* **LEGAL-32** The flag of a `leave` is declared false in front of its target, and the flag of a `continue` at the top of the loop's body, so each iteration starts with it false.
* **LEGAL-33** X2 runs before X4 ([why](why/legalization.md#legal-33)).
* **LEGAL-36** (X6) Where a loop is the last statement of block `$b`, a `leave $b` inside that loop is a leave of the loop: nothing of the block runs behind it.
  It costs nothing, and it runs before X2, which then finds a block without a leave ([why](why/legalization.md#legal-36)).
* **LEGAL-62** (X7) A `once` whose every `break` stands at the end of a side of an `if` is its body, in its place: each such `if` takes what follows it into the side that runs on.
  The test of X5's flag behind a loop is such an `if`, so `if found_left => break` and the tail behind it are `if not found_left` around that tail, and a guard is an `if` and an `else`.
  It gives up where both sides run on into statements that follow, since it would write them twice, and where a `break` stands in a `switch`.
  It runs after X5 over the core tree, and before the lets' rules, which then fold the test of a flag that is constant.
* **LEGAL-34** What follows an exit in its list never runs and is dropped, and so is a `break` that ends the body of a `once`.
* **LEGAL-35** Every name a rule introduces comes from the entry point's mint: `pick_result`, `x_before`, `and_result`, `search_left`, `rows_continued`.

Guard clauses, which X2 takes without a `once`:

```raw
(let shade : float = (block $pick
    (if (local d)
      (then
        (leave $pick (lit 1.0))))
    (if (local c)
      (then
        (leave $pick (lit 2.0))))
    (leave $pick (lit 3.0)) : float))
```

```wgsl
var pick_result: f32;
if d {
    pick_result = 1.0;
} else if c {
    pick_result = 2.0;
} else {
    pick_result = 3.0;
}
let shade: f32 = pick_result;
```

A `loop:` that is a value, which X6 leaves with a plain `break`:

```raw
(let steps : float = (block $loop_value
    (loop $loop
      (assign (local w) = (call multiply (local w) (lit 2.0)))
      (if (call greater (local w) (lit 4.0))
        (then
          (leave $loop_value (local w))))) : float))
```

```wgsl
var loop_value_result: f32;
loop {
    w = w * 2.0;
    if w > 4.0 {
        loop_value_result = w;
        break;
    }
}
let steps: f32 = loop_value_result;
```

A search that leaves a block from inside a loop, which is X4 and X5 with one crossed construct:

```raw
(let shade : float = (block $search
    (for $rows i in (lit 0) ..< (lit 8)
      (assign (local weight) = (call multiply (local weight) (member (local p) a)))
      (if (call less (local weight) (lit 0.125))
        (then
          (leave $search (local weight)))))
    (leave $search (lit 0.0)) : float))
```

```wgsl
var search_result: f32;
var search_left: bool = false;
loop {
    for (var i: i32 = 0; i < 8; i++) {
        weight = weight * p.a;
        if weight < 0.125 {
            search_result = weight;
            search_left = true;
            break;
        }
    }
    if search_left {
        break;
    }
    search_result = 0.0;
    break;
}
let shade: f32 = search_result;
```

A `continue` that would cross a `once`:

```wgsl
for (var i: i32 = 0; i < 2; i++) {
    var rows_continued: bool = false;
    loop {
        if c {
            if d {
                break;
            }
            if e {
                rows_continued = true;
                break;
            }
            weight = weight + 1.0;
        }
        weight = weight * 2.0;
        break;
    }
    if rows_continued {
        continue;
    }
    total = total + weight;
}
```

## Lets

Inlining binds a callee's lets once per call, so a value the source names once is computed and named once per call.
And it hands a callee its literal arguments, so what the callee computes from them is constant where the source was not: `0 / 4`, `(0 & 64) == 64`.
Five rules run last, over the core tree, and are what keeps the text to one name per value and to no arithmetic on constants.
A target's compiler folds the copies too: these rules are for the reader of the text, and for the footprint, which reads the same tree.

* **LEGAL-58** (F1) A call of a pure builtin, a `not`, an `and` or an `or` whose operands are all literals, and whose value is an `int`, a `uint` or a `bool`, is that value's literal.
  An immutable `let` of a literal counts as its literal inside such an expression, and keeps its name everywhere else.
  The value is EVAL's, the interpreter's own evaluator's, so an integer wraps as it does at run time.
  A float result is left to the target, which rounds each step itself, and so is a call that has no value for its arguments (EVAL-84).
  A builtin that takes derivatives, reads memory, or exchanges values between invocations never folds.
* **LEGAL-59** (F2) An `if` on a `bool` literal is the side it takes, in its place, and a `while` on `false` is nothing.
  F1 runs first, so a branch on what it folded goes too, and then L2 drops what only the other side read.

* **LEGAL-56** (L1) A let whose value repeats the value of a let in scope reads that let instead, and is gone.
  The value must be the same wherever the invocation evaluates it.
  It reads no `var` and no local anything writes through, no workgroup memory, and no resource the stage may write.
  It calls no builtin that has an effect, takes derivatives, is a barrier, an atomic or a subgroup operation, or gives what differs between invocations.
  It also reads something: a local, a binding or a sampler.
  Two lets of literals alone name a number, and they keep their own names, since `metallic` and `roughness` may agree on `0.5`.
* **LEGAL-60** (L3) A `let` whose value is a block's result, the `var` E1 declared in front of the block in the same list, is that `var` under the let's name.
  It holds where nothing else reads the `var`, nothing writes it behind the `let`, and no `for`'s `end` reads the `let`, which LEGAL-7 keeps from a `var`.
  So `let lit = shade(...)` reads as one `var lit` its branches assign, rather than `shade_result` and a copy of it.
  So is a `var` given such a value, and so is the `var` a block fills before it leaves with it, such as `T[N].filled(v)`'s.
  So `let mut arr = float[8].filled(0.0)` is the one array the loop fills, rather than a filled one and a copy of it.
* **LEGAL-57** (L2) A let nothing reads is gone, and so is a let only it read.
  Its value must have no effect, and must always have a value: an element, a buffer element, a load of `bytes` and a builtin that has none for some argument are kept (EVAL-84, EVAL-90).
  A builtin that only reads is no effect here, such as a ray query's read of its candidate: it is kept from moving, never from going.
  So a read of a binding that only such a let made leaves the footprint with it.
* **LEGAL-61** (L4) A struct that is read only field by field is its fields: one `let` per field where it was built, in the same order, and each field read reads its field's `let`.
  It holds for an immutable `let` of a construction, and for a `var` E1 declared whose one assignment, a construction in the same list, comes before anything names it.
  A field built from a local nothing writes is that local, and no `let` of its own.
  Every value stays where it was computed, so the rule needs nothing to hold of what happens between the construction and a read.
  It runs after L3, which is what makes a block's result such a `var`, and L2 runs again behind it.
  So a candidate that an any-hit reads one field of computes that field alone, not the fourteen the struct holds.

```wgsl
// before L1 and L2: both calls of `tinted(ctx, …)` bind `scale`, and nothing reads `dead`
let scale: f32 = ctx.exposure * 2.0;
let r: f32 = ctx.red * scale;
let scale_1: f32 = ctx.exposure * 2.0;
let g: f32 = ctx.green * scale_1;
let dead: vec4f = frame.unused * 2.0;

// after
let scale: f32 = ctx.exposure * 2.0;
let r: f32 = ctx.red * scale;
let g: f32 = ctx.green * scale;
```

## What a target's own analysis sees

Each target's compiler analyses flow by its own rules, and it refuses text that is fine by SGL's.
These rules say why the text passes, and DXC (to DXIL and to SPIR-V) and Dawn have compiled every construct they name.

* **LEGAL-37** The text of an entry point ends in a `return`, in a loop that no `break` leaves, or in an `if` whose every branch ends so.
  [CHK-125](checking.md#returning) says so of the source, X1 writes a leave of the root as `return` where it stands, and no rule moves one.
* **LEGAL-38** So no flag test is the last thing a function can reach: a flag belongs to a block or a loop inside the body, and the body goes on behind it.
* **LEGAL-39** A loop that no `break` leaves is `while (true) { … }` and `loop { … }` with nothing behind it, which every target takes as the end of the function.
* **LEGAL-40** A result `var` has no value where it is declared, and every path to its one read assigns it.
  HLSL and MSL leave it undefined until then and WGSL zeroes it, and no target refuses the read.
* **LEGAL-41** WGSL's `once` is `loop { … break; }`, whose behaviour WGSL's analysis derives from the `break`s: it needs no `continuing` and no condition.

## Open

* An inlined body that another inlined body takes as an argument is bound at the top of the inner block, so its statements stand inside that block's `once`.
  That is right and reads worse than binding it in front would.
* Whether X2 should write statements twice where they are few, and take the `once` away.
* Whether a `continue` in tail position of its loop's body should disappear the way a leave does.
* Whether a chain of `==` over a small set of `int`s is worth turning into a `switch` where the patterns are constant but not literals, which needs folding this pass does not do.
* A pattern that has an effect, which LEGAL-47 refuses.
  It wants either a second expression pass behind C1, or C1 in front of the expression rules with a way to reach a `case` inside a block expression.
* GLSL, whose column above has met no emitter.
