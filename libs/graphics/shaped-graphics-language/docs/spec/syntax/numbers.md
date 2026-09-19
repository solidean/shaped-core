# Numbers

A number is not a token: the form phase assembles it from fused tokens.
Back to the [phases](_index.md); the reasons are in [why/numbers.md](why/numbers.md).

## Assembly

* **NUM-1** A number literal starts at a symbol that starts with a digit, in a position where an operand can start ([why](why/numbers.md#num-1)).
* **NUM-2** A number literal is, in order and all fused: the first symbol, an optional DOT, an optional fraction symbol, and an optional signed exponent.
* **NUM-3** The **fraction symbol** is a symbol that starts with a digit.
* **NUM-4** The DOT is part of the literal when no symbol is fused after it, or when a fraction symbol is ([why](why/numbers.md#num-4)).
* **NUM-5** A DOT with any other symbol fused after it is member access on the number before it.
* **NUM-6** The **signed exponent** is the operator `+` or `-` and a symbol that starts with a digit, and it exists only when the part before it ends in an exponent marker.
* **NUM-7** A symbol directly after a fused DOT of a member access never starts a number ([why](why/numbers.md#num-7)).
* **NUM-8** Two dots are an operator by [TOK-24](tokens.md#operators), so they are never part of a number.
* **NUM-9** A prefix `-` or `+` directly on a number literal is part of the literal ([why](why/numbers.md#num-9)).

| source | reads as |
|---|---|
| `1.5` | one float literal |
| `1.` | one float literal |
| `1e-5` | one float literal: `1e`, `-`, `5` |
| `1.5e+3f32` | one float literal: `1`, DOT, `5e`, `+`, `3f32` |
| `1.max(2)` | call of: member `max` of the integer `1` |
| `1..<4` | the range operator `..<` between `1` and `4` |
| `t.0.1` | member `1` of: member `0` of `t` |
| `-3` | one integer literal |
| `- 3` | not a literal: a `-` that is no prefix operator ([OP-23](operators.md#spacing)) |

```sgl
let a = 1
let b = 1.
let c = 1.0
let d = 1e6
let e = 2.5e-3
let f = -3
let g = 1.max(2)
let h = pair.0
```

## Inside the symbols

* **NUM-10** The first symbol may start with the prefix `0x` for hexadecimal or `0b` for binary.
* **NUM-11** `'` separates digits, and it is never the first character, the last character, or next to another `'`.
* **NUM-12** `_` inside a number is the normal error `underscore-in-number`, with a fix that writes `'` ([why](why/numbers.md#num-12)).
* **NUM-13** The exponent marker is `e` in a decimal number, and `p` in any number.
* **NUM-14** The exponent is decimal digits, after an optional sign by NUM-6.
* **NUM-15** A number may end in a suffix: `i`, `u` or `f`, and a bit width.
* **NUM-16** A hexadecimal number without an exponent takes no `f` suffix, because `f` is a digit there.
* **NUM-17** A number with a DOT or an exponent is a float literal, and any other number is an integer literal.
* **NUM-18** The bit width is not checked here; a later phase validates it.
* **NUM-19** A symbol that starts with a digit and fits none of these rules is the normal error `malformed-number`, and it still reads as a number literal.

```sgl
let mask = 0xff00'00ff
let bits = 0b1010'0101
let big = 1'000'000
let scale = 1p8
let half = 0.5f32
let count = 100u32
let small = 18u8
```

`1_000` reports `underscore-in-number`, and it reads as `1'000`.

```sgl error
let big = 1_000
```

`10a7` starts with a digit and is no number, so it reports `malformed-number`.

```sgl error
let x = 10a7
```

## Open

* Whether the bit width of a suffix is optional, as in `1f` or `10u`.
* Whether hexadecimal digits, the prefixes and the exponent markers accept upper case.
* Whether a hexadecimal number takes a fraction: by NUM-3 `0x1.8p1` assembles and `0x1.ap1` is member access, because `ap1` starts with a letter.
* What the exponent `p` means in a decimal number such as `1p8`.
