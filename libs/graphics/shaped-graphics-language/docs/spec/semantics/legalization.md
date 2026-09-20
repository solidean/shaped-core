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
* **LEGAL-3** `break` ends the innermost enclosing `once` or loop.
* **LEGAL-4** `continue` names the innermost enclosing loop, and no `once` stands between the two ([why](why/legalization.md#legal-4)).
* **LEGAL-5** `return` is legal at any depth: it is the exit of the root block.
* **LEGAL-6** The right operand of `and` / `or` has no effect ([why](why/legalization.md#legal-6)).
* **LEGAL-7** The `end` of a `for` has no effect and reads no mutable local, since a target evaluates it before every iteration.
* **LEGAL-8** A tree that is core already is left as it is.
* **LEGAL-9** An emitter refuses a tree that is not core with the error `not-core`, whose detail names the first violation; it never asserts.

## The table

One row per core construct, one column per target; GLSL has no emitter yet, and its column is what it will print.

| core | HLSL | WGSL | MSL | GLSL |
|---|---|---|---|---|
| `once { … }` | `do { … } while (false);` | `loop { … break; }` | `do { … } while (false);` | `do { … } while (false);` |
| `loop { … }` | `while (true) { … }` | `loop { … }` | `while (true) { … }` | `while (true) { … }` |
| `while c { … }` | `while (c) { … }` | `while c { … }` | `while (c) { … }` | `while (c) { … }` |
| `for i in a ..< b { … }` | `for (int i = a; i < b; ++i) { … }` | `for (var i: i32 = a; i < b; i++) { … }` | `for (int i = a; i < b; ++i) { … }` | `for (int i = a; i < b; ++i) { … }` |
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

## Expressions

The rules run cheapest first, and each costs what its line says.

* **LEGAL-14** (E1) A block expression moves in front of the statement that holds it, and its value is a `var` without a value that its leaves assign.
  It costs one `var`.
* **LEGAL-15** (E1) A block whose only leave is its last statement needs no `var`: its statements move, and its value stays where the block stood.
  It costs nothing.
* **LEGAL-16** (E2) Before a block moves, every operand to its LEFT in evaluation order is **pinned** into a `let`.
  It costs one `let` per pinned operand.
* **LEGAL-17** (E2) An operand is left unpinned when it has no effect AND the moved statements assign no local it reads.
* **LEGAL-18** So a literal, an immutable local and a member of one are never pinned, and a read of a `var` that the block assigns always is.
* **LEGAL-19** (E3) `and` / `or` whose right operand moved statements, or has an effect, is an `if` over a `var`; any other stays `&&` / `||`.
  It costs one `var` and one `if`.
* **LEGAL-20** (E4) A `while` whose condition moved statements is a `loop` that starts with them and with `if not c { break }`.
  It costs one `if`.
* **LEGAL-21** The test stands at the top of the body, so a `continue` still meets the condition before the next iteration.
* **LEGAL-22** The `end` of a `for` that LEGAL-7 refuses is pinned in front of the loop, and then `first` is pinned too when it has an effect, so the two keep their order.

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
* `switch`, which will capture `break` like a loop, so an exit that crosses one takes a flag too.
* GLSL, whose column above has met no emitter.
