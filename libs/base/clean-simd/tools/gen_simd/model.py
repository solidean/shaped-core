"""What is generated: element types, kernels, and the signature of every register-layer operation.

A kernel module (x86, arm, wasm, scalar) answers one question per (element, register width): the body of each
operation, and its cost class.
The fixed layer is kernel-agnostic and lives in emit.py; it only ever calls the operations named here.
"""

from __future__ import annotations

from dataclasses import dataclass

# Cost classes, which decide where an operator exists (see `has_operator`).
SINGLE = "single"  # one instruction
SHORT = "short"  # at most three lane-wise instructions: a sign flip and a compare, a shift and a mask
EMULATED = "emulated"  # anything longer: widening, a multi-step sequence, a per-lane loop


@dataclass(frozen=True)
class Elem:
    name: str  # the cimd spelling, also the C++ type in namespace cimd
    bits: int
    kind: str  # "float", "signed", "unsigned"

    @property
    def is_float(self) -> bool:
        return self.kind == "float"

    @property
    def is_signed(self) -> bool:
        return self.kind == "signed"


ELEMS = [Elem("f32", 32, "float"), Elem("i32", 32, "signed"), Elem("u32", 32, "unsigned")]
ELEM = {e.name: e for e in ELEMS}
LANE_BITS = sorted({e.bits for e in ELEMS})


@dataclass(frozen=True)
class Kernel:
    name: str
    guard: str  # the CIMD_HAS_* macro its code is compiled under; empty for scalar
    widths: tuple[int, ...]  # register widths it has, in bits, ascending


KERNELS = [
    Kernel("scalar", "", (128,)),
    Kernel("sse2", "CIMD_HAS_SSE2", (128,)),
    Kernel("sse42", "CIMD_HAS_SSE42", (128,)),
    Kernel("avx2", "CIMD_HAS_AVX2", (128, 256)),
    Kernel("avx512", "CIMD_HAS_AVX512", (128, 256, 512)),
    Kernel("neon", "CIMD_HAS_NEON", (128,)),
    Kernel("simd128", "CIMD_HAS_SIMD128", (128,)),
]
KERNEL = {k.name: k for k in KERNELS}

# The kernels an operator is judged on, each at its widest register: an operator exists only where every one of them
# implements the operation as SINGLE or SHORT.
REFERENCE = [("avx2", 256), ("neon", 128), ("simd128", 128)]

# The register counts the fixed layer is generated flat for.
REGISTER_COUNTS = (1, 2, 4, 8)


@dataclass(frozen=True)
class Impl:
    cost: str
    body: str  # C++ statements, ending in a return where the signature returns


# Register-layer signatures: name -> (return, parameters), in terms of
#   E (the element), type (the register), mtype (the mask register), and to:<elem> (another element's register).
SIGNATURES: dict[str, tuple[str, str]] = {
    "broadcast": ("type", "E x"),
    "zero": ("type", ""),
    "iota": ("type", "E start"),
    "load": ("type", "E const* p"),
    "load_aligned": ("type", "E const* p"),
    "store": ("void", "E* p, type a"),
    "store_aligned": ("void", "E* p, type a"),
    "add": ("type", "type a, type b"),
    "sub": ("type", "type a, type b"),
    "mul": ("type", "type a, type b"),
    "min": ("type", "type a, type b"),
    "max": ("type", "type a, type b"),
    "neg": ("type", "type a"),
    "abs": ("type", "type a"),
    "mul_add": ("type", "type a, type b, type c"),
    "bit_and": ("type", "type a, type b"),
    "bit_or": ("type", "type a, type b"),
    "bit_xor": ("type", "type a, type b"),
    "bit_not": ("type", "type a"),
    "eq": ("mtype", "type a, type b"),
    "ne": ("mtype", "type a, type b"),
    "lt": ("mtype", "type a, type b"),
    "le": ("mtype", "type a, type b"),
    "gt": ("mtype", "type a, type b"),
    "ge": ("mtype", "type a, type b"),
    "select": ("type", "mtype m, type a, type b"),
    "reduce_add": ("E", "type a"),
    "reduce_min": ("E", "type a"),
    "reduce_max": ("E", "type a"),
    "to_i32": ("to:i32", "type a"),
    "to_f32": ("to:f32", "type a"),
}

MASK_SIGNATURES: dict[str, tuple[str, str]] = {
    "bit_and": ("type", "type a, type b"),
    "bit_or": ("type", "type a, type b"),
    "bit_xor": ("type", "type a, type b"),
    "bit_not": ("type", "type a"),
    "bits": ("u32", "type m"),
    "any": ("bool", "type m"),
    "all": ("bool", "type m"),
    "from_bits": ("type", "u32 b"),
}

COMMON_OPS = [
    "broadcast", "zero", "iota", "load", "load_aligned", "store", "store_aligned",
    "add", "sub", "mul", "min", "max", "mul_add",
    "eq", "ne", "lt", "le", "gt", "ge", "select",
    "reduce_add", "reduce_min", "reduce_max",
]  # fmt: skip


def ops_of(e: Elem) -> list[str]:
    """Every register operation element `e` has; every kernel implements exactly these."""
    out = list(COMMON_OPS)
    if e.kind != "unsigned":
        out += ["neg", "abs"]
    if not e.is_float:
        out += ["bit_and", "bit_or", "bit_xor", "bit_not"]
    out += ["to_i32"] if e.is_float else ["to_f32"]
    return out


# Which operations an operator spells, and how.
BINARY_OPERATORS = {"add": "+", "sub": "-", "mul": "*", "bit_and": "&", "bit_or": "|", "bit_xor": "^"}
UNARY_OPERATORS = {"neg": "-", "bit_not": "~"}
COMPARE_OPERATORS = {"eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}
