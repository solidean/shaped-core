"""The neon kernel: arm64 AdvSIMD, 128-bit registers, masks as all-ones lanes.

MSVC's arm64 NEON types are one union under different names, so nothing here overloads on a register type, and every
constant vector is loaded from an array rather than brace-initialized.
"""

from __future__ import annotations

from .model import CONVERSIONS, ELEM, EMULATED, SHORT, SINGLE, Elem, Impl

_SFX = {"f32": "f32", "f64": "f64", "i8": "s8", "i16": "s16", "i32": "s32", "i64": "s64",
        "u8": "u8", "u16": "u16", "u32": "u32", "u64": "u64"}
_TYPE = {"f32": "float32x4_t", "f64": "float64x2_t", "i8": "int8x16_t", "i16": "int16x8_t", "i32": "int32x4_t",
         "i64": "int64x2_t", "u8": "uint8x16_t", "u16": "uint16x8_t", "u32": "uint32x4_t", "u64": "uint64x2_t"}
_MASK = {8: ("uint8x16_t", "u8"), 16: ("uint16x8_t", "u16"), 32: ("uint32x4_t", "u32"), 64: ("uint64x2_t", "u64")}
# The shift-count vector for a lane width: vshlq takes a signed count per lane, negative to shift right.
_COUNT = {8: "s8", 16: "s16", 32: "s32", 64: "s64"}


def reg_type(e: Elem, w: int) -> str:
    return _TYPE[e.name]


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    return _MASK[lane_bits][0]


def _ret(expr: str, cost: str = SINGLE) -> Impl:
    return Impl(cost, f"return {expr};")


def _gather(e: Elem, lanes: int) -> Impl:
    """NEON has no gather: the indices through memory, then one load per lane."""
    t = e.name
    i = f"i{e.bits}"
    body = [f"{i} x[{lanes}];", f"{t} r[{lanes}];", f"vst1q_{_SFX[i]}(x, idx);"]
    body += [f"r[{k}] = p[x[{k}]];" for k in range(lanes)]
    body += [f"return vld1q_{_SFX[t]}(r);"]
    return Impl(EMULATED, "\n".join(body))


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    s = _SFX[e.name]
    t = e.name
    lanes = w // e.bits
    ms = _MASK[e.bits][1]
    out: dict[str, Impl] = {}
    wide64 = e.bits == 64 and not e.is_float

    out["broadcast"] = _ret(f"vdupq_n_{s}(x)")
    out["zero"] = _ret(f"vdupq_n_{s}({t}(0))")
    iota = ", ".join(str(i) for i in range(lanes))
    out["iota"] = Impl(SINGLE, f"static constexpr {t} k[{lanes}] = {{{iota}}};\n"
                               f"return vaddq_{s}(vdupq_n_{s}(start), vld1q_{s}(k));")
    out["load"] = _ret(f"vld1q_{s}(p)")
    out["load_aligned"] = _ret(f"vld1q_{s}(p)")
    out["store"] = Impl(SINGLE, f"vst1q_{s}(p, a);")
    out["store_aligned"] = Impl(SINGLE, f"vst1q_{s}(p, a);")
    out["add"] = _ret(f"vaddq_{s}(a, b)")
    out["sub"] = _ret(f"vsubq_{s}(a, b)")

    if wide64:
        # No 64-bit multiply: each lane multiplied as a scalar, through memory.
        out["mul"] = Impl(EMULATED, f"{t} x[2], y[2];\nvst1q_{s}(x, a);\nvst1q_{s}(y, b);\n"
                                    f"x[0] = {t}(u64(x[0]) * u64(y[0]));\nx[1] = {t}(u64(x[1]) * u64(y[1]));\n"
                                    f"return vld1q_{s}(x);")
        out["min"] = Impl(SHORT, f"return vbslq_{s}(vcltq_{s}(b, a), b, a);")
        out["max"] = Impl(SHORT, f"return vbslq_{s}(vcltq_{s}(a, b), b, a);")
        out["mul_add"] = Impl(EMULATED, "return add(mul(a, b), c);")
    else:
        out["mul"] = _ret(f"vmulq_{s}(a, b)")
        out["min"] = _ret(f"vminq_{s}(a, b)")
        out["max"] = _ret(f"vmaxq_{s}(a, b)")
        out["mul_add"] = _ret(f"vfmaq_{s}(c, a, b)") if e.is_float else _ret(f"vmlaq_{s}(c, a, b)")

    if e.kind != "unsigned":
        out["neg"] = _ret(f"vnegq_{s}(a)")
        out["abs"] = _ret(f"vabsq_{s}(a)")

    if e.is_float:
        out["div"] = _ret(f"vdivq_{s}(a, b)")
        out["sqrt"] = _ret(f"vsqrtq_{s}(a)")
        out["floor"] = _ret(f"vrndmq_{s}(a)")
        out["ceil"] = _ret(f"vrndpq_{s}(a)")
        out["round"] = _ret(f"vrndnq_{s}(a)")
        out["trunc"] = _ret(f"vrndq_{s}(a)")
        out["copysign"] = _ret(f"vbslq_{s}(vdupq_n_{ms}({ms}(1) << {e.bits - 1}), b, a)")
    else:
        out["bit_and"] = _ret(f"vandq_{s}(a, b)")
        out["bit_or"] = _ret(f"vorrq_{s}(a, b)")
        out["bit_xor"] = _ret(f"veorq_{s}(a, b)")
        out["bit_not"] = (Impl(SINGLE, f"return veorq_{s}(a, vdupq_n_{s}({t}(~0ull)));") if e.bits == 64
                          else _ret(f"vmvnq_{s}(a)"))
        c = _COUNT[e.bits]
        out["shl"] = _ret(f"vshlq_{s}(a, vdupq_n_{c}(i{e.bits}(n)))")
        out["shr"] = _ret(f"vshlq_{s}(a, vdupq_n_{c}(i{e.bits}(-n)))")

    for name, ins in (("eq", "vceqq"), ("lt", "vcltq"), ("le", "vcleq"), ("gt", "vcgtq"), ("ge", "vcgeq")):
        out[name] = _ret(f"{ins}_{s}(a, b)")
    if e.bits == 64:
        out["ne"] = Impl(SHORT, f"return veorq_u64(vceqq_{s}(a, b), vdupq_n_u64(~0ull));")
    else:
        out["ne"] = Impl(SHORT, f"return vmvnq_{ms}(vceqq_{s}(a, b));")
    out["select"] = _ret(f"vbslq_{s}(m, a, b)")
    if e.bits == 64:
        out["reverse"] = _ret(f"vextq_{s}(a, a, 1)")
    else:
        out["reverse"] = Impl(SHORT, f"{_TYPE[t]} const r = vrev64q_{s}(a);\nreturn vextq_{s}(r, r, {lanes // 2});")
    out["gather"] = _gather(e, lanes)
    if e.is_float:
        # The estimate is good to 8 bits; one Newton step takes it past the 11 every kernel promises.
        out["rcp_approx"] = Impl(SHORT, f"{_TYPE[t]} const r = vrecpeq_{s}(a);\nreturn vmulq_{s}(r, vrecpsq_{s}(a, r));")
        # Four instructions, one past the rule's three.
        out["rsqrt_approx"] = Impl(EMULATED, f"{_TYPE[t]} const r = vrsqrteq_{s}(a);\n"
                                             f"return vmulq_{s}(r, vrsqrtsq_{s}(vmulq_{s}(a, r), r));")

    if e.is_float and e.bits == 32:
        # Lanes i and i + 2 first, then the two halves: the tree every kernel reduces in.
        for op, ins in (("add", "add"), ("min", "min"), ("max", "max")):
            out[f"reduce_{op}"] = Impl(SHORT, f"float32x2_t const h = v{ins}_f32(vget_low_f32(a), vget_high_f32(a));\n"
                                              f"return vget_lane_f32(vp{ins}_f32(h, h), 0);")
    elif e.bits == 64:
        for op in ("add", "min", "max"):
            out[f"reduce_{op}"] = Impl(SHORT, f"return vgetq_lane_{s}({op}(a, vextq_{s}(a, a, 1)), 0);")
    else:
        # Integer reductions are exact, so their order is free: the across-vector instructions.
        out["reduce_add"] = _ret(f"{t}(vaddvq_{s}(a))")
        out["reduce_min"] = _ret(f"vminvq_{s}(a)")
        out["reduce_max"] = _ret(f"vmaxvq_{s}(a)")

    for target in CONVERSIONS.get(t, []):
        out[f"to_{target}"] = _ret(f"vcvtq_{_SFX[target]}_{s}(a)")
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    mt, s = _MASK[lane_bits]
    lanes = w // lane_bits
    weights = ", ".join(str(1 << (i % 8 if lane_bits == 8 else i)) for i in range(lanes))
    table = f"static constexpr {s} w[{lanes}] = {{{weights}}};\n"
    out = {
        "bit_and": _ret(f"vandq_{s}(a, b)"),
        "bit_or": _ret(f"vorrq_{s}(a, b)"),
        "bit_xor": _ret(f"veorq_{s}(a, b)"),
    }
    if lane_bits == 64:
        out["bit_not"] = _ret("veorq_u64(a, vdupq_n_u64(~0ull))")
        out["bits"] = Impl(SHORT, f"{table}return vaddvq_u64(vandq_u64(m, vld1q_u64(w)));")
        out["any"] = _ret("vmaxvq_u32(vreinterpretq_u32_u64(m)) != 0")
        out["all"] = _ret("vminvq_u32(vreinterpretq_u32_u64(m)) != 0")
        out["from_bits"] = Impl(SHORT, f"{table}return vtstq_u64(vdupq_n_u64(b), vld1q_u64(w));")
    elif lane_bits == 8:
        out["bit_not"] = _ret("vmvnq_u8(a)")
        out["bits"] = Impl(SHORT, f"{table}uint8x16_t const x = vandq_u8(m, vld1q_u8(w));\n"
                                  "return u64(vaddv_u8(vget_low_u8(x))) | (u64(vaddv_u8(vget_high_u8(x))) << 8);")
        out["any"] = _ret("vmaxvq_u8(m) != 0")
        out["all"] = _ret("vminvq_u8(m) != 0")
        out["from_bits"] = Impl(SHORT, f"{table}return vtstq_u8(vcombine_u8(vdup_n_u8(u8(b)), vdup_n_u8(u8(b >> 8))), "
                                       "vld1q_u8(w));")
    else:
        out["bit_not"] = _ret(f"vmvnq_{s}(a)")
        out["bits"] = Impl(SHORT, f"{table}return vaddvq_{s}(vandq_{s}(m, vld1q_{s}(w)));")
        out["any"] = _ret(f"vmaxvq_{s}(m) != 0")
        out["all"] = _ret(f"vminvq_{s}(m) != 0")
        out["from_bits"] = Impl(SHORT, f"{table}return vtstq_{s}(vdupq_n_{s}({s}(b)), vld1q_{s}(w));")
    return out
