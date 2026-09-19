# Operators

This file holds the precedence ladder of the [form phase](forms.md), the operator table, and the rules for spacing and mixing.
Back to the [phases](_index.md); the reasons are in [why/operators.md](why/operators.md).

## The precedence ladder

* **OP-1** A run of group tokens is read by this ladder, where level 1 binds loosest and level 15 binds tightest.

| level | construct | associates | example |
|---|---|---|---|
| 1 | sequence `;` | | `a; b` |
| 2 | assignment operators | right | `let x : int = 10` |
| 3 | computes-as `=>` | right | `fun f(x: int) -> int => x + 1` |
| 4 | [keyword form](forms.md#keyword-forms) | | `assert a == b, "msg"` |
| 5 | `and` `or` `not` | left | `a and not b` |
| 6 | comparisons `<` `<=` `==` `!=` `>=` `>` | chained | `0 <= i < n` |
| 7 | `:` `->` `as` `in` | left | `x as int in bounds : bool` |
| 8 | ranges `..<` `..=` | none | `0..<n + 1` |
| 9 | bit-like `&` `\|` `^` `<<` `>>` | left | `(x >> 16) ^ x` |
| 10 | add-like `+` `-` | left | `a + b - c` |
| 11 | mul-like `*` `/` `%` | left | `a * b / c` |
| 12 | [application](forms.md#application) | | `cross a b` |
| 13 | prefix and postfix operators | | `-x`, `~x`, `..x` |
| 14 | [postfix forms](forms.md#postfix-forms): calls and member access | | `f(x).y[i]` |
| 15 | atoms | | `x`, `1.5`, `(a, b)` |

* **OP-2** `and`, `or` and `not` share one level, `:` `->` `as` `in` share one level, and the bit-like operators share one level ([why](why/operators.md#op-2)).
* **OP-3** `=>` binds looser than a keyword form and tighter than assignment ([why](why/operators.md#op-3)).
* **OP-4** The bit-like operators bind tighter than comparisons ([why](why/operators.md#op-4)).

| source | reads as |
|---|---|
| `a & mask == 0` | `(a & mask) == 0` |
| `0..<n + 1` | `0..<(n + 1)` |
| `a + b as float` | `(a + b) as float` |
| `not a == b` | `not (a == b)` |
| `-f(x).y` | `-((f(x)).y)` |
| `let g = fun (x) => x + 1` | `let g = ((fun (x)) => (x + 1))` |

## The operator table

* **OP-5** The level of an infix operator comes from its first character ([why](why/operators.md#op-5)).

| first character | level |
|---|---|
| `*` `/` `%` | 11, mul-like |
| `+` `-` | 10, add-like |
| `&` `\|` `^` | 9, bit-like |
| `<` `>` | 9, bit-like, unless the operator is a comparison |
| `..` | 8, range |

* **OP-6** The comparisons are exactly `<`, `<=`, `==`, `!=`, `>=` and `>`.
* **OP-7** An operator that ends in `=` and is not a comparison is an assignment operator, whatever its first character.
* **OP-8** The range operators are `..<` and `..=`, and an infix `..` alone is the normal error `bare-range`.
* **OP-9** The prefix operators are `-`, `+`, `~` and the splat `..`.
* **OP-10** An operator the language does not define is the normal error `unknown-operator`, and it is placed by OP-5 and OP-7.
* **OP-11** `^` is exclusive or, and there is no exponent operator ([why](why/operators.md#op-11)).
* **OP-12** `!` alone and every operator that starts with `?` are reserved, and they are the normal error `reserved-operator`.
* **OP-13** `and`, `or`, `not`, `as` and `in` are **word operators**: symbols that the keyword table marks as operators.
* **OP-14** `:`, `->` and `=>` are operators of the ladder, and they are not operator tokens.
* **OP-30** The prefix `..` is the splat, and where it may stand is decided by the AST ([AST-28](ast.md#lists), [why](why/operators.md#op-30)).
* **OP-31** The postfix `..` is reserved by OP-24, and it is kept free for a half-open range ([why](why/operators.md#op-31)).

```sgl
let h = (x >> 16) ^ x
let masked = a & mask == 0
let n = -x + ~y
count += 1
bits <<= 2
let f = i as float
let wide = (..normal, 0)
```

`!` is reserved, so the line below reports `reserved-operator`; it is written `not ready`.

```sgl error
let waiting = !ready
```

The `..` below is a postfix operator, so the line reports `reserved-operator`; the splat is written `..normal`.

```sgl error
let wide = (normal.., 0)
```

## Mixing

* **OP-15** Two different operators of level 5, or of level 9, in one run without parentheses are the normal error `mixed-operators`, and the run reads left to right ([why](why/operators.md#op-15)).
* **OP-16** A `not` is allowed on the last operand of a run of `and` or `or`, and on an operand that stands alone; any other `not` is the normal error `misplaced-not`.
* **OP-17** A comparison chain reads as the pairwise comparisons joined by `and`, with each operand evaluated once.
* **OP-18** A comparison chain must be monotone: only `<`, `<=` and `==`, or only `>`, `>=` and `==`.
* **OP-19** A `!=` stands in no chain with another comparison.
* **OP-20** A chain that breaks OP-18 or OP-19 is the normal error `non-monotone-comparison`, and it still reads by OP-17.
* **OP-21** A range operator does not associate: two in one run are the normal error `chained-range`.

| source | result |
|---|---|
| `a and not b` | accepted |
| `a and b or c` | `mixed-operators`, reads as `(a and b) or c` |
| `not a and b` | `misplaced-not` |
| `a & b \| c` | `mixed-operators` |
| `a < b <= c` | accepted |
| `a == b == c` | accepted |
| `a <= b == c < d` | accepted |
| `a < b > c` | `non-monotone-comparison` |
| `a != b != c` | `non-monotone-comparison` |

```sgl
let inside = 0 <= i < n and not hidden
let same = a == b == c
let flags = (a & b) | c
```

```sgl error
let either = a and b or c
```

## Spacing

* **OP-22** The spacing around an operator token decides its role ([why](why/operators.md#op-22)).

| left side | right side | role |
|---|---|---|
| whitespace | whitespace | infix |
| whitespace, an opener, a comma or the start of the line | fused | prefix |
| fused | whitespace, a closer, a comma or the end of the line | postfix |
| fused | fused | the normal error `operator-needs-spaces`, read as infix; a range operator is exempt by OP-28 |

* **OP-23** An operator with whitespace on its right side is never a prefix operator: `- a` is not a negation.
* **OP-24** Postfix operators are reserved, and one is the normal error `reserved-operator`.
* **OP-25** `:`, DOT, COMMA and the word operators are not operator tokens, and they owe no spaces: `a: float` is accepted.
* **OP-26** A prefix operator that OP-9 does not list and OP-12 does not reserve is the normal error `unknown-operator`.
* **OP-28** An operator that starts with `..` may be fused on both sides, and it is infix there: `0..<4` and `0 ..< 4` read the same ([why](why/operators.md#op-28)).
* **OP-29** `->` and `=>` are always infix, and one that is fused on both sides is the normal error `operator-needs-spaces` ([why](why/operators.md#op-29)).

| source | reads as |
|---|---|
| `a - b` | subtraction |
| `foo -a` | `foo` applied to `-a` |
| `(-a, -b)` | two negations |
| `(..a, b)` | the splat of `a`, and `b` |
| `(a.., b)` | `reserved-operator` |
| `a-b` | `operator-needs-spaces`, then subtraction |
| `a->b` and `x=>y` | `operator-needs-spaces`, then `->` and `=>` |
| `0..<4` and `0 ..< 4` | the same range |
| `x: int` and `x : int` | the same ascription |

```sgl
let d = a - b
let m = max(-a, -b)
let s = scale -a
let r = 0..<4
let t = 0 ..< 4
for i in first..=count:
    total += i
```

`a-b` reports `operator-needs-spaces`.

```sgl error
let d = a-b
```

`x=>y` reports `operator-needs-spaces`; it is written `x => y`.

```sgl error
let twice = x=>x * 2
```

## Sequence

* **OP-27** `;` separates forms on one line, outside paren groups only ([FORM-39](forms.md#composites-and-sequences)).

## Open

* The level of an infix operator that starts with `~`, `!`, `=` or `?` and is no comparison and no assignment.
* Whether an operator directly after `:`, `->` or `=>` without whitespace, as in `x:-1`, counts as a prefix operator.
* User-declared operators, prefix and postfix alike, are a planned extension.
