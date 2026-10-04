"""The scalar kernel: plain C++, one 128-bit "register" held as an array, every lane written out.

It is the reference every other kernel is tested against, so it is the one place behaviour is spelled out per lane.
Integer arithmetic goes through an unsigned type of at least 32 bits, which wraps the way every SIMD unit does where
signed overflow — or a u16 product promoted to int — would be undefined.
"""

from __future__ import annotations

from .model import CONVERSIONS, ELEM, SINGLE, Elem, Impl


def reg_type(e: Elem, w: int) -> str:
    return f"scalar_reg<{e.name}, {w // e.bits}>"


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    return f"scalar_mreg<{w // lane_bits}>"


def _wide(e: Elem) -> str:
    """The unsigned type integer arithmetic on `e` goes through."""
    return "u64" if e.bits == 64 else "u32"


def _lanes(lanes: int, expr: str, result: str = "type") -> Impl:
    """`result r; r.v[i] = expr(i) …; return r;` with `{i}` substituted per lane."""
    body = [f"{result} r;"] + [f"r.v[{i}] = {expr.format(i=i)};" for i in range(lanes)] + ["return r;"]
    return Impl(SINGLE, "\n".join(body))


def _tree(lanes: int, f) -> str:
    """Lane i with lane i + n/2, recursively: the order every kernel reduces in."""
    level = [f"a.v[{i}]" for i in range(lanes)]
    while len(level) > 1:
        half = len(level) // 2
        level = [f(level[i], level[i + half]) for i in range(half)]
    return level[0]


# Round to nearest, ties to even, for a float whose magnitude is below 2^mantissa; anything above is already integral.
# The sum and difference round in the default mode, which is what makes this the hardware's nearest.
_NEAREST = {
    "f32": "auto const nearest = [](f32 x) { f32 const ax = x < 0 ? -x : x; "
           "if (!(ax < 8388608.f)) return x; f32 const r = (ax + 8388608.f) - 8388608.f; return x < 0 ? -r : r; };\n",
    "f64": "auto const nearest = [](f64 x) { f64 const ax = x < 0 ? -x : x; "
           "if (!(ax < 4503599627370496.0)) return x; f64 const r = (ax + 4503599627370496.0) - 4503599627370496.0; "
           "return x < 0 ? -r : r; };\n",
}


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    t = e.name
    lanes = w // e.bits
    u = _wide(e)
    out: dict[str, Impl] = {}

    def arith(op: str) -> str:
        if e.is_float:
            return f"a.v[{{i}}] {op} b.v[{{i}}]"
        return f"{t}({u}(a.v[{{i}}]) {op} {u}(b.v[{{i}}]))"

    out["broadcast"] = _lanes(lanes, "x")
    out["zero"] = _lanes(lanes, f"{t}(0)")
    out["iota"] = _lanes(lanes, f"start + {t}({{i}})" if e.is_float else f"{t}({u}(start) + {{i}}u)")
    out["load"] = _lanes(lanes, "p[{i}]")
    out["load_aligned"] = _lanes(lanes, "p[{i}]")
    out["store"] = Impl(SINGLE, "\n".join(f"p[{i}] = a.v[{i}];" for i in range(lanes)))
    out["store_aligned"] = out["store"]
    out["add"] = _lanes(lanes, arith("+"))
    out["sub"] = _lanes(lanes, arith("-"))
    out["mul"] = _lanes(lanes, arith("*"))
    out["min"] = _lanes(lanes, "b.v[{i}] < a.v[{i}] ? b.v[{i}] : a.v[{i}]")
    out["max"] = _lanes(lanes, "a.v[{i}] < b.v[{i}] ? b.v[{i}] : a.v[{i}]")
    if e.is_float:
        out["mul_add"] = _lanes(lanes, "a.v[{i}] * b.v[{i}] + c.v[{i}]")
    else:
        out["mul_add"] = _lanes(lanes, f"{t}({u}(a.v[{{i}}]) * {u}(b.v[{{i}}]) + {u}(c.v[{{i}}]))")

    if e.is_float:
        bits = "u32" if e.bits == 32 else "u64"
        mask = "0x7FFFFFFFu" if e.bits == 32 else "0x7FFFFFFFFFFFFFFFull"
        sign = "0x80000000u" if e.bits == 32 else "0x8000000000000000ull"
        out["neg"] = _lanes(lanes, "-a.v[{i}]")
        out["abs"] = _lanes(lanes, f"cc::bit_cast<{t}>({bits}(cc::bit_cast<{bits}>(a.v[{{i}}]) & {mask}))")
        out["div"] = _lanes(lanes, "a.v[{i}] / b.v[{i}]")
        out["sqrt"] = _lanes(lanes, "std::sqrt(a.v[{i}])")
        nearest = _NEAREST[t]
        rounded = "nearest(a.v[{i}])"
        down = f"({rounded} > a.v[{{i}}] ? {rounded} - {t}(1) : {rounded})"
        up = f"({rounded} < a.v[{{i}}] ? {rounded} + {t}(1) : {rounded})"
        out["round"] = Impl(SINGLE, nearest + _lanes(lanes, rounded).body)
        out["floor"] = Impl(SINGLE, nearest + _lanes(lanes, down).body)
        out["ceil"] = Impl(SINGLE, nearest + _lanes(lanes, up).body)
        out["trunc"] = Impl(SINGLE, nearest + _lanes(lanes, f"a.v[{{i}}] < 0 ? {up} : {down}").body)
        out["copysign"] = _lanes(lanes, f"cc::bit_cast<{t}>({bits}((cc::bit_cast<{bits}>(a.v[{{i}}]) & ~{sign}) | "
                                        f"(cc::bit_cast<{bits}>(b.v[{{i}}]) & {sign})))")
    else:
        if e.is_signed:
            out["neg"] = _lanes(lanes, f"{t}({u}(0) - {u}(a.v[{{i}}]))")
            out["abs"] = _lanes(lanes, f"a.v[{{i}}] < 0 ? {t}({u}(0) - {u}(a.v[{{i}}])) : a.v[{{i}}]")
        out["bit_and"] = _lanes(lanes, f"{t}(a.v[{{i}}] & b.v[{{i}}])")
        out["bit_or"] = _lanes(lanes, f"{t}(a.v[{{i}}] | b.v[{{i}}])")
        out["bit_xor"] = _lanes(lanes, f"{t}(a.v[{{i}}] ^ b.v[{{i}}])")
        out["bit_not"] = _lanes(lanes, f"{t}(~a.v[{{i}}])")
        out["shl"] = _lanes(lanes, f"{t}({u}(a.v[{{i}}]) << n)")
        out["shr"] = _lanes(lanes, f"{t}(a.v[{{i}}] >> n)")

    for name, op in (("eq", "=="), ("ne", "!="), ("lt", "<"), ("le", "<="), ("gt", ">"), ("ge", ">=")):
        out[name] = _lanes(lanes, f"a.v[{{i}}] {op} b.v[{{i}}]", "mtype")
    out["select"] = _lanes(lanes, "m.v[{i}] ? a.v[{i}] : b.v[{i}]")

    if e.is_float:
        out["reduce_add"] = Impl(SINGLE, f"return {_tree(lanes, lambda x, y: f'({x} + {y})')};")
    else:
        out["reduce_add"] = Impl(SINGLE, f"return {_tree(lanes, lambda x, y: f'{t}({u}({x}) + {u}({y}))')};")
    out["reduce_min"] = Impl(SINGLE, f"auto const f = []({t} x, {t} y) {{ return y < x ? y : x; }};\n"
                                     f"return {_tree(lanes, lambda x, y: f'f({x}, {y})')};")
    out["reduce_max"] = Impl(SINGLE, f"auto const f = []({t} x, {t} y) {{ return x < y ? y : x; }};\n"
                                     f"return {_tree(lanes, lambda x, y: f'f({x}, {y})')};")

    for target in CONVERSIONS.get(t, []):
        rt = reg_type(ELEM[target], w)
        if e.is_float:
            # Out of range is unspecified for cimd; this saturates, which keeps the C++ conversion away from its UB.
            hi = "2147483648.f" if e.bits == 32 else "9223372036854775808.0"
            body = (f"auto const conv = []({t} x) -> {target} {{ if (!(x == x)) return 0; "
                    f"if (x >= {hi}) return {target}(~0ull >> {65 - e.bits}); "
                    f"if (x < -{hi}) return {target}(-{target}(~0ull >> {65 - e.bits}) - 1); return {target}(x); }};\n")
            out[f"to_{target}"] = Impl(SINGLE, body + _lanes(lanes, "conv(a.v[{i}])", rt).body)
        else:
            out[f"to_{target}"] = _lanes(lanes, f"{target}(a.v[{{i}}])", rt)
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    lanes = w // lane_bits
    bits = " | ".join(f"(u64(m.v[{i}]) << {i})" for i in range(lanes))
    return {
        "bit_and": _lanes(lanes, "a.v[{i}] && b.v[{i}]"),
        "bit_or": _lanes(lanes, "a.v[{i}] || b.v[{i}]"),
        "bit_xor": _lanes(lanes, "a.v[{i}] != b.v[{i}]"),
        "bit_not": _lanes(lanes, "!a.v[{i}]"),
        "bits": Impl(SINGLE, f"return {bits};"),
        "any": Impl(SINGLE, "return " + " || ".join(f"m.v[{i}]" for i in range(lanes)) + ";"),
        "all": Impl(SINGLE, "return " + " && ".join(f"m.v[{i}]" for i in range(lanes)) + ";"),
        "from_bits": _lanes(lanes, "((b >> {i}) & 1u) != 0"),
    }
