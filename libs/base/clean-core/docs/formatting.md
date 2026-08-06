# Formatting

`cc::format` and the three ways clean-core turns a value into text.

This page owns the placeholder grammar, the customization protocol, and what the compile-time check does and does not cover.
Per-function documentation lives in the headers; the [cheat-sheet](../cheat-sheet.md) lists the members.

## Which one to reach for

| you want | use | header |
|---|---|---|
| one value as a string | `cc::to_string(v)` | `string/to_string.hh` |
| several values composed into a string | `cc::format("{} of {}", a, b)` | `string/format.hh` |
| a value in a diagnostic, whatever its type | `cc::to_debug_string(v)` | `string/to_debug_string.hh` |
| any of the above on stdout or stderr | `cc::print` / `println` / `eprint` / `eprintln` | `string/print.hh` |

`to_string` is an overload set over the built-in types only, so an unsupported type is a plain overload-resolution error.
Numbers go straight through `std::to_chars` into a stack buffer.
On 64-bit every result a number can produce fits in a `cc::string`'s inline storage, so that path does not allocate.

`format` composes, validates its format string at compile time, and is the extensible one.

`to_debug_string` is for diagnostics and nothing else — **its output is explicitly unstable**, may be lossy, and may differ between builds.
It accepts anything: it falls back through `to_string`, a member `to_string()`, containers, tuples, and finally a raw byte dump.
Never parse it, and never put it in a user-facing message.

`cc::format` has three entry points:

* `format(fmt, args...)` returns a fresh `cc::string`;
* `format_append(out, fmt, args...)` appends to one you already have — also spelled `out.appendf(fmt, args...)`;
* `format_to(span<char> out, fmt, args...)` writes into a caller-provided buffer and allocates nothing.

`format_to` follows `snprintf` semantics: it writes at most `out.size()` bytes and returns how many it *would* have written, so a return value above `out.size()` means truncation.
It never null-terminates.

## The placeholder grammar

A replacement field is `{` `[arg_index]` `[':' format_spec]` `}`, and `{{` / `}}` produce a literal brace.

```text
format_spec ::= [[fill] align] [sign] ['#'] ['0'] [width] [grouping] ['.' precision] [type]
align       ::= '<' | '>' | '^'
sign        ::= '+' | '-' | ' '
grouping    ::= one digit-group separator
```

A fill character is only recognized when an alignment character follows it, so `{:*>8}` pads with `*` while `{:*}` is a grouping separator.

Argument indices are either all automatic or all explicit — `"{} {1}"` is a compile error, not a fallback.

A grouping separator is a single non-alphanumeric character, read at that one position in the spec rather than anywhere in it.
Alignment, sign and `#` have already been consumed by then, so `<`, `>`, `^`, `+`, `-`, space and `#` never reach it, and `.`, `{` and `}` are excluded outright.
Decimal digits group by 3 and binary / hex / octal by 4; a float groups only its integer part, always by 3.
So `{:'}` on `1232453254` gives `1'232'453'254`.

Precision is **maximum length** for strings — `{:.3}` on `"abcdef"` yields `abc`.
For floats it is digits after the point, but **only alongside an explicit `f`/`e`/`g` presentation type**.
With no type a float is rendered shortest-round-trip and the precision is ignored outright, so `{:.2}` on a double changes nothing.

```cpp
cc::format("{} + {} = {}", 1, 2, 3);   // "1 + 2 = 3"
cc::format("{:#06x}", 255);            // "0x00ff"
cc::format("{:>8.2f}", 3.14159);       // "    3.14"
cc::format("{:'}", 1232453254);        // "1'232'453'254"
```

### What each type accepts

| argument | presentation types | also rejected |
|---|---|---|
| integers (and `cc::byte`) | `d` `b` `B` `o` `x` `X` `c` | precision |
| `char` | `c` `d` `b` `B` `o` `x` `X` | precision, grouping |
| `bool` | `s` `d` `b` `B` `o` `x` `X` | precision, grouping |
| `float` / `double` | `f` `F` `e` `E` `g` `G` | — |
| string-likes | `s` | sign, `#`, `0`, grouping |
| pointers | `p` | sign, `#`, `0`, precision, grouping |
| a user type reaching the member `to_string()` tier | none — the empty `{}` only | everything |

Omitting the presentation type gives each its default.
That is shortest round-trip for floats, `true`/`false` for `bool`, the character itself for `char`, and `0x` plus two uppercase hex digits for `cc::byte`.
`{:c}` on an integer emits it as a character instead.

## Making a type formattable

`cc::format` performs **no ADL** on its arguments, so a free `to_string` in your type's namespace will not be found.
Each argument is dispatched in this order:

1. a `cc::custom::formatter<T>` specialization, checked first — it wins even for a built-in type;
2. a built-in type, rendered by `cc::format_value` with the grammar above;
3. a member `T::to_string()` convertible to `string_view`, which accepts the empty `{}` spec only.

Anything else is a `static_assert`.

A `cc::custom::formatter<T>` receives the raw spec text verbatim, so a type may define its own spec language entirely.
It provides `format` and, optionally, `validate`:

```cpp
template <>
struct cc::custom::formatter<my_vec2>
{
    static consteval void validate(cc::string_view spec)
    {
        if (!spec.empty())
            throw "my_vec2 takes no format spec";
    }
    static void format(cc::format_sink out, cc::string_view /*spec*/, my_vec2 const& v)
    {
        out.put("(");
        cc::format_value(out, "", v.x); // reuse the standard grammar for the components
        out.put(", ");
        cc::format_value(out, "", v.y);
        out.put(")");
    }
};
```

Two helpers exist so a custom formatter can reuse the standard grammar rather than reimplement it:
`cc::format_value(out, spec, v)` renders a built-in value at runtime, and `cc::validate_format_spec(spec)` checks the spec's syntax at compile time.
Both come with `<clean-core/string/format.hh>`.

`cc::format_sink` is a context pointer plus a write function, so it implies no allocation of its own.
The same sink type backs both `cc::format` and `cc::format_to`; only the string-backed one grows a `cc::string` behind it.

Formatting is the one customization point that deliberately skips the ADL tier the rest of `cc::custom::` offers, so that `cc::format` does not trigger ADL over every argument on every call.
[customization-points](customization-points.md) owns that convention.

## What the compile-time check covers

The format string is a `consteval` `cc::format_string<Args...>`, constructed implicitly from a literal, and it validates in two passes:

* **structure**, independent of the arguments — brace matching, `{{` / `}}` escapes, that every index is in range, and that automatic and explicit indexing are not mixed;
* **each field's spec against the type of the argument it names**, using the table above.

A malformed call is therefore a compile error rather than a runtime failure, and the runtime render loop re-uses the very same parser so the two can never disagree.

Two gaps are worth knowing:

* **Omitting `validate` opts a type out.** A `cc::custom::formatter<T>` without one gets no compile-time spec checking at all, and whatever `format` does with a bad spec is what happens.
* **The compile-fail cases are not enforced by CI.**
  `tests/string/format-test.cc` carries them behind `CC_FORMAT_COMPILE_FAIL_TESTS`, which is off by default, so they only run when someone turns them on by hand.

At runtime `cc::impl::format_error` routes through the clean-core assertion handler.
Reaching it means the string was not validated, so it is a defensive path rather than an error channel — there is no runtime "bad format string" result to handle.

## Printing

`cc::print` and `cc::eprint` write to stdout and stderr, each in two forms: a `string_view` written verbatim, or a `cc::format` string plus arguments.
Braces in the verbatim form are **not** interpreted.

The `println` / `eprintln` variants append `'\n'` and always flush, so line-oriented output survives being redirected or piped.
`print` / `eprint` do not flush.
For buffered line output, use `print` and append your own newline.

Output goes through `std::fwrite`; there is no `<iostream>` and no locale handling anywhere in clean-core's formatting.

## The value→text seam

Every number ultimately reaches text through `std::to_chars`, and clean-core confines that to two places.
The `format_chars_*` definitions in `string/format.cc` cover the format path, and `string/to_string.cc` covers the `to_string` overloads.
Replacing the number-formatting backend means replacing those definitions and nothing else.

The rest of `impl/format_backend.hh` layers decoration — sign, prefix, padding, alignment, grouping — on top of the raw digits, and the grammar itself lives in `impl/format_spec.hh`.
Both are private; nothing outside `string/` should include them.
