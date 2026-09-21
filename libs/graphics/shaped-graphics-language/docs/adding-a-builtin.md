# Adding a builtin

A builtin is one record in the C++ **builtin registry**, and adding one touches that record and nothing else.
The check pass, the interpreter, every emitter and the layout check look the record up; none of them holds a list of builtins.
The reasons are in [why/checking.md](spec/semantics/why/checking.md#chk-138) and [why/emitting.md](spec/semantics/why/emitting.md#emit-74).

## Where things live

| what | where |
|---|---|
| the record types, and the registry | [builtins/registry.hh](../src/shaped-graphics-language/builtins/registry.hh) |
| the topics, called in a fixed order | [builtins/register.hh](../src/shaped-graphics-language/builtins/register.hh), and one `register_*.cc` per topic |
| the generated prelude, committed | [prelude/builtins.sgl](../prelude/builtins.sgl) |
| the hand-written prelude | [prelude/core.sgl](../prelude/core.sgl) |

**First ask whether it needs C++ at all.**
A function that can be written in terms of what `builtins.sgl` declares belongs in `core.sgl`, as ordinary SGL with a body.
It is inlined like any function of a program, so it costs an emitter nothing.
A record is for what SGL cannot say: an opaque type, or a function some target spells in its own way.

## A function

Say `fract`, the fractional part of a float, which HLSL calls `frac`.
It goes into the topic it belongs to, here `register_scalar_math` or the componentwise family of `register_vector_math`.

```cpp
void fract(cc::span<check::scalar const> in, cc::vector<check::scalar>& out)
{
    for (auto const& leaf : in)
        out.push_back(check::scalar::of(leaf.as_float() - floor_of(leaf.as_float())));
}

// inside the loop over {"float", "float3", "float4"}:
add_function(r, "fract", {"x", type}, type, fract, {.hlsl = "frac"}, "/// What is left of `x` behind the point.");
```

That is the whole change to the C++.
Then regenerate the committed file, which the `sgl-prelude` step of `dev.py check` otherwise refuses:

```bash
uv run dev.py check sgl-prelude --fix
```

What each part of the record is for:

* **The signature is SGL source text.**
  `add_function` formats `@pure fun fract(x: float3) -> float3`, and the generator puts `@builtin` in front.
  The normal parser reads it; the registry has no signature language of its own, and it reads the name and the parameter types back from that parse.
* **The key is the name together with the parameter types**, so every overload is a record of its own.
  Until SGL has generics, a family of overloads is a C++ loop that formats one signature per type.
* **The evaluator is what the interpreter runs.**
  It is given the scalars of every argument, one argument behind the other, and appends the scalars of the result.
  The interpreter has checked both against the record's types, so an evaluator checks nothing.
  One evaluator serves a whole family when it reads its width off the number of scalars it was given.
* **The spelling is how every target writes a call.**
  The default is a call of the SGL name; `.hlsl`, `.wgsl` and `.msl` rename it in one language.
  `infix("+")` is an operator at its usual level, `spelling_kind::prefix` a prefix operator.
  `spelling_kind::custom` is a function that writes the call itself, for the few that are no call and no operator.
  `transform_position` is one: it widens its position by `1.0`, and HLSL writes the product as `mul(m, v)`.
* **`@pure` stands in the signature**, and a record without it is a function with an effect, which the legalizer then keeps in its place.
* **The name a target calls is reserved in that target from the record**, so a local named `frac` is renamed in HLSL without an entry in any list.
  The exception is a function only a custom writer calls, such as `mul`, which stands in [reserved_words.cc](../src/shaped-graphics-language/emit/reserved_words.cc).

`add_infix`, `add_negate` and `add_function` in `register.hh` cover everything the prelude has today.
A record that fits none of them is built by hand and added with `r.add(function_record{…})`.

## A type

A type record carries its declaration as SGL text, opaque or with fields, and what every target needs to know about it.

* **Its name per target**: `float3`, `vec3f`, `float3`.
* **Its size and alignment in a constant block, per target**, which is what the `layout-mismatch` check of an `@inline binding` compares.
  A size of 0 means the type has no place in a block, which is `bool` today.
* **What a value is to the interpreter**: how many scalars, and of which kind.
* **Whether it may be a member of a struct that crosses a stage edge.**

## What the check is

The registry is the source of truth, and `prelude/builtins.sgl` is what it generates, committed so that the prelude can be read and its changes reviewed.
The compiler never opens that file: it generates the same text in memory.
Two things keep the two from drifting apart:

* the `sgl-prelude` step of `uv run dev.py check`, which builds the `sgl` tool and runs `sgl prelude --check` on the file, and `--write` under `--fix`;
* the test `sgl driver - the prelude is the generated builtins and the hand-written core`, which compares the same two texts without the tool.

The test `sgl builtins - one record is all a new builtin takes` adds `fract` to a registry of its own.
It checks that the record alone gets it checked, run and written for every target, so this page stays true.
