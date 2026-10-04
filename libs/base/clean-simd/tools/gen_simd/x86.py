"""The x86 kernels — sse2, sse42, avx2, avx512 — as one module, since most bodies differ only by width and level.

Level 0 is SSE2 (x86-64-v1), 1 SSE4.2 (v2), 2 AVX2 with FMA (v3), 3 AVX-512 F/BW/CD/DQ/VL (v4).
Below level 3 a mask is an integer register of all-ones lanes; at level 3 it is a k-register.
"""

from __future__ import annotations

from .model import EMULATED, SHORT, SINGLE, Elem, Impl

LEVEL = {"sse2": 0, "sse42": 1, "avx2": 2, "avx512": 3}

_P = {128: "_mm", 256: "_mm256", 512: "_mm512"}
_F = {128: "__m128", 256: "__m256", 512: "__m512"}
_I = {128: "__m128i", 256: "__m256i", 512: "__m512i"}
_SI = {128: "si128", 256: "si256", 512: "si512"}

_PRED = {"eq": "_CMP_EQ_OQ", "ne": "_CMP_NEQ_UQ", "lt": "_CMP_LT_OQ", "le": "_CMP_LE_OQ", "gt": "_CMP_GT_OQ",
         "ge": "_CMP_GE_OQ"}
_SSE_CMP = {"eq": "cmpeq", "ne": "cmpneq", "lt": "cmplt", "le": "cmple", "gt": "cmpgt", "ge": "cmpge"}


def reg_type(e: Elem, w: int) -> str:
    return _F[w] if e.is_float else _I[w]


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    if LEVEL[kernel] < 3:
        return _I[w]
    lanes = w // lane_bits
    return "__mmask8" if lanes <= 8 else f"__mmask{lanes}"


def _ret(expr: str) -> Impl:
    return Impl(SINGLE, f"return {expr};")


def _descending(values: list[str]) -> str:
    return ", ".join(reversed(values))


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    lvl = LEVEL[kernel]
    p = _P[w]
    lanes = w // e.bits
    out: dict[str, Impl] = {}
    one_wide = lvl >= 3  # k-register masks

    if e.is_float:
        out["broadcast"] = _ret(f"{p}_set1_ps(x)")
        out["zero"] = _ret(f"{p}_setzero_ps()")
        out["iota"] = _ret(f"{p}_add_ps({p}_set1_ps(start), {p}_set_ps({_descending([f'{i}.f' for i in range(lanes)])}))")
        out["load"] = _ret(f"{p}_loadu_ps(p)")
        out["load_aligned"] = _ret(f"{p}_load_ps(p)")
        out["store"] = Impl(SINGLE, f"{p}_storeu_ps(p, a);")
        out["store_aligned"] = Impl(SINGLE, f"{p}_store_ps(p, a);")
        for name in ("add", "sub", "mul", "min", "max"):
            out[name] = _ret(f"{p}_{name}_ps(a, b)")
        out["neg"] = _ret(f"{p}_xor_ps(a, {p}_set1_ps(-0.f))")
        out["abs"] = _ret(f"{p}_abs_ps(a)") if (lvl >= 3 and w == 512) else _ret(f"{p}_andnot_ps({p}_set1_ps(-0.f), a)")
        out["mul_add"] = _ret(f"{p}_fmadd_ps(a, b, c)") if lvl >= 2 else Impl(SHORT, f"return {p}_add_ps({p}_mul_ps(a, b), c);")
        for name, pred in _PRED.items():
            if one_wide:
                out[name] = _ret(f"{p}_cmp_ps_mask(a, b, {pred})")
            elif lvl >= 2:
                out[name] = _ret(f"{p}_castps_{_SI[w]}({p}_cmp_ps(a, b, {pred}))")
            else:
                out[name] = _ret(f"_mm_castps_si128(_mm_{_SSE_CMP[name]}_ps(a, b))")
        if one_wide:
            out["select"] = _ret(f"{p}_mask_blend_ps(m, b, a)")
        elif lvl >= 1:
            out["select"] = _ret(f"{p}_blendv_ps(b, a, {p}_castsi{w}_ps(m))")
        else:
            out["select"] = Impl(SHORT, "__m128 const f = _mm_castsi128_ps(m);\n"
                                        "return _mm_or_ps(_mm_and_ps(f, a), _mm_andnot_ps(f, b));")
        for op in ("add", "min", "max"):
            if w == 128:
                body = (f"__m128 t = {op}(a, _mm_movehl_ps(a, a));\n"
                        f"t = {op}(t, _mm_shuffle_ps(t, t, 1));\n"
                        "return _mm_cvtss_f32(t);")
            elif w == 256:
                body = (f"return reg<f32, {kernel}, 128>::reduce_{op}("
                        f"reg<f32, {kernel}, 128>::{op}(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1)));")
            else:
                body = (f"return reg<f32, {kernel}, 256>::reduce_{op}("
                        f"reg<f32, {kernel}, 256>::{op}(_mm512_castps512_ps256(a), _mm512_extractf32x8_ps(a, 1)));")
            out[f"reduce_{op}"] = Impl(SHORT, body)
        out["to_i32"] = _ret(f"{p}_cvttps_epi32(a)")
        return out

    # 32-bit integers
    signed = e.is_signed
    su = "epi32" if signed else "epu32"
    si = _SI[w]
    if w == 512:
        out["load"] = _ret("_mm512_loadu_si512(p)")
        out["load_aligned"] = _ret("_mm512_load_si512(p)")
        out["store"] = Impl(SINGLE, "_mm512_storeu_si512(p, a);")
        out["store_aligned"] = Impl(SINGLE, "_mm512_store_si512(p, a);")
    else:
        out["load"] = _ret(f"{p}_loadu_{si}(reinterpret_cast<{_I[w]} const*>(p))")
        out["load_aligned"] = _ret(f"{p}_load_{si}(reinterpret_cast<{_I[w]} const*>(p))")
        out["store"] = Impl(SINGLE, f"{p}_storeu_{si}(reinterpret_cast<{_I[w]}*>(p), a);")
        out["store_aligned"] = Impl(SINGLE, f"{p}_store_{si}(reinterpret_cast<{_I[w]}*>(p), a);")
    out["broadcast"] = _ret(f"{p}_set1_epi32(int(x))")
    out["zero"] = _ret(f"{p}_setzero_{si}()")
    out["iota"] = _ret(f"{p}_add_epi32({p}_set1_epi32(int(start)), {p}_set_epi32({_descending([str(i) for i in range(lanes)])}))")
    out["add"] = _ret(f"{p}_add_epi32(a, b)")
    out["sub"] = _ret(f"{p}_sub_epi32(a, b)")
    if lvl >= 1:
        out["mul"] = _ret(f"{p}_mullo_epi32(a, b)")
    else:
        out["mul"] = Impl(EMULATED,
                          "__m128i const even = _mm_mul_epu32(a, b);\n"
                          "__m128i const odd = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));\n"
                          "return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),\n"
                          "                          _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));")
    out["mul_add"] = Impl(SHORT if lvl >= 1 else EMULATED, "return add(mul(a, b), c);")
    out["bit_and"] = _ret(f"{p}_and_{si}(a, b)")
    out["bit_or"] = _ret(f"{p}_or_{si}(a, b)")
    out["bit_xor"] = _ret(f"{p}_xor_{si}(a, b)")
    out["bit_not"] = _ret(f"{p}_xor_{si}(a, {p}_set1_epi32(-1))")
    if signed:
        out["neg"] = _ret(f"{p}_sub_epi32({p}_setzero_{si}(), a)")
        if lvl >= 1:
            out["abs"] = _ret(f"{p}_abs_epi32(a)")
        else:
            out["abs"] = Impl(SHORT, "__m128i const s = _mm_srai_epi32(a, 31);\n"
                                     "return _mm_sub_epi32(_mm_xor_si128(a, s), s);")

    if lvl >= 1:
        out["min"] = _ret(f"{p}_min_{su}(a, b)")
        out["max"] = _ret(f"{p}_max_{su}(a, b)")
    elif signed:
        out["min"] = Impl(SHORT, "return select(_mm_cmpgt_epi32(a, b), b, a);")
        out["max"] = Impl(SHORT, "return select(_mm_cmpgt_epi32(a, b), a, b);")
    else:
        flip = "__m128i const f = _mm_set1_epi32(int(0x80000000u));\n"
        gt = "_mm_cmpgt_epi32(_mm_xor_si128(a, f), _mm_xor_si128(b, f))"
        out["min"] = Impl(EMULATED, f"{flip}return select({gt}, b, a);")
        out["max"] = Impl(EMULATED, f"{flip}return select({gt}, a, b);")

    if one_wide:
        for name, cmp in (("eq", "cmpeq"), ("ne", "cmpneq"), ("lt", "cmplt"), ("le", "cmple"), ("gt", "cmpgt"),
                          ("ge", "cmpge")):
            out[name] = _ret(f"{p}_{cmp}_{su}_mask(a, b)")
        out["select"] = _ret(f"{p}_mask_blend_epi32(m, b, a)")
    else:
        out["eq"] = _ret(f"{p}_cmpeq_epi32(a, b)")
        if signed:
            out["gt"] = _ret(f"{p}_cmpgt_epi32(a, b)")
            out["lt"] = _ret(f"{p}_cmpgt_epi32(b, a)")
        else:
            flip = f"{_I[w]} const f = {p}_set1_epi32(int(0x80000000u));\n"
            out["gt"] = Impl(SHORT, f"{flip}return {p}_cmpgt_epi32({p}_xor_{si}(a, f), {p}_xor_{si}(b, f));")
            out["lt"] = Impl(SHORT, f"{flip}return {p}_cmpgt_epi32({p}_xor_{si}(b, f), {p}_xor_{si}(a, f));")
        out["ne"] = Impl(SHORT, "return mr::bit_not(eq(a, b));")
        out["le"] = Impl(SHORT, "return mr::bit_not(gt(a, b));")
        out["ge"] = Impl(SHORT, "return mr::bit_not(lt(a, b));")
        if lvl >= 1:
            out["select"] = _ret(f"{p}_blendv_epi8(b, a, m)")
        else:
            out["select"] = Impl(SHORT, "return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));")

    t = e.name
    for op in ("add", "min", "max"):
        if w == 128:
            body = (f"__m128i t = {op}(a, _mm_shuffle_epi32(a, _MM_SHUFFLE(1, 0, 3, 2)));\n"
                    f"t = {op}(t, _mm_shuffle_epi32(t, _MM_SHUFFLE(2, 3, 0, 1)));\n"
                    f"return {t}(_mm_cvtsi128_si32(t));")
        elif w == 256:
            body = (f"return reg<{t}, {kernel}, 128>::reduce_{op}("
                    f"reg<{t}, {kernel}, 128>::{op}(_mm256_castsi256_si128(a), _mm256_extracti128_si256(a, 1)));")
        else:
            body = (f"return reg<{t}, {kernel}, 256>::reduce_{op}("
                    f"reg<{t}, {kernel}, 256>::{op}(_mm512_castsi512_si256(a), _mm512_extracti32x8_epi32(a, 1)));")
        out[f"reduce_{op}"] = Impl(SHORT, body)

    if signed:
        out["to_f32"] = _ret(f"{p}_cvtepi32_ps(a)")
    elif lvl >= 3:
        out["to_f32"] = _ret(f"{p}_cvtepu32_ps(a)")
    else:
        # Two exact halves: the high one scaled by 2^16 is exact, so the sum rounds once, as a native conversion would.
        out["to_f32"] = Impl(EMULATED,
                             f"{_F[w]} const lo = {p}_cvtepi32_ps({p}_and_{si}(a, {p}_set1_epi32(0xFFFF)));\n"
                             f"{_F[w]} const hi = {p}_cvtepi32_ps({p}_srli_epi32(a, 16));\n"
                             f"return {p}_add_ps({p}_mul_ps(hi, {p}_set1_ps(65536.f)), lo);")
    return out


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    p = _P[w]
    si = _SI[w]
    lanes = w // lane_bits
    full = f"0x{(1 << lanes) - 1:X}u"
    out: dict[str, Impl] = {}
    if LEVEL[kernel] >= 3:
        mt = mask_type(kernel, lane_bits, w)
        out["bit_and"] = _ret(f"{mt}(a & b)")
        out["bit_or"] = _ret(f"{mt}(a | b)")
        out["bit_xor"] = _ret(f"{mt}(a ^ b)")
        out["bit_not"] = _ret(f"{mt}(~a & {full})")
        out["bits"] = _ret("u32(m)")
        out["any"] = _ret("m != 0")
        out["all"] = _ret(f"(u32(m) & {full}) == {full}")
        out["from_bits"] = _ret(f"{mt}(b & {full})")
        return out

    out["bit_and"] = _ret(f"{p}_and_{si}(a, b)")
    out["bit_or"] = _ret(f"{p}_or_{si}(a, b)")
    out["bit_xor"] = _ret(f"{p}_xor_{si}(a, b)")
    out["bit_not"] = _ret(f"{p}_xor_{si}(a, {p}_set1_epi32(-1))")
    out["bits"] = _ret(f"u32({p}_movemask_ps({p}_castsi{w}_ps(m)))")
    out["any"] = _ret("bits(m) != 0")
    out["all"] = _ret(f"bits(m) == {full}")
    weights = _descending([str(1 << i) for i in range(lanes)])
    out["from_bits"] = Impl(SHORT, f"{_I[w]} const w = {p}_set_epi32({weights});\n"
                                   f"return {p}_cmpeq_epi32({p}_and_{si}({p}_set1_epi32(int(b)), w), w);")
    return out
