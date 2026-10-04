"""The neon kernel: arm64 AdvSIMD, 128-bit registers, masks as all-ones lanes.

MSVC's arm64 NEON types are one union under different names, so nothing here overloads on a register type, and every
constant vector is loaded from an array rather than brace-initialized.
"""

from __future__ import annotations

from .model import SHORT, SINGLE, Elem, Impl

_SFX = {"f32": "f32", "i32": "s32", "u32": "u32"}
_TYPE = {"f32": "float32x4_t", "i32": "int32x4_t", "u32": "uint32x4_t"}


def reg_type(e: Elem, w: int) -> str:
    return _TYPE[e.name]


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    return "uint32x4_t"


def _ret(expr: str) -> Impl:
    return Impl(SINGLE, f"return {expr};")


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    s = _SFX[e.name]
    t = e.name
    out: dict[str, Impl] = {}
    out["broadcast"] = _ret(f"vdupq_n_{s}(x)")
    out["zero"] = _ret(f"vdupq_n_{s}({t}(0))")
    out["iota"] = Impl(SINGLE, f"static constexpr {t} k[4] = {{0, 1, 2, 3}};\n"
                               f"return vaddq_{s}(vdupq_n_{s}(start), vld1q_{s}(k));")
    out["load"] = _ret(f"vld1q_{s}(p)")
    out["load_aligned"] = _ret(f"vld1q_{s}(p)")
    out["store"] = Impl(SINGLE, f"vst1q_{s}(p, a);")
    out["store_aligned"] = Impl(SINGLE, f"vst1q_{s}(p, a);")
    for name, ins in (("add", "vaddq"), ("sub", "vsubq"), ("mul", "vmulq"), ("min", "vminq"), ("max", "vmaxq")):
        out[name] = _ret(f"{ins}_{s}(a, b)")
    if e.kind != "unsigned":
        out["neg"] = _ret(f"vnegq_{s}(a)")
        out["abs"] = _ret(f"vabsq_{s}(a)")
    out["mul_add"] = _ret(f"vfmaq_{s}(c, a, b)") if e.is_float else _ret(f"vmlaq_{s}(c, a, b)")
    if not e.is_float:
        out["bit_and"] = _ret(f"vandq_{s}(a, b)")
        out["bit_or"] = _ret(f"vorrq_{s}(a, b)")
        out["bit_xor"] = _ret(f"veorq_{s}(a, b)")
        out["bit_not"] = _ret(f"vmvnq_{s}(a)")
    for name, ins in (("eq", "vceqq"), ("lt", "vcltq"), ("le", "vcleq"), ("gt", "vcgtq"), ("ge", "vcgeq")):
        out[name] = _ret(f"{ins}_{s}(a, b)")
    out["ne"] = Impl(SHORT, f"return vmvnq_u32(vceqq_{s}(a, b));")
    out["select"] = _ret(f"vbslq_{s}(m, a, b)")

    # Lanes i and i + 2 first, then the two halves: the tree every kernel reduces in.
    half = {"f32": "float32x2_t", "i32": "int32x2_t", "u32": "uint32x2_t"}[t]
    for op, ins in (("add", "add"), ("min", "min"), ("max", "max")):
        out[f"reduce_{op}"] = Impl(SHORT, f"{half} const h = v{ins}_{s}(vget_low_{s}(a), vget_high_{s}(a));\n"
                                          f"return vget_lane_{s}(vp{ins}_{s}(h, h), 0);")
    if e.is_float:
        out["to_i32"] = _ret("vcvtq_s32_f32(a)")
    else:
        out["to_f32"] = _ret(f"vcvtq_f32_{s}(a)")
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    weights = "static constexpr u32 w[4] = {1, 2, 4, 8};\n"
    return {
        "bit_and": _ret("vandq_u32(a, b)"),
        "bit_or": _ret("vorrq_u32(a, b)"),
        "bit_xor": _ret("veorq_u32(a, b)"),
        "bit_not": _ret("vmvnq_u32(a)"),
        "bits": Impl(SHORT, f"{weights}return vaddvq_u32(vandq_u32(m, vld1q_u32(w)));"),
        "any": _ret("vmaxvq_u32(m) != 0"),
        "all": _ret("vminvq_u32(m) != 0"),
        "from_bits": Impl(SHORT, f"{weights}return vtstq_u32(vdupq_n_u32(b), vld1q_u32(w));"),
    }
