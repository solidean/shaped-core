"""The simd128 kernel: WebAssembly SIMD128, one untyped 128-bit register, masks as all-ones lanes."""

from __future__ import annotations

from .model import SHORT, SINGLE, Elem, Impl


def reg_type(e: Elem, w: int) -> str:
    return "v128_t"


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    return "v128_t"


def _ret(expr: str) -> Impl:
    return Impl(SINGLE, f"return {expr};")


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    t = e.name
    v = {"f32": "f32x4", "i32": "i32x4", "u32": "u32x4"}[t]
    out: dict[str, Impl] = {}
    out["broadcast"] = _ret(f"wasm_{v}_splat(x)")
    out["zero"] = _ret("wasm_i32x4_splat(0)")
    out["iota"] = _ret(f"wasm_{'f32x4' if e.is_float else 'i32x4'}_add(wasm_{v}_splat(start), wasm_{v}_make(0, 1, 2, 3))")
    out["load"] = _ret("wasm_v128_load(p)")
    out["load_aligned"] = _ret("wasm_v128_load(p)")
    out["store"] = Impl(SINGLE, "wasm_v128_store(p, a);")
    out["store_aligned"] = Impl(SINGLE, "wasm_v128_store(p, a);")
    arith = "f32x4" if e.is_float else "i32x4"
    out["add"] = _ret(f"wasm_{arith}_add(a, b)")
    out["sub"] = _ret(f"wasm_{arith}_sub(a, b)")
    out["mul"] = _ret(f"wasm_{arith}_mul(a, b)")
    if e.is_float:
        # pmin/pmax are the x86-shaped forms, one instruction on every engine; min/max propagate NaN at a cost.
        out["min"] = _ret("wasm_f32x4_pmin(a, b)")
        out["max"] = _ret("wasm_f32x4_pmax(a, b)")
    else:
        out["min"] = _ret(f"wasm_{v}_min(a, b)")
        out["max"] = _ret(f"wasm_{v}_max(a, b)")
    if e.kind != "unsigned":
        out["neg"] = _ret(f"wasm_{arith}_neg(a)")
        out["abs"] = _ret(f"wasm_{arith}_abs(a)")
    out["mul_add"] = Impl(SHORT, f"return wasm_{arith}_add(wasm_{arith}_mul(a, b), c);")
    if not e.is_float:
        out["bit_and"] = _ret("wasm_v128_and(a, b)")
        out["bit_or"] = _ret("wasm_v128_or(a, b)")
        out["bit_xor"] = _ret("wasm_v128_xor(a, b)")
        out["bit_not"] = _ret("wasm_v128_not(a)")
    eq_ne = "f32x4" if e.is_float else "i32x4"
    out["eq"] = _ret(f"wasm_{eq_ne}_eq(a, b)")
    out["ne"] = _ret(f"wasm_{eq_ne}_ne(a, b)")
    for name in ("lt", "le", "gt", "ge"):
        out[name] = _ret(f"wasm_{v}_{name}(a, b)")
    out["select"] = _ret("wasm_v128_bitselect(a, b, m)")
    for op in ("add", "min", "max"):
        out[f"reduce_{op}"] = Impl(SHORT, f"v128_t t = {op}(a, wasm_i32x4_shuffle(a, a, 2, 3, 0, 1));\n"
                                          f"t = {op}(t, wasm_i32x4_shuffle(t, t, 1, 0, 3, 2));\n"
                                          f"return wasm_{v}_extract_lane(t, 0);")
    if e.is_float:
        out["to_i32"] = _ret("wasm_i32x4_trunc_sat_f32x4(a)")
    else:
        out["to_f32"] = _ret(f"wasm_f32x4_convert_{v}(a)")
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    return {
        "bit_and": _ret("wasm_v128_and(a, b)"),
        "bit_or": _ret("wasm_v128_or(a, b)"),
        "bit_xor": _ret("wasm_v128_xor(a, b)"),
        "bit_not": _ret("wasm_v128_not(a)"),
        "bits": _ret("u32(wasm_i32x4_bitmask(m))"),
        "any": _ret("wasm_v128_any_true(m)"),
        "all": _ret("wasm_i32x4_all_true(m)"),
        "from_bits": Impl(SHORT, "return wasm_i32x4_ne(wasm_v128_and(wasm_i32x4_splat(int(b)), "
                                 "wasm_i32x4_make(1, 2, 4, 8)), wasm_i32x4_splat(0));"),
    }
