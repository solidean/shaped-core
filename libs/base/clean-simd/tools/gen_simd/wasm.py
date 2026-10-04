"""The simd128 kernel: WebAssembly SIMD128, one untyped 128-bit register, masks as all-ones lanes."""

from __future__ import annotations

from .model import CONVERSIONS, ELEM, EMULATED, SHORT, SINGLE, Elem, Impl

_V = {"f32": "f32x4", "f64": "f64x2", "i8": "i8x16", "i16": "i16x8", "i32": "i32x4", "i64": "i64x2",
      "u8": "u8x16", "u16": "u16x8", "u32": "u32x4", "u64": "u64x2"}
_INT = {8: "i8x16", 16: "i16x8", 32: "i32x4", 64: "i64x2"}


def reg_type(e: Elem, w: int) -> str:
    return "v128_t"


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    return "v128_t"


def _ret(expr: str, cost: str = SINGLE) -> Impl:
    return Impl(cost, f"return {expr};")


def _shuffle(lane_bits: int, shift_bytes: int) -> str:
    """Byte indices rotating the register down by `shift_bytes`, as wasm_i8x16_shuffle takes them."""
    return ", ".join(str((k + shift_bytes) % 16) for k in range(16))


def _per_lane(e: Elem, expr: str, out_t: str | None = None) -> Impl:
    """Through memory, one lane at a time: the honest spelling of what SIMD128 has no instruction for."""
    t = e.name
    o = out_t or t
    lanes = 128 // e.bits
    body = [f"{t} x[{lanes}], y[{lanes}];", f"{o} r[{lanes}];", "wasm_v128_store(x, a);", "wasm_v128_store(y, b);"]
    body += [f"r[{i}] = {expr.format(x=f'x[{i}]', y=f'y[{i}]')};" for i in range(lanes)]
    body += ["return wasm_v128_load(r);"]
    return Impl(EMULATED, "\n".join(body))


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    t = e.name
    v = _V[t]
    iv = "f32x4" if t == "f32" else "f64x2" if t == "f64" else _INT[e.bits]  # the signed-or-float arithmetic spelling
    lanes = 128 // e.bits
    out: dict[str, Impl] = {}
    wide = "u64" if e.bits == 64 else "u32"

    out["broadcast"] = _ret(f"wasm_{v}_splat(x)")
    out["zero"] = _ret("wasm_i32x4_splat(0)")
    out["iota"] = _ret(f"wasm_{iv}_add(wasm_{v}_splat(start), wasm_{v}_make({', '.join(str(i) for i in range(lanes))}))")
    out["load"] = _ret("wasm_v128_load(p)")
    out["load_aligned"] = _ret("wasm_v128_load(p)")
    out["store"] = Impl(SINGLE, "wasm_v128_store(p, a);")
    out["store_aligned"] = Impl(SINGLE, "wasm_v128_store(p, a);")
    out["add"] = _ret(f"wasm_{iv}_add(a, b)")
    out["sub"] = _ret(f"wasm_{iv}_sub(a, b)")

    if e.bits == 8:
        out["mul"] = _per_lane(e, f"{t}(u32({{x}}) * u32({{y}}))")
    else:
        out["mul"] = _ret(f"wasm_{iv}_mul(a, b)")

    if e.is_float:
        # pmin/pmax are the x86-shaped forms, one instruction on every engine; min/max propagate NaN at a cost.
        out["min"] = _ret(f"wasm_{v}_pmin(a, b)")
        out["max"] = _ret(f"wasm_{v}_pmax(a, b)")
    elif e.bits == 64:
        out["min"] = Impl(SHORT, "return wasm_v128_bitselect(b, a, lt(b, a));")
        out["max"] = Impl(SHORT, "return wasm_v128_bitselect(b, a, lt(a, b));")
    else:
        out["min"] = _ret(f"wasm_{v}_min(a, b)")
        out["max"] = _ret(f"wasm_{v}_max(a, b)")
    cost_mul_add = SHORT if out["mul"].cost != EMULATED else EMULATED
    out["mul_add"] = Impl(cost_mul_add, "return add(mul(a, b), c);")

    if e.kind != "unsigned":
        out["neg"] = _ret(f"wasm_{iv}_neg(a)")
        out["abs"] = _ret(f"wasm_{iv}_abs(a)")

    if e.is_float:
        out["div"] = _ret(f"wasm_{v}_div(a, b)")
        out["sqrt"] = _ret(f"wasm_{v}_sqrt(a)")
        out["floor"] = _ret(f"wasm_{v}_floor(a)")
        out["ceil"] = _ret(f"wasm_{v}_ceil(a)")
        out["round"] = _ret(f"wasm_{v}_nearest(a)")
        out["trunc"] = _ret(f"wasm_{v}_trunc(a)")
        sign = "wasm_i32x4_splat(int(0x80000000u))" if e.bits == 32 else "wasm_i64x2_splat(i64(0x8000000000000000ull))"
        out["copysign"] = _ret(f"wasm_v128_bitselect(b, a, {sign})")
    else:
        out["bit_and"] = _ret("wasm_v128_and(a, b)")
        out["bit_or"] = _ret("wasm_v128_or(a, b)")
        out["bit_xor"] = _ret("wasm_v128_xor(a, b)")
        out["bit_not"] = _ret("wasm_v128_not(a)")
        out["shl"] = _ret(f"wasm_{_INT[e.bits]}_shl(a, u32(n))")
        out["shr"] = _ret(f"wasm_{v}_shr(a, u32(n))")

    eq_ne = iv
    out["eq"] = _ret(f"wasm_{eq_ne}_eq(a, b)")
    out["ne"] = _ret(f"wasm_{eq_ne}_ne(a, b)")
    if e.bits == 64 and e.kind == "unsigned":
        # No unsigned 64-bit compare: flip the sign bits and compare signed.
        flip = "v128_t const f = wasm_i64x2_splat(i64(0x8000000000000000ull));\n"
        for name in ("lt", "le", "gt", "ge"):
            out[name] = Impl(SHORT, f"{flip}return wasm_i64x2_{name}(wasm_v128_xor(a, f), wasm_v128_xor(b, f));")
    else:
        for name in ("lt", "le", "gt", "ge"):
            out[name] = _ret(f"wasm_{v}_{name}(a, b)")
    out["select"] = _ret("wasm_v128_bitselect(a, b, m)")
    size = e.bits // 8
    rev = ", ".join(str((lanes - 1 - j // size) * size + j % size) for j in range(16))
    out["reverse"] = _ret(f"wasm_i8x16_shuffle(a, a, {rev})")
    gather = [f"i{e.bits} x[{lanes}];", f"{t} r[{lanes}];", "wasm_v128_store(x, idx);"]
    gather += [f"r[{k}] = p[x[{k}]];" for k in range(lanes)]
    out["gather"] = Impl(EMULATED, "\n".join(gather + ["return wasm_v128_load(r);"]))
    if e.is_float:
        # SIMD128 has no estimate, so the approximation is the exact quotient.
        one = "1.f" if e.bits == 32 else "1.0"
        out["rcp_approx"] = _ret(f"wasm_{v}_div(wasm_{v}_splat({one}), a)")
        out["rsqrt_approx"] = Impl(SHORT, f"return wasm_{v}_div(wasm_{v}_splat({one}), wasm_{v}_sqrt(a));")

    # Lane i with lane i + n/2, recursively: rotate the register down by half its live bytes each step.
    extract = f"wasm_{v}_extract_lane"
    for op in ("add", "min", "max"):
        steps = []
        shift = 8
        while shift >= e.bits // 8:
            steps.append(f"t = {op}(t, wasm_i8x16_shuffle(t, t, {_shuffle(e.bits, shift)}));")
            shift //= 2
        out[f"reduce_{op}"] = Impl(SHORT, "v128_t t = a;\n" + "\n".join(steps) + f"\nreturn {extract}(t, 0);")

    for target in CONVERSIONS.get(t, []):
        to = ELEM[target]
        if e.bits == 32:
            spelled = {("f32", "i32"): "wasm_i32x4_trunc_sat_f32x4(a)", ("i32", "f32"): "wasm_f32x4_convert_i32x4(a)",
                       ("u32", "f32"): "wasm_f32x4_convert_u32x4(a)"}[(t, target)]
            out[f"to_{target}"] = _ret(spelled)
        else:
            # SIMD128 has no 64-bit conversions; lane by lane, saturating as the scalar kernel does.
            if e.is_float:
                expr = ("({x} != {x} ? i64(0) : {x} >= 9223372036854775808.0 ? i64(~0ull >> 1) "
                        ": {x} < -9223372036854775808.0 ? i64(-i64(~0ull >> 1) - 1) : i64({x}))")
            else:
                expr = f"{target}({{x}})"
            body = [f"{t} x[2];", f"{target} r[2];", "wasm_v128_store(x, a);"]
            body += [f"r[{i}] = {expr.format(x=f'x[{i}]')};" for i in range(2)]
            body += ["return wasm_v128_load(r);"]
            out[f"to_{target}"] = Impl(EMULATED, "\n".join(body))
    del wide
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    iv = _INT[lane_bits]
    lanes = 128 // lane_bits
    weights = ", ".join(str(1 << i) for i in range(lanes)) if lane_bits != 8 else ", ".join(str(1 << (i % 8)) for i in range(16))
    if lane_bits == 8:
        make = (f"wasm_v128_and(wasm_i8x16_shuffle(wasm_i64x2_splat(i64(b)), wasm_i64x2_splat(i64(b)), "
                f"0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1), wasm_i8x16_make({weights}))")
    else:
        make = f"wasm_v128_and(wasm_{iv}_splat(i{lane_bits}(b)), wasm_{iv}_make({weights}))"
    return {
        "bit_and": _ret("wasm_v128_and(a, b)"),
        "bit_or": _ret("wasm_v128_or(a, b)"),
        "bit_xor": _ret("wasm_v128_xor(a, b)"),
        "bit_not": _ret("wasm_v128_not(a)"),
        "bits": _ret(f"u64(wasm_{iv}_bitmask(m))"),
        "any": _ret("wasm_v128_any_true(m)"),
        "all": _ret(f"wasm_{iv}_all_true(m)"),
        "from_bits": Impl(SHORT, f"return wasm_{iv}_ne({make}, wasm_i32x4_splat(0));"),
    }
