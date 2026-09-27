#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Generate typed-geometry's loop-free fixed_int specializations and their golden-vector tests.

Run it as an explicit step after changing the rule or the emitted code, never from the build:

    uv run libs/base/typed-geometry/tools/gen-fixed-int.py --write
    uv run libs/base/typed-geometry/tools/gen-fixed-int.py --check    # exit 2 when a committed file differs

`dev.py check` runs --check (and --write under --fix), so a hand edit to a generated file or a script change nobody
re-ran is caught before commit.
The output is clang-formatted here, so `dev.py format` leaves it alone.

What is generated, per result width up to --max-bits, signed and unsigned together in generated/w<R>.hh:
- the same-width add, sub, mul, shl and shr every operator runs;
- every (R, A, B) triple of tg::add / sub / mul that the bound rule below admits.

The bound rule, per signedness: an operand of width W holds values that need W bits, the smallest of them one past
the previous width's range (1 for the narrowest width).
Combining the two operands' smallest magnitudes gives the low bound on the result, their largest the high bound, and
every width from the one the low bound needs to the one the high bound needs gets a body, capped at --max-bits.
`sub` uses `add`'s magnitudes; unsigned `sub` only ever gets R = max(A, B), since a wider unsigned difference is either
unchanged or wrapped.
libs/base/typed-geometry/docs/modules/scalar.md has the reasoning.
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIB = HERE.parent
REPO = LIB.parents[2]
GENERATED = LIB / "src" / "typed-geometry" / "scalar" / "fixed_int" / "generated"
GOLDEN = LIB / "tests" / "scalar" / "fixed_int-golden.gen.cc"
COMMAND = "uv run libs/base/typed-geometry/tools/gen-fixed-int.py --write"

# --- the triple table ------------------------------------------------------------------------------------------


def widths(max_bits: int) -> list[int]:
    return [32] + list(range(64, max_bits + 1, 64))


def value_range(w: int, signed: bool) -> tuple[int, int]:
    return (-(1 << (w - 1)), (1 << (w - 1)) - 1) if signed else (0, (1 << w) - 1)


def width_for(values: list[int], signed: bool, candidates: list[int]) -> int | None:
    for w in candidates:
        lo, hi = value_range(w, signed)
        if all(lo <= v <= hi for v in values):
            return w
    return None


def operand_extremes(w: int, ws: list[int], signed: bool) -> tuple[list[int], list[int]]:
    """The smallest magnitudes that need w bits, and w's own extremes."""
    i = ws.index(w)
    lo, hi = value_range(w, signed)
    if i == 0:
        small = [1, -1] if signed else [1]
    else:
        plo, phi = value_range(ws[i - 1], signed)
        small = [phi + 1, plo - 1] if signed else [phi + 1]
    big = [lo, hi] if signed else [hi]
    return small, big


def result_widths(op: str, a: int, b: int, signed: bool, max_bits: int) -> list[int]:
    ws = widths(max_bits)
    if op == "sub" and not signed:
        return [max(a, b)]
    candidates = widths(2 * max_bits)
    combine = (lambda x, y: x * y) if op == "mul" else (lambda x, y: x + y)
    sa, ba = operand_extremes(a, ws, signed)
    sb, bb = operand_extremes(b, ws, signed)
    low = [combine(x, y) for x in sa for y in sb]
    high = [combine(x, y) for x in ba for y in bb]
    if signed:
        # a bound is a magnitude: the low one must hold both signs of it
        m = max(abs(v) for v in low)
        low = [m, -m]
    lo_w = width_for(low, signed, candidates)
    hi_w = width_for(high, signed, candidates)
    return [w for w in candidates if lo_w <= w <= hi_w and w <= max_bits]


def triple_table(max_bits: int) -> list[tuple[str, int, int, int, bool]]:
    """Every (op, R, A, B, signed) that gets a specialization, both operand orders included."""
    table: set[tuple[str, int, int, int, bool]] = set()
    ws = widths(max_bits)
    for signed in (True, False):
        for w in ws:  # the same-width operators
            for op in ("add", "sub", "mul"):
                table.add((op, w, w, w, signed))
        for i, a in enumerate(ws):
            for b in ws[i:]:
                for op in ("add", "sub", "mul"):
                    for r in result_widths(op, a, b, signed, max_bits):
                        table.add((op, r, a, b, signed))
                        table.add((op, r, b, a, signed))
    return sorted(table, key=lambda t: (t[1], not t[4], t[0], t[2], t[3]))


# --- C++ emission ----------------------------------------------------------------------------------------------


def word_count(bits: int) -> int:
    return 1 if bits == 32 else bits // 64


def ty(bits: int, signed: bool) -> str:
    return f"fixed_integer<{bits}, {'true' if signed else 'false'}>"


def operand_words(name: str, bits: int, signed: bool) -> tuple[list[str], list[str], str]:
    """Declarations of the operand's words, their names, and the name of its sign fill past the top."""
    decls = []
    names = []
    if bits == 32:
        expr = f"u64(i64(i32({name}.limbs[0])))" if signed else f"u64({name}.limbs[0])"
        decls.append(f"u64 const {name}0 = {expr};")
        names.append(f"{name}0")
    else:
        for i in range(bits // 64):
            decls.append(f"u64 const {name}{i} = {name}.limbs[{i}];")
            names.append(f"{name}{i}")
    if signed:
        decls.append(f"u64 const {name}_fill = u64(i64({names[-1]}) >> 63);")
        fill = f"{name}_fill"
    else:
        fill = "u64(0)"
    return decls, names, fill


def result_store(r: int, signed: bool, words: list[str]) -> list[str]:
    lines = [f"{ty(r, signed)} r;"]
    if r == 32:
        lines.append(f"r.limbs[0] = u32({words[0]});")
    else:
        for i, w in enumerate(words):
            lines.append(f"r.limbs[{i}] = {w};")
    lines.append("return r;")
    return lines


def body_add_sub(op: str, r: int, a: int, b: int, signed: bool) -> list[str]:
    da, na, fa = operand_words("a", a, signed)
    db, nb, fb = operand_words("b", b, signed)
    rw = word_count(r)
    lines = da + db
    fn = "cc::add_with_carry" if op == "add" else "cc::sub_with_borrow"
    flag = "carry" if op == "add" else "borrow"
    sym = "+" if op == "add" else "-"
    out = []
    prev = None
    for i in range(rw):
        x = na[i] if i < len(na) else fa
        y = nb[i] if i < len(nb) else fb
        if i == rw - 1:
            # the last word needs no carry out
            tail = f" {sym} s{prev}.{flag}" if prev is not None else ""
            lines.append(f"u64 const w{i} = {x} {sym} {y}{tail};")
            out.append(f"w{i}")
        else:
            carry_in = f", s{prev}.{flag}" if prev is not None else ""
            lines.append(f"auto const s{i} = {fn}({x}, {y}{carry_in});")
            out.append(f"s{i}.value")
            prev = i
    return lines + result_store(r, signed, out)


def body_mul(r: int, a: int, b: int, signed: bool) -> list[str]:
    rw = word_count(r)
    # A 32-bit operand takes part as its sign-extended 64-bit word.
    ae = max(a, 64)
    be = max(b, 64)
    aw = ae // 64
    bw = be // 64

    if r == 128 and a <= 64 and b <= 64:
        da, na, _ = operand_words("a", a, signed)
        db, nb, _ = operand_words("b", b, signed)
        if signed:
            call = f"cc::imul128(i64({na[0]}), i64({nb[0]}))"
            return da + db + [f"auto const p = {call};"] + result_store(r, signed, ["p.lo", "u64(p.hi)"])
        call = f"cc::umul128({na[0]}, {nb[0]})"
        return da + db + [f"auto const p = {call};"] + result_store(r, signed, ["p.lo", "p.hi"])

    da, na, _ = operand_words("a", a, signed)
    db, nb, _ = operand_words("b", b, signed)
    lines = [d for d in da + db if "_fill" not in d]

    # The unsigned product of the raw words, truncated to rw words: one row per word of a, in SSA form.
    acc: list[str | None] = [None] * rw
    counter = 0

    def fresh(prefix: str) -> str:
        nonlocal counter
        counter += 1
        return f"{prefix}{counter}"

    for i in range(min(aw, rw)):
        carry = None
        for j in range(min(bw, rw - i)):
            k = i + j
            cur = acc[k] if acc[k] is not None else "u64(0)"
            if k == rw - 1:
                # only the low half of the top product lands below 2^R
                v = fresh("t")
                extra = f" + {carry}" if carry is not None else ""
                lines.append(f"u64 const {v} = {cur} + {na[i]} * {nb[j]}{extra};")
                acc[k] = v
                carry = None
                break
            p = fresh("p")
            lines.append(f"auto const {p} = mul_add({cur}, {na[i]}, {nb[j]}, {carry if carry else 'u64(0)'});")
            acc[k] = f"{p}.lo"
            carry = f"{p}.hi"
        if carry is not None and i + bw < rw:
            acc[i + bw] = carry

    words = [w if w is not None else "u64(0)" for w in acc]

    # A negative a read as unsigned is a + 2^Ae, so the product is too large by b_raw * 2^Ae; likewise for b.
    # Subtracting both corrections leaves 2^(Ae + Be) * [a < 0][b < 0], which vanishes modulo 2^R.
    if signed:
        for shift, mask_src, other, other_words in ((aw, "a", "b", nb), (bw, "b", "a", na)):
            if shift >= rw:
                continue
            m = fresh("m")
            lines.append(f"u64 const {m} = mask_if(i64({(na if mask_src == 'a' else nb)[-1]}) < 0);")
            borrow = None
            for k in range(shift, rw):
                idx = k - shift
                term = f"({other_words[idx]} & {m})" if idx < len(other_words) else "u64(0)"
                if k == rw - 1:
                    v = fresh("t")
                    tail = f" - {borrow}.borrow" if borrow else ""
                    lines.append(f"u64 const {v} = {words[k]} - {term}{tail};")
                    words[k] = v
                else:
                    s = fresh("d")
                    bin_ = f", {borrow}.borrow" if borrow else ""
                    lines.append(f"auto const {s} = cc::sub_with_borrow({words[k]}, {term}{bin_});")
                    words[k] = f"{s}.value"
                    borrow = s

    return lines + result_store(r, signed, words)


def body_shift(direction: str, bits: int, signed: bool) -> list[str]:
    if bits == 32:
        if direction == "shl":
            return [f"{ty(32, signed)} r;", "r.limbs[0] = u32(x.limbs[0] << n);", "return r;"]
        expr = "u32(i32(x.limbs[0]) >> n)" if signed else "u32(x.limbs[0] >> n)"
        return [f"{ty(32, signed)} r;", f"r.limbs[0] = {expr};", "return r;"]

    count = bits // 64
    lines = [f"u64 const x{i} = x.limbs[{i}];" for i in range(count)]
    lines += ["auto const word = n >> 6;", "auto const bit = n & 63;"]
    if direction == "shl":
        # stage 1 moves whole words by a data-dependent amount with masks, stage 2 funnels the bits across
        for i in range(count):
            terms = [f"(x{k} & mask_if(word == {i - k}))" for k in range(i + 1)]
            lines.append(f"u64 const w{i} = {' | '.join(terms)};")
        out = []
        for i in range(count):
            low = f" | ((w{i - 1} >> 1) >> (63 - bit))" if i > 0 else ""
            out.append(f"(w{i} << bit){low}")
    else:
        fill = "u64(i64(x{0}) >> 63)".format(count - 1) if signed else "u64(0)"
        lines.append(f"u64 const fill = {fill};")
        for i in range(count):
            terms = [f"(x{k} & mask_if(word == {k - i}))" for k in range(i, count)]
            terms.append(f"(fill & mask_if(word > {count - 1 - i}))")
            lines.append(f"u64 const w{i} = {' | '.join(terms)};")
        out = []
        for i in range(count):
            high = f"w{i + 1}" if i + 1 < count else "fill"
            out.append(f"(w{i} >> bit) | (({high} << 1) << (63 - bit))")
    lines.append(f"{ty(bits, signed)} r;")
    for i, e in enumerate(out):
        lines.append(f"r.limbs[{i}] = {e};")
    lines.append("return r;")
    return lines


def specialization(struct: str, params: str, ret: str, args: str, body: list[str]) -> list[str]:
    return [
        "template <>",
        f"struct tg::impl::{struct}<{params}>",
        "{",
        "    static constexpr bool generated = true;",
        f"    [[nodiscard]] static constexpr {ret} apply({args})",
        "    {",
        *[f"        {line}" for line in body],
        "    }",
        "};",
        "",
    ]


def emit_width(r: int, table: list[tuple[str, int, int, int, bool]]) -> str:
    lines = [
        f"// GENERATED by {COMMAND} — do not edit.",
        f"// Every fixed_int operation whose result is {r} bits wide, loop-free and branch-free.",
        "// libs/base/typed-geometry/tools/gen-fixed-int.py has the rule that picked them.",
        "#pragma once",
        "",
        "#include <typed-geometry/scalar/fixed_int/impl/core.hh>",
        "",
        "// NOLINTBEGIN",
        "",
    ]
    for signed in (True, False):
        s = "true" if signed else "false"
        for direction in ("shl", "shr"):
            body = body_shift(direction, r, signed)
            lines += specialization(f"{direction}_op", f"{r}, {s}", ty(r, signed), f"{ty(r, signed)} const& x, int n", body)
    for op, rr, a, b, signed in table:
        if rr != r:
            continue
        s = "true" if signed else "false"
        args = f"{ty(a, signed)} const& a, {ty(b, signed)} const& b"
        if op in ("add", "mul") and a > b:
            body = [f"return {op}_op<{r}, {b}, {a}, {s}>::apply(b, a);"]
        elif op == "mul":
            body = body_mul(r, a, b, signed)
        else:
            body = body_add_sub(op, r, a, b, signed)
        lines += specialization(f"{op}_op", f"{r}, {a}, {b}, {s}", ty(r, signed), args, body)
    lines += ["// NOLINTEND", ""]
    return "\n".join(lines)


def emit_all(max_bits: int) -> str:
    lines = [
        f"// GENERATED by {COMMAND} — do not edit.",
        "// Every generated header, so that nothing can see a *_op primary without its specializations.",
        "#pragma once",
        "",
    ]
    lines += [f"#include <typed-geometry/scalar/fixed_int/generated/w{r}.hh>" for r in widths(max_bits)]
    return "\n".join(lines) + "\n"


# --- golden vectors --------------------------------------------------------------------------------------------


def wrap(v: int, bits: int) -> int:
    return v & ((1 << bits) - 1)


def as_signed(v: int, bits: int) -> int:
    v = wrap(v, bits)
    return v - (1 << bits) if v >> (bits - 1) else v


def words_of(v: int, bits: int) -> list[int]:
    v = wrap(v, bits)
    return [(v >> (64 * i)) & ((1 << 64) - 1) for i in range(word_count(bits))]


def sample_values(bits: int, signed: bool, rng: random.Random, n_random: int) -> list[int]:
    lo, hi = value_range(bits, signed)
    vals = {0, 1, lo, hi}
    if signed:
        vals.add(-1)
    for p in range(31, bits, 32):
        vals.update({1 << p, (1 << p) - 1})
        if signed:
            vals.add(-(1 << p))
    for _ in range(n_random):
        v = rng.getrandbits(bits) >> rng.randrange(bits)
        vals.add(as_signed(v, bits) if signed else v)
    return sorted(v for v in vals if lo <= v <= hi)


def cpp_words(ws: list[int]) -> str:
    return "{" + ", ".join(f"0x{w:x}ull" for w in ws) + "}"


def emit_golden(table: list[tuple[str, int, int, int, bool]], max_bits: int) -> str:
    rng = random.Random(0x5EED)
    lines = [
        f"// GENERATED by {COMMAND} — do not edit.",
        "// Golden vectors computed with Python's integers: every generated fixed_int operation against a value no C++",
        "// shares a mistake with.",
        "#include <nexus/test.hh>",
        "#include <typed-geometry/scalar/fixed_int/fixed_arith.hh>",
        "",
        "using namespace cc::primitive_defines;",
        "",
        "// NOLINTBEGIN",
        "",
        "namespace",
        "{",
        "template <int Bits, bool Signed>",
        "tg::impl::fixed_integer<Bits, Signed> from_words(cc::span<u64 const> ws)",
        "{",
        "    tg::impl::fixed_integer<Bits, Signed> r;",
        "    for (auto i = 0; i < r.limb_count; ++i)",
        "        tg::impl::set_limb(r, i, ws[i]);",
        "    return r;",
        "}",
        "",
        "template <int N>",
        "struct golden_binary",
        "{",
        "    u64 a[4];",
        "    u64 b[4];",
        "    u64 r[N];",
        "};",
        "",
        "template <int N>",
        "struct golden_shift",
        "{",
        "    u64 x[N];",
        "    int n;",
        "    u64 shl[N];",
        "    u64 shr[N];",
        "};",
        "",
        "template <class Op, int R, int A, int B, bool S, int N, auto Count>",
        "void check_binary(golden_binary<N> const (&cases)[Count])",
        "{",
        "    static_assert(Op::generated, \"a table entry resolved to the generic primary\");",
        "    for (auto const& c : cases)",
        "    {",
        "        auto const a = from_words<A, S>(c.a);",
        "        auto const b = from_words<B, S>(c.b);",
        "        CHECK((Op::apply(a, b) == from_words<R, S>(c.r)));",
        "    }",
        "}",
        "",
        "template <int Bits, bool S, int N, auto Count>",
        "void check_shifts(golden_shift<N> const (&cases)[Count])",
        "{",
        "    for (auto const& c : cases)",
        "    {",
        "        auto const x = from_words<Bits, S>(c.x);",
        "        CHECK((tg::impl::shl_op<Bits, S>::apply(x, c.n) == from_words<Bits, S>(c.shl)));",
        "        CHECK((tg::impl::shr_op<Bits, S>::apply(x, c.n) == from_words<Bits, S>(c.shr)));",
        "    }",
        "}",
        "} // namespace",
        "",
    ]

    # arithmetic, one TEST per result width so a failure names it
    for r in widths(max_bits):
        lines.append(f'TEST("tg fixed_int - golden vectors, {r}-bit results")')
        lines.append("{")
        for op, rr, a, b, signed in table:
            if rr != r:
                continue
            s = "true" if signed else "false"
            av = sample_values(a, signed, rng, 3)
            bv = sample_values(b, signed, rng, 3)
            pairs = [(x, y) for x in av for y in bv]
            rng.shuffle(pairs)
            pairs = pairs[:10]
            f = {"add": lambda x, y: x + y, "sub": lambda x, y: x - y, "mul": lambda x, y: x * y}[op]
            n = word_count(r)
            cases = [
                f"{{{cpp_words(words_of(x, a))}, {cpp_words(words_of(y, b))}, {cpp_words(words_of(f(x, y), r))}}}"
                for x, y in pairs
            ]
            lines.append("    {")
            lines.append(f"        golden_binary<{n}> const cases[] = {{")
            lines += [f"            {c}," for c in cases]
            lines.append("        };")
            lines.append(f"        check_binary<tg::impl::{op}_op<{r}, {a}, {b}, {s}>, {r}, {a}, {b}, {s}>(cases);")
            lines.append("    }")
        for signed in (True, False):
            s = "true" if signed else "false"
            n = word_count(r)
            cases = []
            for x in sample_values(r, signed, rng, 4)[:8]:
                for k in sorted(k for k in {0, 1, 31, 63, r - 1, rng.randrange(r)} if k < r):
                    shl = wrap(x << k, r)
                    shr = x >> k  # Python's >> on a negative int is arithmetic, on a non-negative one logical
                    cases.append(f"{{{cpp_words(words_of(x, r))}, {k}, {cpp_words(words_of(shl, r))}, {cpp_words(words_of(shr, r))}}}")
            lines.append("    {")
            lines.append(f"        golden_shift<{n}> const cases[] = {{")
            lines += [f"            {c}," for c in cases]
            lines.append("        };")
            lines.append(f"        check_shifts<{r}, {s}>(cases);")
            lines.append("    }")
        lines.append("}")
        lines.append("")

    # division and float conversion, which have no generated bodies but deserve an oracle outside C++
    lines.append('TEST("tg fixed_int - golden vectors, division and to_f64")')
    lines.append("{")
    for r in widths(max_bits):
        for signed in (True, False):
            s = "true" if signed else "false"
            vals = sample_values(r, signed, rng, 5)
            for _ in range(6):
                x = rng.choice(vals)
                y = rng.choice(vals)
                if y == 0 or (signed and x == value_range(r, True)[0] and y == -1):
                    continue
                q = abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)
                m = x - q * y
                fq, fm = divmod(x, y)
                lines.append("    {")
                named = [("x", x), ("y", y), ("q", q), ("m", m), ("fq", fq), ("fm", fm)]
                lines += [f"        u64 const {n}[] = {cpp_words(words_of(v, r))};" for n, v in named]
                lines.append(f"        auto const a = from_words<{r}, {s}>(x);")
                lines.append(f"        auto const b = from_words<{r}, {s}>(y);")
                for name, n in (("div_trunc", "q"), ("mod_trunc", "m"), ("div_floor", "fq"), ("mod_floor", "fm")):
                    lines.append(f"        CHECK((tg::{name}(a, b) == from_words<{r}, {s}>({n})));")
                lines.append(f"        CHECK(a.to_f64() == {float(x).hex()});")
                lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("// NOLINTEND")
    lines.append("")
    return "\n".join(lines)


# --- driver ----------------------------------------------------------------------------------------------------


def find_clang_format(explicit: str | None) -> str:
    if explicit:
        return explicit
    pinned = REPO / "tools" / "bin" / ("clang-format.exe" if os.name == "nt" else "clang-format")
    if pinned.exists():
        return str(pinned)
    found = shutil.which("clang-format")
    if found:
        return found
    sys.exit("gen-fixed-int: no clang-format found; pass --clang-format PATH")


def clang_format(text: str, assume: Path, exe: str) -> str:
    result = subprocess.run(
        [exe, f"--assume-filename={assume}", f"--style=file:{REPO / '.clang-format'}"],
        input=text.encode("utf-8"),
        capture_output=True,
        check=True,
    )
    return result.stdout.decode("utf-8").replace("\r\n", "\n")


def generate(max_bits: int, exe: str) -> dict[Path, str]:
    table = triple_table(max_bits)
    out = {}
    for r in widths(max_bits):
        path = GENERATED / f"w{r}.hh"
        out[path] = clang_format(emit_width(r, table), path, exe)
    out[GENERATED / "all.hh"] = clang_format(emit_all(max_bits), GENERATED / "all.hh", exe)
    out[GOLDEN] = clang_format(emit_golden(table, max_bits), GOLDEN, exe)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--max-bits", type=int, default=256, help="the widest operand and result that is generated")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="write the generated files")
    mode.add_argument("--check", action="store_true", help="exit 2 when a committed file differs from the output")
    parser.add_argument("--clang-format", help="the clang-format to format the output with")
    parser.add_argument("--list", action="store_true", help="print the triple table")
    args = parser.parse_args()

    if args.list:
        for op, r, a, b, signed in triple_table(args.max_bits):
            print(f"{op} {'fi' if signed else 'fu'}{a} x {'fi' if signed else 'fu'}{b} -> {r}")

    files = generate(args.max_bits, find_clang_format(args.clang_format))
    stale = sorted(p for p in GENERATED.glob("w*.hh") if p not in files)

    # Line endings are the checkout's business, so a file differs only in its content.
    differs = [p for p, text in files.items() if not p.exists() or p.read_bytes().decode("utf-8").replace("\r\n", "\n") != text]

    if args.write:
        # Only what changed is written, so a no-op --fix touches no file and triggers no rebuild.
        GENERATED.mkdir(parents=True, exist_ok=True)
        for path in differs:
            path.write_bytes(files[path].encode("utf-8"))
        for path in stale:
            path.unlink()
        print(f"gen-fixed-int: wrote {len(differs)} of {len(files)} files, removed {len(stale)}")
        return 0

    differs += stale
    for p in differs:
        print(f"gen-fixed-int: {p.relative_to(REPO).as_posix()} differs from the generator's output")
    if differs:
        print(f"gen-fixed-int: run `{COMMAND}`")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
