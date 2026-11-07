#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Drive the AMD DXIL-without-llvm.ident reproduction.

Standalone: no shaped-core imports, no dev.py.
It builds repro.cc with clang-cl and compiles shader.hlsl with dxc, the newest Windows SDK's unless --dxc names one.
That container is the control; the variant is the same module with its `!llvm.ident` named metadata deleted.
The deletion rewrites the bitcode record by record and renumbers nothing, since named metadata takes no number.
The variant carries the unsigned-container digest, which the runtime takes from a container no validator signed.
Each container is compiled into a pipeline in a process of its own, on the first AMD adapter unless --vendor names another.
Windows only, since the bug is D3D12's.

    uv run run.py [--dxc <dxc.exe>] [--vendor <hex PCI id>]

Exits 1 when the variant crashes the driver and the control compiles, 0 when both compile, 2 when inconclusive.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
SOURCE = HERE / "repro.cc"
SHADER = HERE / "shader.hlsl"


# ---- an LLVM bitstream reader and an unabbreviated writer, enough to delete one named metadata node

class Bits:
    def __init__(self, data: bytes):
        self.data, self.pos = data, 0

    def read(self, n: int) -> int:
        v = 0
        for i in range(n):
            v |= ((self.data[self.pos >> 3] >> (self.pos & 7)) & 1) << i
            self.pos += 1
        return v

    def vbr(self, n: int) -> int:
        v, shift, hi = 0, 0, 1 << (n - 1)
        while True:
            piece = self.read(n)
            v |= (piece & (hi - 1)) << shift
            shift += n - 1
            if not piece & hi:
                return v

    def align32(self) -> None:
        self.pos = (self.pos + 31) & ~31


CHAR6 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"
blockinfo: dict[int, list] = {}


def read_abbrev(b: Bits) -> list:
    ops = []
    for _ in range(b.vbr(5)):
        if b.read(1):
            ops.append(("lit", b.vbr(8)))
        else:
            enc = b.read(3)
            ops.append((("fixed", "vbr")[enc - 1], b.vbr(5)) if enc in (1, 2) else ({3: "array", 4: "char6"}[enc], None))
    return ops


def scalar(b: Bits, op) -> int:
    kind, v = op
    return v if kind == "lit" else b.read(v) if kind == "fixed" else b.vbr(v) if kind == "vbr" else ord(CHAR6[b.read(6)])


def abbreviated(b: Bits, abbrev: list) -> list[int]:
    vals, i = [], 0
    while i < len(abbrev):
        if abbrev[i][0] == "array":
            vals += [scalar(b, abbrev[i + 1]) for _ in range(b.vbr(6))]
            i += 2
        else:
            vals.append(scalar(b, abbrev[i]))
            i += 1
    return vals


def parse_block(b: Bits, width: int, block_id: int) -> list:
    abbrevs, items, current = list(blockinfo.get(block_id, [])), [], None
    while True:
        aid = b.read(width)
        if aid == 0:
            b.align32()
            return items
        if aid == 1:
            sub, w = b.vbr(8), b.vbr(4)
            b.align32()
            b.read(32)
            items.append(("block", sub, w, parse_block(b, w, sub)))
        elif aid == 2:
            abbrev = read_abbrev(b)
            (blockinfo.setdefault(current, []) if block_id == 0 else abbrevs).append(abbrev)
        else:
            if aid == 3:
                code, n = b.vbr(6), b.vbr(6)
                ops = [b.vbr(6) for _ in range(n)]
            else:
                vals = abbreviated(b, abbrevs[aid - 4])
                code, ops = vals[0], vals[1:]
            if block_id == 0 and code == 1:
                current = ops[0]
            items.append(("record", code, ops))


class Writer:
    def __init__(self):
        self.bits: list[int] = []

    def write(self, v: int, n: int) -> None:
        self.bits += [(v >> i) & 1 for i in range(n)]

    def vbr(self, v: int, n: int) -> None:
        hi = 1 << (n - 1)
        while True:
            piece, v = v & (hi - 1), v >> (n - 1)
            self.write(piece | hi if v else piece, n)
            if not v:
                return

    def align32(self) -> None:
        self.bits += [0] * (-len(self.bits) % 32)

    def emit_block(self, block_id: int, width: int, items: list, outer: int) -> None:
        self.write(1, outer)
        self.vbr(block_id, 8)
        self.vbr(width, 4)
        self.align32()
        at = len(self.bits)
        self.write(0, 32)
        start = len(self.bits)
        for it in items:
            if it[0] == "block":
                self.emit_block(it[1], it[2], it[3], width)
            else:
                self.write(3, width)
                self.vbr(it[1], 6)
                self.vbr(len(it[2]), 6)
                for o in it[2]:
                    self.vbr(o, 6)
        self.write(0, width)
        self.align32()
        words = (len(self.bits) - start) // 32
        self.bits[at:at + 32] = [(words >> i) & 1 for i in range(32)]

    def to_bytes(self) -> bytes:
        self.align32()
        out = bytearray(len(self.bits) // 8)
        for i, bit in enumerate(self.bits):
            out[i >> 3] |= bit << (i & 7)
        return bytes(out)


def without_named_metadata(container: bytes, name: str) -> bytes:
    """The container with one named metadata node deleted from its DXIL, unsigned."""
    count = struct.unpack_from("<I", container, 28)[0]
    parts = []
    for off in struct.unpack_from(f"<{count}I", container, 32):
        size = struct.unpack_from("<I", container, off + 4)[0]
        parts.append((container[off:off + 4], container[off + 8:off + 8 + size]))
    dxil = dict(parts)[b"DXIL"]
    program_version, _, magic, dxil_version, bc_offset, bc_size = struct.unpack_from("<II4sIII", dxil, 0)
    b = Bits(dxil[8 + bc_offset:8 + bc_offset + bc_size])
    assert b.read(32) == 0xDEC04342 and b.read(2) == 1
    module_id, module_width = b.vbr(8), b.vbr(4)
    b.align32()
    b.read(32)
    module = parse_block(b, module_width, module_id)

    target, removed = [ord(c) for c in name], False
    for it in module:
        if it[0] == "block" and it[1] == 15:  # METADATA
            kept, skip = [], False
            for r in it[3]:
                if skip:  # the NAMED_NODE record that follows its NAME
                    skip = False
                    continue
                if r[0] == "record" and r[1] == 4 and r[2] == target:
                    skip = removed = True
                    continue
                kept.append(r)
            it[3][:] = kept
    if not removed:
        raise SystemExit(f"the module has no !{name}")

    w = Writer()
    w.write(0xDEC04342, 32)
    w.emit_block(module_id, module_width, module, 2)
    bitcode = w.to_bytes()
    new_dxil = struct.pack("<II4sIII", program_version, (24 + len(bitcode)) // 4, magic, dxil_version, 16, len(bitcode)) + bitcode
    # the hash would describe the old module, and STAT is reflection nothing here reads
    parts = [(f, new_dxil if f == b"DXIL" else blob) for f, blob in parts if f not in (b"HASH", b"STAT")]
    header = 32 + 4 * len(parts)
    offsets, body = [], b""
    for f, blob in parts:
        offsets.append(header + len(body))
        body += f + struct.pack("<I", len(blob)) + blob
    bypass_digest = bytes([1] * 16)
    return (b"DXBC" + bypass_digest + struct.pack("<HHII", 1, 0, header + len(body), len(parts))
            + struct.pack(f"<{len(parts)}I", *offsets) + body)


# ---- the run

def find_dxc() -> Path | None:
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: tuple(int(x) for x in p.parent.parent.name.split(".")))
    return candidates[-1] if candidates else None


def build(workdir: Path) -> Path | None:
    exe = workdir / "repro.exe"
    if not shutil.which("clang-cl"):
        print("no clang-cl on PATH", file=sys.stderr)
        return None
    cmd = ["clang-cl", "/O2", "/std:c++20", "/EHsc", "/MD", "/nologo", str(SOURCE), f"/Fe:{exe}", f"/Fo:{workdir / 'repro.obj'}"]
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if out.returncode != 0 or not exe.is_file():
        print(out.stdout, out.stderr, file=sys.stderr)
        return None
    return exe


def create_pipeline(exe: Path, container: Path, vendor: str) -> tuple[str, str]:
    try:
        out = subprocess.run([str(exe), str(container), "--vendor", vendor], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    code = out.returncode & 0xFFFFFFFF
    verdict = {0: "compiled", 2: "no adapter", 3: "refused"}.get(code, f"crashed (exit {code:#x})")
    return verdict, out.stdout.strip().splitlines()[0] if out.stdout.strip() else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--vendor", default="1002")
    args = parser.parse_args()
    if sys.platform != "win32":
        print("D3D12 runs on Windows alone")
        return 2
    dxc = args.dxc or find_dxc()
    if dxc is None:
        print("no Windows SDK dxc found; pass --dxc")
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        exe = build(workdir)
        if exe is None:
            return 2
        control = workdir / "with-ident.dxil"
        out = subprocess.run([str(dxc), "-nologo", "-T", "cs_6_6", "-E", "cs", "-Fo", str(control), str(SHADER)],
                             capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stdout, out.stderr, file=sys.stderr)
            return 2
        variant = workdir / "without-ident.dxil"
        variant.write_bytes(without_named_metadata(control.read_bytes(), "llvm.ident"))

        results = {}
        for label, path in (("with !llvm.ident", control), ("without !llvm.ident", variant)):
            verdict, adapter = create_pipeline(exe, path, args.vendor)
            results[label] = verdict
            print(f"{label:20} {verdict:24} {adapter}")
    if "no adapter" in results.values() or "timeout" in results.values():
        print("inconclusive: no such adapter, or a run did not finish")
        return 2
    if results["with !llvm.ident"] == "compiled" and results["without !llvm.ident"].startswith("crashed"):
        print("REPRODUCED: the driver crashes compiling a module that has no !llvm.ident")
        return 1
    print("not reproduced: both modules compile" if all(v == "compiled" for v in results.values()) else "inconclusive")
    return 0 if all(v == "compiled" for v in results.values()) else 2


if __name__ == "__main__":
    sys.exit(main())
