# Why: tokens

The reasons behind the rules of [tokens.md](../tokens.md).
Nothing here is normative.

## TOK-6

A tokenizer that is handed a mode and hands modes on is a pure function of one line.
That is what keeps the error reach of [LINE-23](line-tree.md#line-23): a line tells its children how to be read and its next sibling how to start, and tells nobody else anything.
A conventional lexer carries its state across the whole file, which is exactly how one missing quote flips every later line.

## TOK-11

Max-munch means the tokenizer has no table of known words and no context.
A new operator, a new keyword or a new literal suffix never changes it.
The rules are inclusive on purpose: the tokenizer accepts more than the language means, and the later phases reject with a better message than a tokenizer could give.

## TOK-15

`10a7`, `@range`, `let`, `#ff00bb` and `\phi_1` are all the same kind of token.
Telling a keyword from an identifier needs the keyword table, and the table is data of a later phase.
Telling a number from an identifier needs rules about prefixes, separators and suffixes, which are better reported as one `malformed-number` than as a confusing split into three tokens.
`@` and `#` are symbol characters so that an attribute and a hash literal such as `#rgb` are one token each.
`\` is one so that `\phi` can be typed on any keyboard and turned into `φ` by [notation](../../notation.md).
`'` continues a symbol and never starts one: `x'` reads as "x prime", `1'000` separates digits, and a leading `'` stays free to open a quoted literal.

## TOK-16

Names in a shader are mirrored into the host language: a binding `frame_data` becomes a struct in C++.
No host language we target can spell a dash in a name, so a name with a dash would need a second, mangled spelling, and the two would drift.
snake_case is the convention on both sides.
An earlier idea was the opposite, `a-b` as one symbol, as in Lisp, with subtraction always needing spaces.
The second half of that idea survives: [OP-22](operators.md#op-22) makes `a-b` an error and not a subtraction.
So the spelling `a-b` means nothing today, and it could still be given to identifiers later without breaking a program.

## TOK-19

SGL has no `::`.
`.` reaches a member of a value, of a type and of a module alike, and modules do the job that namespaces do elsewhere.
One accessor means one thing to learn, and a refactoring from a module constant to a struct member does not touch the use sites.
The token exists for one reason: everyone who arrives from C++, Rust or HLSL types `color::red` in the first hour.
With a token of its own, the diagnostic says "SGL uses `.`" and offers the fix.
Without it, the reader would see two ascription colons and a baffling message.

## TOK-22

An operator is any run of operator characters, so `<<=`, `..<` and an operator nobody has thought of yet are all one token.
Precedence comes from the first character ([OP-5](operators.md#op-5)), so adding an operator to the language, or later letting a user declare one, never touches the tokenizer.
The price is that `a*-b` is the single unknown operator `*-`.
The spacing rule wants `a * -b` anyway.
The run stops before `//` because a comment directly after an operator is ordinary, and `+//` is not an operator anyone wants.

## TOK-31

`a.b`, `a .b` and `a . b` are different things, and so are `f(x)` and `f (x)`.
The first draft had a synthetic FUSED token in the stream before a dot, a colon or an opener.
That puts a fake token into a stream that should reproduce the source, and it makes parenthesis matching step around it.
Fusedness is already in the byte spans: two tokens are fused when one ends where the next begins.
Only group tokens have to store it, because a continuation line changes it for a leading dot ([GRP-25](groups.md#grp-25)).
A colon is the one place where being fused means nothing, so that `x: int` and `x : int` are both fine.

## TOK-35

`1.5` cannot be one token without the tokenizer knowing what a number is, and then `1.max(2)`, `1..<4` and `t.0.1` each need a special case in the tokenizer.
Left as symbol, dot, symbol, all four read correctly by one small rule in the form phase, where being fused is known ([NUM-4](numbers.md#num-4)).
The same holds for `1e-5`: the `-` is an ordinary operator token that the form phase absorbs when the symbol before it ends in an exponent marker.
