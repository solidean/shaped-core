"""The scalar kernel: plain C++, one 128-bit "register" held as an array, every lane written out.

It is the reference every other kernel is tested against, so it is the one place behaviour is spelled out per lane.
Signed arithmetic goes through unsigned to wrap the way every SIMD unit does, rather than be undefined.
"""

from __future__ import annotations

from .model import SINGLE, Elem, Impl


def reg_type(e: Elem, w: int) -> str:
    return f"scalar_reg<{e.name}, {w // e.bits}>"


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    return f"scalar_mreg<{w // lane_bits}>"


def _lanes(e: Elem, w: int, expr: str) -> Impl:
    """`type r; r.v[i] = expr(i) ...; return r;` with `{i}` substituted per lane."""
    lanes = w // e.bits
    body = ["type r;"] + [f"r.v[{i}] = {expr.format(i=i)};" for i in range(lanes)] + ["return r;"]
    return Impl(SINGLE, "\n".join(body))


def _mlanes(lanes: int, expr: str, result: str = "mtype") -> Impl:
    body = [f"{result} r;"] + [f"r.v[{i}] = {expr.format(i=i)};" for i in range(lanes)] + ["return r;"]
    return Impl(SINGLE, "\n".join(body))


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    t = e.name
    lanes = w // e.bits
    out: dict[str, Impl] = {}
    wrap = e.is_signed  # signed integer arithmetic goes through u32

    def arith(op: str) -> str:
        if wrap:
            return f"{t}(u32(a.v[{{i}}]) {op} u32(b.v[{{i}}]))"
        return f"{t}(a.v[{{i}}] {op} b.v[{{i}}])"

    out["broadcast"] = _lanes(e, w, "x")
    out["zero"] = _lanes(e, w, f"{t}(0)")
    out["iota"] = _lanes(e, w, f"{t}(start + {t}({{i}}))" if not wrap else f"{t}(u32(start) + {{i}}u)")
    out["load"] = _lanes(e, w, "p[{i}]")
    out["load_aligned"] = _lanes(e, w, "p[{i}]")
    out["store"] = Impl(SINGLE, "\n".join(f"p[{i}] = a.v[{i}];" for i in range(lanes)))
    out["store_aligned"] = out["store"]
    out["add"] = _lanes(e, w, arith("+"))
    out["sub"] = _lanes(e, w, arith("-"))
    out["mul"] = _lanes(e, w, arith("*"))
    out["min"] = _lanes(e, w, "b.v[{i}] < a.v[{i}] ? b.v[{i}] : a.v[{i}]")
    out["max"] = _lanes(e, w, "a.v[{i}] < b.v[{i}] ? b.v[{i}] : a.v[{i}]")
    if e.is_float:
        out["neg"] = _lanes(e, w, "-a.v[{i}]")
        out["abs"] = _lanes(e, w, "cc::bit_cast<f32>(cc::bit_cast<u32>(a.v[{i}]) & 0x7FFFFFFFu)")
        out["mul_add"] = _lanes(e, w, "a.v[{i}] * b.v[{i}] + c.v[{i}]")
    elif wrap:
        out["neg"] = _lanes(e, w, f"{t}(0u - u32(a.v[{{i}}]))")
        out["abs"] = _lanes(e, w, f"a.v[{{i}}] < 0 ? {t}(0u - u32(a.v[{{i}}])) : a.v[{{i}}]")
        out["mul_add"] = _lanes(e, w, f"{t}(u32(a.v[{{i}}]) * u32(b.v[{{i}}]) + u32(c.v[{{i}}]))")
    else:
        out["mul_add"] = _lanes(e, w, f"{t}(a.v[{{i}}] * b.v[{{i}}] + c.v[{{i}}])")
    if not e.is_float:
        out["bit_and"] = _lanes(e, w, f"{t}(a.v[{{i}}] & b.v[{{i}}])")
        out["bit_or"] = _lanes(e, w, f"{t}(a.v[{{i}}] | b.v[{{i}}])")
        out["bit_xor"] = _lanes(e, w, f"{t}(a.v[{{i}}] ^ b.v[{{i}}])")
        out["bit_not"] = _lanes(e, w, f"{t}(~a.v[{{i}}])")
    for name, op in (("eq", "=="), ("ne", "!="), ("lt", "<"), ("le", "<="), ("gt", ">"), ("ge", ">=")):
        out[name] = _mlanes(lanes, f"a.v[{{i}}] {op} b.v[{{i}}]")
    out["select"] = _lanes(e, w, "m.v[{i}] ? a.v[{i}] : b.v[{i}]")

    def tree(f) -> str:
        # Lane i with lane i + 2, then the two halves: the order every kernel reduces in.
        return f"return {f(f('a.v[0]', 'a.v[2]'), f('a.v[1]', 'a.v[3]'))};"

    if wrap:
        out["reduce_add"] = Impl(SINGLE, tree(lambda x, y: f"{t}(u32({x}) + u32({y}))"))
    else:
        out["reduce_add"] = Impl(SINGLE, tree(lambda x, y: f"{t}({x} + {y})"))
    out["reduce_min"] = Impl(SINGLE, "auto const f = [](" + t + " x, " + t + " y) { return y < x ? y : x; };\n"
                         + tree(lambda x, y: f"f({x}, {y})"))
    out["reduce_max"] = Impl(SINGLE, "auto const f = [](" + t + " x, " + t + " y) { return x < y ? y : x; };\n"
                         + tree(lambda x, y: f"f({x}, {y})"))
    if e.is_float:
        # Out of range is unspecified; this picks saturation, and keeps the C++ conversion away from its UB.
        out["to_i32"] = Impl(SINGLE, "\n".join(
            ["scalar_reg<i32, 4> r;"]
            + [f"r.v[{i}] = to_i32_lane(a.v[{i}]);" for i in range(lanes)]
            + ["return r;"]))
    else:
        out["to_f32"] = Impl(SINGLE, "\n".join(
            ["scalar_reg<f32, 4> r;"] + [f"r.v[{i}] = f32(a.v[{i}]);" for i in range(lanes)] + ["return r;"]))
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    lanes = w // lane_bits
    bits = " | ".join(f"(u32(m.v[{i}]) << {i})" for i in range(lanes))
    return {
        "bit_and": _mlanes(lanes, "a.v[{i}] && b.v[{i}]", "type"),
        "bit_or": _mlanes(lanes, "a.v[{i}] || b.v[{i}]", "type"),
        "bit_xor": _mlanes(lanes, "a.v[{i}] != b.v[{i}]", "type"),
        "bit_not": _mlanes(lanes, "!a.v[{i}]", "type"),
        "bits": Impl(SINGLE, f"return {bits};"),
        "any": Impl(SINGLE, "return " + " || ".join(f"m.v[{i}]" for i in range(lanes)) + ";"),
        "all": Impl(SINGLE, "return " + " && ".join(f"m.v[{i}]" for i in range(lanes)) + ";"),
        "from_bits": _mlanes(lanes, "((b >> {i}) & 1u) != 0", "type"),
    }
