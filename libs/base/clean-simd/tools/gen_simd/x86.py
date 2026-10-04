"""The x86 kernels — sse2, sse42, avx2, avx512 — as one module, since most bodies differ only by width and level.

Level 0 is SSE2 (x86-64-v1), 1 SSE4.2 (v2), 2 AVX2 with FMA (v3), 3 AVX-512 F/BW/CD/DQ/VL (v4).
Below level 3 a mask is an integer register of all-ones lanes; at level 3 it is a k-register.
Where a level lacks an instruction the body says how it is emulated, and the cost class says how much that costs.
"""

from __future__ import annotations

from .model import CONVERSIONS, ELEM, EMULATED, SHORT, SINGLE, Elem, Impl, negated

LEVEL = {"sse2": 0, "sse42": 1, "avx2": 2, "avx512": 3}

_P = {128: "_mm", 256: "_mm256", 512: "_mm512"}
_I = {128: "__m128i", 256: "__m256i", 512: "__m512i"}
_SI = {128: "si128", 256: "si256", 512: "si512"}
_PRED = {"eq": "_CMP_EQ_OQ", "ne": "_CMP_NEQ_UQ", "lt": "_CMP_LT_OQ", "le": "_CMP_LE_OQ", "gt": "_CMP_GT_OQ",
         "ge": "_CMP_GE_OQ"}
_SSE_CMP = {"eq": "cmpeq", "ne": "cmpneq", "lt": "cmplt", "le": "cmple", "gt": "cmpgt", "ge": "cmpge"}


def _f(e: Elem, w: int) -> str:
    return {(32, 128): "__m128", (32, 256): "__m256", (32, 512): "__m512",
            (64, 128): "__m128d", (64, 256): "__m256d", (64, 512): "__m512d"}[(e.bits, w)]


def reg_type(e: Elem, w: int) -> str:
    return _f(e, w) if e.is_float else _I[w]


def mask_type(kernel: str, lane_bits: int, w: int) -> str:
    if LEVEL[kernel] < 3:
        return _I[w]
    lanes = w // lane_bits
    return "__mmask8" if lanes <= 8 else f"__mmask{lanes}"


def _ret(expr: str, cost: str = SINGLE) -> Impl:
    return Impl(cost, f"return {expr};")


def _set1(b: int, w: int, x: str) -> str:
    if b == 64:
        return f"{_P[w]}_set1_epi64x(i64({x}))" if w != 512 else f"_mm512_set1_epi64(i64({x}))"
    return f"{_P[w]}_set1_epi{b}(i{b}({x}))"


def _iota(t: str, w: int, lanes: int) -> str:
    values = ", ".join(str(i) for i in range(lanes))
    return f"alignas({w // 8}) static constexpr {t} k[{lanes}] = {{{values}}};\n"


def _through_memory(e: Elem, to: Elem, w: int, expr: str) -> Impl:
    """Lane by lane through a buffer: the honest spelling of a conversion this level has no instruction for."""
    lanes = w // e.bits
    p = _P[w]
    if e.is_float:
        store = f"{p}_store_{'ps' if e.bits == 32 else 'pd'}(x, a);"
    else:
        store = "_mm512_store_si512(x, a);" if w == 512 else f"{p}_store_{_SI[w]}(reinterpret_cast<{_I[w]}*>(x), a);"
    if to.is_float:
        load = f"{p}_load_{'ps' if to.bits == 32 else 'pd'}(r)"
    else:
        load = "_mm512_load_si512(r)" if w == 512 else f"{p}_load_{_SI[w]}(reinterpret_cast<{_I[w]} const*>(r))"
    body = [f"alignas({w // 8}) {e.name} x[{lanes}];", f"alignas({w // 8}) {to.name} r[{lanes}];", store]
    body += [f"r[{i}] = {expr.format(i=i)};" for i in range(lanes)]
    body += [f"return {load};"]
    return Impl(EMULATED, "\n".join(body))


def _table(t: str, w: int, values: list[int]) -> str:
    """A constant vector as an aligned array named k, for the bodies that load one."""
    return f"alignas({w // 8}) static constexpr {t} k[{len(values)}] = {{{', '.join(str(v) for v in values)}}};\n"


def _load_k(w: int) -> str:
    return "_mm512_load_si512(k)" if w == 512 else f"{_P[w]}_load_{_SI[w]}(reinterpret_cast<{_I[w]} const*>(k))"


def _byte_reverse(lane_bits: int, w: int) -> list[int]:
    """pshufb indices reversing the lanes within each 128-bit block."""
    size = lane_bits // 8
    return [(16 // size - 1 - j // size) * size + j % size for j in range(16)] * (w // 128)


def _gather(kernel: str, e: Elem, w: int) -> Impl:
    """AVX2 gathers 32- and 64-bit lanes; anything else goes through memory, one load per lane."""
    p = _P[w]
    b = e.bits
    if LEVEL[kernel] >= 2 and b >= 32:
        fn = {32: "i32gather_ps", 64: "i64gather_pd"}[b] if e.is_float else {32: "i32gather_epi32", 64: "i64gather_epi64"}[b]
        if w == 512:
            return _ret(f"_mm512_{fn}(idx, p, {b // 8})")
        base = "p" if e.is_float else ("reinterpret_cast<int const*>(p)" if b == 32 else "reinterpret_cast<long long const*>(p)")
        return _ret(f"{p}_{fn}({base}, idx, {b // 8})")
    lanes = w // b
    store = "_mm512_store_si512(x, idx);" if w == 512 else f"{p}_store_{_SI[w]}(reinterpret_cast<{_I[w]}*>(x), idx);"
    body = [f"alignas({w // 8}) i{b} x[{lanes}];", f"alignas({w // 8}) {e.name} r[{lanes}];", store]
    body += [f"r[{k}] = p[x[{k}]];" for k in range(lanes)]
    return Impl(EMULATED, "\n".join(body + ["return load_aligned(r);"]))


def _float_ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    lvl = LEVEL[kernel]
    p = _P[w]
    s = "ps" if e.bits == 32 else "pd"
    ft = _f(e, w)
    si = _SI[w]
    lanes = w // e.bits
    k = lvl >= 3
    t = e.name
    out: dict[str, Impl] = {}

    out["broadcast"] = _ret(f"{p}_set1_{s}(x)")
    out["zero"] = _ret(f"{p}_setzero_{s}()")
    out["iota"] = Impl(SINGLE, _iota(t, w, lanes) + f"return {p}_add_{s}({p}_set1_{s}(start), {p}_load_{s}(k));")
    out["load"] = _ret(f"{p}_loadu_{s}(p)")
    out["load_aligned"] = _ret(f"{p}_load_{s}(p)")
    out["store"] = Impl(SINGLE, f"{p}_storeu_{s}(p, a);")
    out["store_aligned"] = Impl(SINGLE, f"{p}_store_{s}(p, a);")
    for name in ("add", "sub", "mul", "div"):
        out[name] = _ret(f"{p}_{name}_{s}(a, b)")
    # x86 returns the second operand on a tie or a NaN, so swapping them returns `a`, as scalar and simd128 do.
    for name in ("min", "max"):
        out[name] = _ret(f"{p}_{name}_{s}(b, a)")
    out["sqrt"] = _ret(f"{p}_sqrt_{s}(a)")
    sign = f"{p}_set1_{s}({'-0.f' if e.bits == 32 else '-0.0'})"
    out["neg"] = _ret(f"{p}_xor_{s}(a, {sign})")
    out["abs"] = _ret(f"{p}_andnot_{s}({sign}, a)")
    out["copysign"] = Impl(SHORT, f"{ft} const s = {sign};\nreturn {p}_or_{s}({p}_andnot_{s}(s, a), {p}_and_{s}(s, b));")
    out["mul_add"] = _ret(f"{p}_fmadd_{s}(a, b, c)") if lvl >= 2 else _ret(f"{p}_add_{s}({p}_mul_{s}(a, b), c)", SHORT)

    for name, pred in _PRED.items():
        if k:
            out[name] = _ret(f"{p}_cmp_{s}_mask(a, b, {pred})")
        elif lvl >= 2:
            out[name] = _ret(f"{p}_cast{s}_{si}({p}_cmp_{s}(a, b, {pred}))")
        else:
            out[name] = _ret(f"_mm_cast{s}_si128(_mm_{_SSE_CMP[name]}_{s}(a, b))")
    if k:
        out["select"] = _ret(f"{p}_mask_blend_{s}(m, b, a)")
    elif lvl >= 1:
        out["select"] = _ret(f"{p}_blendv_{s}(b, a, {p}_castsi{w}_{s}(m))")
    else:
        out["select"] = Impl(SHORT, f"{ft} const f = _mm_castsi128_{s}(m);\n"
                                    f"return _mm_or_{s}(_mm_and_{s}(f, a), _mm_andnot_{s}(f, b));")

    if w == 128:
        out["reverse"] = _ret("_mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 1, 2, 3))" if e.bits == 32 else "_mm_shuffle_pd(a, a, 1)")
    elif w == 256 and e.bits == 32:
        out["reverse"] = Impl(SINGLE, _table("i32", w, list(range(7, -1, -1))) + f"return _mm256_permutevar8x32_ps(a, {_load_k(w)});")
    elif w == 256:
        out["reverse"] = _ret("_mm256_permute4x64_pd(a, _MM_SHUFFLE(0, 1, 2, 3))")
    else:
        it = f"i{e.bits}"
        out["reverse"] = Impl(SINGLE, _table(it, w, list(range(lanes - 1, -1, -1)))
                              + f"return _mm512_permutexvar_{s}({_load_k(w)}, a);")
    out["gather"] = _gather(kernel, e, w)
    if k:
        out["rcp_approx"] = _ret(f"{p}_rcp14_{s}(a)")
        out["rsqrt_approx"] = _ret(f"{p}_rsqrt14_{s}(a)")
    elif e.bits == 32:
        out["rcp_approx"] = _ret(f"{p}_rcp_ps(a)")
        out["rsqrt_approx"] = _ret(f"{p}_rsqrt_ps(a)")
    else:
        # No double estimate below AVX-512: the exact quotient.
        out["rcp_approx"] = _ret(f"{p}_div_pd({p}_set1_pd(1.0), a)")
        out["rsqrt_approx"] = Impl(SHORT, f"return {p}_div_pd({p}_set1_pd(1.0), {p}_sqrt_pd(a));")

    if lvl >= 1:
        modes = {"floor": "_MM_FROUND_TO_NEG_INF", "ceil": "_MM_FROUND_TO_POS_INF",
                 "round": "_MM_FROUND_TO_NEAREST_INT", "trunc": "_MM_FROUND_TO_ZERO"}
        for name, mode in modes.items():
            if w == 512:
                out[name] = _ret(f"_mm512_roundscale_{s}(a, {mode} | _MM_FROUND_NO_EXC)")
            else:
                out[name] = _ret(f"{p}_round_{s}(a, {mode} | _MM_FROUND_NO_EXC)")
    else:
        # SSE2 has no rounding instruction, so nearest adds and subtracts 2^mantissa to the magnitude.
        # Floor, ceil and trunc step off it by one where it overshot.
        # Above 2^mantissa every float is integral already.
        # Stepping -1 up by one gives +0, so the input's sign is ORed back in: rounding never changes a sign, and a
        # NaN keeps its own.
        # Nearest carries it already.
        big = "8388608.f" if e.bits == 32 else "4503599627370496.0"
        one = f"_mm_set1_{s}({'1.f' if e.bits == 32 else '1.0'})"
        nearest = (f"{ft} const s = _mm_and_{s}(a, {sign});\n"
                   f"{ft} const ax = _mm_xor_{s}(a, s);\n"
                   f"{ft} const big = _mm_set1_{s}({big});\n"
                   f"{ft} const r = _mm_or_{s}(_mm_sub_{s}(_mm_add_{s}(ax, big), big), s);\n"
                   f"{ft} const n = select(lt(ax, big), r, a);\n")
        down = f"_mm_sub_{s}(n, _mm_and_{s}(_mm_castsi128_{s}(gt(n, a)), {one}))"
        up = f"_mm_add_{s}(n, _mm_and_{s}(_mm_castsi128_{s}(lt(n, a)), {one}))"
        signed = lambda x: f"return _mm_or_{s}({x}, s);"  # noqa: E731
        out["round"] = Impl(EMULATED, nearest + "return n;")
        out["floor"] = Impl(EMULATED, nearest + signed(down))
        out["ceil"] = Impl(EMULATED, nearest + signed(up))
        out["trunc"] = Impl(EMULATED, nearest + signed(f"select(lt(a, _mm_setzero_{s}()), {up}, {down})"))

    for op in ("add", "min", "max"):
        if w == 128 and e.bits == 32:
            body = (f"__m128 t = {op}(a, _mm_movehl_ps(a, a));\n"
                    f"t = {op}(t, _mm_shuffle_ps(t, t, 1));\n"
                    "return _mm_cvtss_f32(t);")
        elif w == 128:
            body = f"return _mm_cvtsd_f64({op}(a, _mm_unpackhi_pd(a, a)));"
        elif w == 256:
            body = (f"return reg<{t}, {kernel}, 128>::reduce_{op}("
                    f"reg<{t}, {kernel}, 128>::{op}(_mm256_cast{s}256_{s}128(a), _mm256_extractf128_{s}(a, 1)));")
        else:
            hi = "_mm512_extractf32x8_ps(a, 1)" if e.bits == 32 else "_mm512_extractf64x4_pd(a, 1)"
            body = (f"return reg<{t}, {kernel}, 256>::reduce_{op}("
                    f"reg<{t}, {kernel}, 256>::{op}(_mm512_cast{s}512_{s}256(a), {hi}));")
        out[f"reduce_{op}"] = Impl(SHORT, body)

    for target in CONVERSIONS.get(t, []):
        if e.bits == 32:
            out[f"to_{target}"] = _ret(f"{p}_cvttps_epi32(a)")
        elif k:
            out[f"to_{target}"] = _ret(f"{p}_cvttpd_epi64(a)")
        else:
            out[f"to_{target}"] = _through_memory(
                e, ELEM[target], w,
                "x[{i}] != x[{i}] ? i64(0) : x[{i}] >= 9223372036854775808.0 ? i64(~0ull >> 1) "
                ": x[{i}] < -9223372036854775808.0 ? i64(-i64(~0ull >> 1) - 1) : i64(x[{i}])")
    return out


def _int_ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    lvl = LEVEL[kernel]
    p = _P[w]
    b = e.bits
    si = _SI[w]
    it = _I[w]
    lanes = w // b
    k = lvl >= 3
    t = e.name
    signed = e.is_signed
    su = f"epi{b}" if signed else f"epu{b}"
    out: dict[str, Impl] = {}

    if w == 512:
        out["load"] = _ret("_mm512_loadu_si512(p)")
        out["load_aligned"] = _ret("_mm512_load_si512(p)")
        out["store"] = Impl(SINGLE, "_mm512_storeu_si512(p, a);")
        out["store_aligned"] = Impl(SINGLE, "_mm512_store_si512(p, a);")
    else:
        out["load"] = _ret(f"{p}_loadu_{si}(reinterpret_cast<{it} const*>(p))")
        out["load_aligned"] = _ret(f"{p}_load_{si}(reinterpret_cast<{it} const*>(p))")
        out["store"] = Impl(SINGLE, f"{p}_storeu_{si}(reinterpret_cast<{it}*>(p), a);")
        out["store_aligned"] = Impl(SINGLE, f"{p}_store_{si}(reinterpret_cast<{it}*>(p), a);")
    out["broadcast"] = _ret(_set1(b, w, "x"))
    out["zero"] = _ret(f"{p}_setzero_{si}()")
    out["iota"] = Impl(SINGLE, _iota(t, w, lanes) + "return add(broadcast(start), load_aligned(k));")
    out["add"] = _ret(f"{p}_add_epi{b}(a, b)")
    out["sub"] = _ret(f"{p}_sub_epi{b}(a, b)")
    out["bit_and"] = _ret(f"{p}_and_{si}(a, b)")
    out["bit_or"] = _ret(f"{p}_or_{si}(a, b)")
    out["bit_xor"] = _ret(f"{p}_xor_{si}(a, b)")
    out["bit_not"] = _ret(f"{p}_xor_{si}(a, {p}_set1_epi32(-1))")

    # multiply
    if b == 16:
        out["mul"] = _ret(f"{p}_mullo_epi16(a, b)")
    elif b == 32 and lvl >= 1:
        out["mul"] = _ret(f"{p}_mullo_epi32(a, b)")
    elif b == 32:
        out["mul"] = Impl(EMULATED,
                          "__m128i const even = _mm_mul_epu32(a, b);\n"
                          "__m128i const odd = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));\n"
                          "return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),\n"
                          "                          _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));")
    elif b == 64 and k:
        out["mul"] = _ret(f"{p}_mullo_epi64(a, b)")
    elif b == 64:
        # The low halves' full product, plus the two cross products shifted into the high half.
        out["mul"] = Impl(EMULATED,
                          f"{it} const lo = {p}_mul_epu32(a, b);\n"
                          f"{it} const c1 = {p}_mul_epu32(a, {p}_srli_epi64(b, 32));\n"
                          f"{it} const c2 = {p}_mul_epu32({p}_srli_epi64(a, 32), b);\n"
                          f"return {p}_add_epi64(lo, {p}_slli_epi64({p}_add_epi64(c1, c2), 32));")
    else:
        # No 8-bit multiply on any x86 level: the even and odd bytes as 16-bit products, low bytes kept.
        out["mul"] = Impl(EMULATED,
                          f"{it} const even = {p}_mullo_epi16(a, b);\n"
                          f"{it} const odd = {p}_mullo_epi16({p}_srli_epi16(a, 8), {p}_srli_epi16(b, 8));\n"
                          f"return {p}_or_{si}({p}_slli_epi16(odd, 8), {p}_and_{si}(even, {_set1(16, w, '0xFF')}));")
    out["mul_add"] = Impl(SHORT if out["mul"].cost != EMULATED else EMULATED, "return add(mul(a, b), c);")

    # shifts by one count for every lane
    count = "_mm_cvtsi32_si128(n)"
    if b == 8:
        out["shl"] = Impl(SHORT, f"return {p}_and_{si}({p}_sll_epi16(a, {count}), {p}_set1_epi8(i8(u8(0xFF << n))));")
        if signed:
            out["shr"] = Impl(EMULATED,
                              f"{it} const lo = {p}_srai_epi16({p}_slli_epi16(a, 8), 8);\n"
                              f"{it} const hi = {p}_srai_epi16(a, 8);\n"
                              f"return {p}_or_{si}({p}_and_{si}({p}_sra_epi16(lo, {count}), {_set1(16, w, '0xFF')}),\n"
                              f"    {p}_slli_epi16({p}_sra_epi16(hi, {count}), 8));")
        else:
            out["shr"] = Impl(SHORT, f"return {p}_and_{si}({p}_srl_epi16(a, {count}), {p}_set1_epi8(i8(u8(0xFF >> n))));")
    else:
        out["shl"] = _ret(f"{p}_sll_epi{b}(a, {count})")
        if not signed:
            out["shr"] = _ret(f"{p}_srl_epi{b}(a, {count})")
        elif b < 64 or k:
            out["shr"] = _ret(f"{p}_sra_epi{b}(a, {count})")
        else:
            # No 64-bit arithmetic shift below AVX-512: shift logically, then extend the sign through a mask.
            out["shr"] = Impl(SHORT, f"{it} const m = {p}_srl_epi64({_set1(64, w, '0x8000000000000000ull')}, {count});\n"
                                     f"return {p}_sub_epi64({p}_xor_{si}({p}_srl_epi64(a, {count}), m), m);")

    # compares
    if k:
        for name, cmp in _SSE_CMP.items():
            out[name] = _ret(f"{p}_{cmp}_{su}_mask(a, b)")
    else:
        if b == 64 and lvl == 0:
            out["eq"] = Impl(SHORT, "__m128i const e = _mm_cmpeq_epi32(a, b);\n"
                                    "return _mm_and_si128(e, _mm_shuffle_epi32(e, _MM_SHUFFLE(2, 3, 0, 1)));")
            # Greater-than from 32-bit halves: the high halves decide unless equal, then the low ones, unsigned.
            pre = "" if signed else f"__m128i const s = {_set1(64, w, '0x8000000000000000ull')};\n"
            x = "a" if signed else "_mm_xor_si128(a, s)"
            y = "b" if signed else "_mm_xor_si128(b, s)"
            out["gt"] = Impl(EMULATED,
                             f"{pre}__m128i const x = {x};\n__m128i const y = {y};\n"
                             "__m128i const hi_gt = _mm_cmpgt_epi32(x, y);\n"
                             "__m128i const hi_eq = _mm_cmpeq_epi32(x, y);\n"
                             "__m128i const f = _mm_set1_epi32(int(0x80000000u));\n"
                             "__m128i const lo_gt = _mm_cmpgt_epi32(_mm_xor_si128(x, f), _mm_xor_si128(y, f));\n"
                             "__m128i const r = _mm_or_si128(hi_gt, _mm_and_si128(hi_eq, _mm_shuffle_epi32(lo_gt, "
                             "_MM_SHUFFLE(2, 2, 0, 0))));\n"
                             "return _mm_shuffle_epi32(r, _MM_SHUFFLE(3, 3, 1, 1));")
            out["lt"] = Impl(EMULATED, "return gt(b, a);")
        else:
            out["eq"] = _ret(f"{p}_cmpeq_epi{b}(a, b)")
            if signed:
                out["gt"] = _ret(f"{p}_cmpgt_epi{b}(a, b)")
                out["lt"] = _ret(f"{p}_cmpgt_epi{b}(b, a)")
            else:
                flip = f"{it} const f = {_set1(b, w, f'u{b}(1) << {b - 1}')};\n"
                out["gt"] = Impl(SHORT, f"{flip}return {p}_cmpgt_epi{b}({p}_xor_{si}(a, f), {p}_xor_{si}(b, f));")
                out["lt"] = Impl(SHORT, f"{flip}return {p}_cmpgt_epi{b}({p}_xor_{si}(b, f), {p}_xor_{si}(a, f));")
        out["ne"] = negated(out["eq"], "return mr::bit_not(eq(a, b));")
        if not signed and (b == 8 or (b in (16, 32) and lvl >= 1)):
            # a <= b exactly where max(a, b) is b: an unsigned max and an equality, with no sign flip.
            out["le"] = Impl(SHORT, f"return {p}_cmpeq_epi{b}({p}_max_epu{b}(a, b), b);")
            out["ge"] = Impl(SHORT, f"return {p}_cmpeq_epi{b}({p}_max_epu{b}(a, b), a);")
        else:
            out["le"] = negated(out["gt"], "return mr::bit_not(gt(a, b));")
            out["ge"] = negated(out["lt"], "return mr::bit_not(lt(a, b));")

    if k:
        out["select"] = _ret(f"{p}_mask_blend_epi{b}(m, b, a)")
    elif lvl >= 1:
        out["select"] = _ret(f"{p}_blendv_epi8(b, a, m)")
    else:
        out["select"] = Impl(SHORT, "return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));")

    # min and max: native where the level has them, else a compare and a select
    native_minmax = (k or (b == 8 and (not signed or lvl >= 1)) or (b == 16 and (signed or lvl >= 1))
                     or (b == 32 and lvl >= 1))
    if native_minmax:
        out["min"] = _ret(f"{p}_min_{su}(a, b)")
        out["max"] = _ret(f"{p}_max_{su}(a, b)")
    else:
        cost = EMULATED if out["gt"].cost == EMULATED else SHORT
        out["min"] = Impl(cost, "return select(gt(a, b), b, a);")
        out["max"] = Impl(cost, "return select(gt(a, b), a, b);")

    if signed:
        out["neg"] = _ret(f"{p}_sub_epi{b}({p}_setzero_{si}(), a)")
        if k or (b < 64 and lvl >= 1):
            out["abs"] = _ret(f"{p}_abs_epi{b}(a)")
        else:
            out["abs"] = Impl(EMULATED if out["lt"].cost == EMULATED else SHORT,
                              "return select(lt(a, zero()), neg(a), a);")

    # reverse: a lane shuffle where one exists, pshufb within each 128-bit block and a block swap above
    if b >= 32 and w == 128:
        out["reverse"] = _ret(f"_mm_shuffle_epi32(a, {'_MM_SHUFFLE(0, 1, 2, 3)' if b == 32 else '_MM_SHUFFLE(1, 0, 3, 2)'})")
    elif b == 32 and w == 256:
        out["reverse"] = Impl(SINGLE, _table("i32", w, list(range(7, -1, -1)))
                              + f"return _mm256_permutevar8x32_epi32(a, {_load_k(w)});")
    elif b == 64 and w == 256:
        out["reverse"] = _ret("_mm256_permute4x64_epi64(a, _MM_SHUFFLE(0, 1, 2, 3))")
    elif w == 512 and b >= 16:
        out["reverse"] = Impl(SINGLE, _table(f"i{b}", w, list(range(lanes - 1, -1, -1)))
                              + f"return _mm512_permutexvar_epi{b}({_load_k(w)}, a);")
    elif w == 128 and lvl >= 1:
        out["reverse"] = Impl(SINGLE, _table("u8", w, _byte_reverse(b, w)) + f"return _mm_shuffle_epi8(a, {_load_k(w)});")
    elif w == 256:
        out["reverse"] = Impl(SHORT, _table("u8", w, _byte_reverse(b, w))
                              + f"return _mm256_permute4x64_epi64(_mm256_shuffle_epi8(a, {_load_k(w)}), _MM_SHUFFLE(1, 0, 3, 2));")
    elif w == 512:
        out["reverse"] = Impl(SHORT, _table("u8", w, _byte_reverse(b, w))
                              + f"__m512i const t = _mm512_shuffle_epi8(a, {_load_k(w)});\n"
                              "return _mm512_shuffle_i64x2(t, t, _MM_SHUFFLE(0, 1, 2, 3));")
    else:
        # SSE2 has no byte shuffle: reverse the 16-bit lanes, after swapping the bytes within each for 8-bit lanes.
        words = ("__m128i const t = _mm_or_si128(_mm_slli_epi16(a, 8), _mm_srli_epi16(a, 8));\n" if b == 8
                 else "__m128i const t = a;\n")
        out["reverse"] = Impl(SHORT if b == 16 else EMULATED,
                              words + "return _mm_shuffle_epi32(_mm_shufflelo_epi16(_mm_shufflehi_epi16(t, 0x1B), 0x1B), "
                                      "_MM_SHUFFLE(1, 0, 3, 2));")
    out["gather"] = _gather(kernel, e, w)

    # reductions: lane i with lane i + n/2, by shifting the register down half its live bytes each step
    for op in ("add", "min", "max"):
        if w == 128:
            steps = []
            shift = 8
            while shift >= b // 8:
                steps.append(f"t = {op}(t, _mm_srli_si128(t, {shift}));")
                shift //= 2
            extract = f"{t}(_mm_cvtsi128_si64(t))" if b == 64 else f"{t}(_mm_cvtsi128_si32(t))"
            body = "__m128i t = a;\n" + "\n".join(steps) + f"\nreturn {extract};"
        elif w == 256:
            body = (f"return reg<{t}, {kernel}, 128>::reduce_{op}("
                    f"reg<{t}, {kernel}, 128>::{op}(_mm256_castsi256_si128(a), _mm256_extracti128_si256(a, 1)));")
        else:
            body = (f"return reg<{t}, {kernel}, 256>::reduce_{op}("
                    f"reg<{t}, {kernel}, 256>::{op}(_mm512_castsi512_si256(a), _mm512_extracti64x4_epi64(a, 1)));")
        out[f"reduce_{op}"] = Impl(SHORT, body)

    for target in CONVERSIONS.get(t, []):
        to = ELEM[target]
        if b == 32 and signed:
            out[f"to_{target}"] = _ret(f"{p}_cvtepi32_ps(a)")
        elif b == 32 and k:
            out[f"to_{target}"] = _ret(f"{p}_cvtepu32_ps(a)")
        elif b == 32:
            # Two exact halves: the high one scaled by 2^16 is exact, so the sum rounds once, as a native conversion would.
            out[f"to_{target}"] = Impl(EMULATED,
                                       f"{_f(to, w)} const lo = {p}_cvtepi32_ps({p}_and_{si}(a, {p}_set1_epi32(0xFFFF)));\n"
                                       f"{_f(to, w)} const hi = {p}_cvtepi32_ps({p}_srli_epi32(a, 16));\n"
                                       f"return {p}_add_ps({p}_mul_ps(hi, {p}_set1_ps(65536.f)), lo);")
        elif k:
            out[f"to_{target}"] = _ret(f"{p}_cvt{'epi' if signed else 'epu'}64_pd(a)")
        else:
            out[f"to_{target}"] = _through_memory(e, to, w, "f64(x[{i}])")
    return out


def ops(kernel: str, e: Elem, w: int) -> dict[str, Impl]:
    return _float_ops(kernel, e, w) if e.is_float else _int_ops(kernel, e, w)


def mask_ops(kernel: str, lane_bits: int, w: int) -> dict[str, Impl]:
    p = _P[w]
    si = _SI[w]
    it = _I[w]
    lanes = w // lane_bits
    full = f"0x{(1 << lanes) - 1:X}ull"
    out: dict[str, Impl] = {}
    if LEVEL[kernel] >= 3:
        mt = mask_type(kernel, lane_bits, w)
        out["bit_and"] = _ret(f"{mt}(a & b)")
        out["bit_or"] = _ret(f"{mt}(a | b)")
        out["bit_xor"] = _ret(f"{mt}(a ^ b)")
        out["bit_not"] = _ret(f"{mt}(~u64(a) & {full})")
        out["bits"] = _ret("u64(m)")
        out["any"] = _ret("m != 0")
        out["all"] = _ret(f"(u64(m) & {full}) == {full}")
        out["from_bits"] = _ret(f"{mt}(b & {full})")
        return out

    out["bit_and"] = _ret(f"{p}_and_{si}(a, b)")
    out["bit_or"] = _ret(f"{p}_or_{si}(a, b)")
    out["bit_xor"] = _ret(f"{p}_xor_{si}(a, b)")
    out["bit_not"] = _ret(f"{p}_xor_{si}(a, {p}_set1_epi32(-1))")
    if lane_bits == 8:
        out["bits"] = _ret(f"u64(u32({p}_movemask_epi8(m)))")
    elif lane_bits == 16 and w == 128:
        out["bits"] = Impl(SHORT, "return u64(u32(_mm_movemask_epi8(_mm_packs_epi16(m, _mm_setzero_si128()))));")
    elif lane_bits == 16:
        out["bits"] = Impl(SHORT, f"return mreg<16, {kernel}, 128>::bits(_mm256_castsi256_si128(m))\n"
                                  f"     | (mreg<16, {kernel}, 128>::bits(_mm256_extracti128_si256(m, 1)) << 8);")
    elif lane_bits == 32:
        out["bits"] = _ret(f"u64(u32({p}_movemask_ps({p}_castsi{w}_ps(m))))")
    else:
        out["bits"] = _ret(f"u64(u32({p}_movemask_pd({p}_castsi{w}_pd(m))))")
    out["any"] = _ret("bits(m) != 0")
    out["all"] = _ret(f"bits(m) == {full}")
    # Every lane all-ones where its bit is set, written out lane by lane: from_bits is never on a hot path.
    lt = f"i{lane_bits}"
    body = [f"alignas({w // 8}) {lt} x[{lanes}];"]
    body += [f"x[{i}] = ((b >> {i}) & 1u) != 0 ? {lt}(-1) : {lt}(0);" for i in range(lanes)]
    body += [f"return {p}_load_{si}(reinterpret_cast<{it} const*>(x));"]
    out["from_bits"] = Impl(EMULATED, "\n".join(body))
    return out
