#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""SGL's own SPIR-V and DXIL against the same SGL's text through DXC: size and opcodes, entry point by entry point.

    uv run libs/graphics/shaped-graphics-language/tools/native-parity.py table [filter]
    uv run libs/graphics/shaped-graphics-language/tools/native-parity.py diff <file.sgl> <entry> <cs|vs|ps|lib>

`table` prints docs/native-parity.md's size table over the repo's real shaders.
`diff` prints one entry's opcode histogram difference, and leaves both binaries and their disassemblies in --out.
It needs sgl built (`uv run dev.py build -t sgl`) and DXC fetched; it runs on Windows, where DXC's validator is.
DXC gets slib's own arguments, so its side is what a context of the text route builds.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
DXC = ROOT / "extern/dxc/.install/bin/dxc.exe"
SPIRV_HEADER = ROOT / "extern/spirv-reflect/include/spirv/unified1/spirv.h"
DXC_SPIRV = ["-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-dx-layout", "-fvk-support-nonzero-base-vertex",
             "-fvk-support-nonzero-base-instance", "-fvk-use-dx-position-w"]
MODULES = ["libs/graphics/shaped-viewer/shaders/sgl", "libs/graphics/shaped-rendering/shaders/modules"]
FILES = [
    "libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl",
    "libs/graphics/shaped-rendering/shaders/svgf.sgl",
    "libs/graphics/shaped-rendering/shaders/nn_conv.sgl",
    "libs/graphics/shaped-rendering/shaders/nn_pool.sgl",
    "libs/graphics/shaped-rendering/shaders/nn_upsample.sgl",
    "libs/graphics/shaped-rendering/shaders/bloom.sgl",
    "libs/graphics/shaped-rendering/shaders/box_filter_mipmap.sgl",
    "libs/graphics/shaped-rendering/shaders/mix.sgl",
    "libs/graphics/shaped-rendering/shaders/exposure_meter.sgl",
    "libs/graphics/shaped-rendering/shaders/compression/bc.sgl",
    "libs/graphics/shaped-rendering/shaders/imgui.sgl",
    "libs/graphics/shaped-rendering/shaders/slug_quads.sgl",
    "libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl",
    "libs/graphics/shaped-viewer/shaders/wavefront.sgl",
    "libs/graphics/shaped-viewer/shaders/raster_pass.sgl",
    "libs/graphics/shaped-viewer/shaders/raster_cull.sgl",
]
STAGES = {"compute": "cs", "vertex": "vs", "pixel": "ps"}
ENTRY = re.compile(r"^@(compute|vertex|pixel|raygen|closest_hit|any_hit|miss|intersection|callable)\b[^\n]*?\bfun ([a-z_0-9]+)", re.M)
NOT_CODE = {"Name", "MemberName", "String", "Line", "NoLine", "Source", "SourceExtension", "SourceContinued",
            "ModuleProcessed", "Decorate", "MemberDecorate", "DecorateId", "DecorateString", "MemberDecorateString"}


def spirv_opcodes():
    text = SPIRV_HEADER.read_text(encoding="utf-8")
    body = text[text.index("typedef enum SpvOp_"):]
    body = body[: body.index("} SpvOp;")]
    names = {}
    for m in re.finditer(r"SpvOp(\w+) = (\d+),", body):
        names.setdefault(int(m.group(2)), m.group(1))
    return names


def spirv_histogram(path, names):
    data = path.read_bytes()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    counts = Counter()
    in_function = False
    i = 5
    while i < len(words):
        op = words[i] & 0xFFFF
        name = names.get(op, "op%d" % op)
        in_function = in_function or name == "Function"
        if in_function and name not in NOT_CODE:
            counts[name] += 1
        in_function = in_function and name != "FunctionEnd"
        i += max(words[i] >> 16, 1)
    return counts


def dxil_histogram(listing):
    counts = Counter()
    for line in listing.splitlines():
        s = line.strip()
        if not s or s[0] in ";!}@" or " = type " in s or s.startswith(("define", "declare", "attributes", "target ", "%dx.types", "%struct")) or s.endswith(":"):
            continue
        m = re.search(r"@dx\.op\.(\w+)", s)
        if m:
            counts["dx." + m.group(1)] += 1
            continue
        m = re.match(r"(?:%[\w.]+ = )?(\w+)", s)
        if m:
            counts[m.group(1)] += 1
    return counts


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("%s\n%s" % (" ".join(str(c) for c in cmd), (r.stdout + r.stderr)[-2000:]))
    return r.stdout


def compare(sgl, file, entry, stage, out, names):
    """{(target, route): (bytes, histogram)} for one entry point."""
    base = out / entry
    modules = [a for d in MODULES for a in ("--module-dir", ROOT / d)]
    emit = [sgl, "emit", file, "--entry", entry] + modules
    profile = {"cs": "cs_6_8", "vs": "vs_6_8", "ps": "ps_6_8", "lib": "lib_6_8"}[stage]
    common = ["-E", entry, "-T", profile, "-O3", "-enable-16bit-types"]
    result = {}
    run(emit + ["--target", "spirv", "--spirv", base.with_suffix(".native.spv")])
    base.with_suffix(".vk.hlsl").write_text(run(emit + ["--target", "hlsl-vulkan"]))
    run([DXC, base.with_suffix(".vk.hlsl")] + common + DXC_SPIRV + ["-Fo", base.with_suffix(".dxc.spv")])
    for route in ("native", "dxc"):
        spv = base.with_suffix(".%s.spv" % route)
        result[("spirv", route)] = (spv.stat().st_size, spirv_histogram(spv, names))
    run(emit + ["--target", "dxil", "--dxil", base.with_suffix(".native.dxil")])
    base.with_suffix(".dx.hlsl").write_text(run(emit + ["--target", "hlsl-dx12"]))
    run([DXC, base.with_suffix(".dx.hlsl")] + common + ["-Fo", base.with_suffix(".dxc.dxil"), "-Qstrip_reflect", "-Qstrip_debug"])
    for route in ("native", "dxc"):
        dxil = base.with_suffix(".%s.dxil" % route)
        listing = run([DXC, "-dumpbin", dxil])
        base.with_suffix(".%s.ll" % route).write_text(listing)
        result[("dxil", route)] = (dxil.stat().st_size, dxil_histogram(listing))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["table", "diff"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--build-dir", default="build/x64-windows-clang-ninja-relwithdebinfo")
    ap.add_argument("--out", default=None, help="where the binaries and listings go; a temporary folder by default")
    a = ap.parse_args()
    sgl = ROOT / a.build_dir / "libs/graphics/shaped-graphics-language/sgl.exe"
    if not sgl.exists() or not DXC.exists():
        sys.exit("needs %s (uv run dev.py build -t sgl) and %s (fetched by a configure)" % (sgl, DXC))
    out = Path(a.out) if a.out else Path(os.environ.get("TEMP", "/tmp")) / "sgl-native-parity"
    out.mkdir(parents=True, exist_ok=True)
    names = spirv_opcodes()

    if a.mode == "diff":
        if len(a.args) != 3:
            sys.exit("diff takes <file.sgl> <entry> <cs|vs|ps|lib>")
        file, entry, stage = a.args
        result = compare(sgl, Path(file).resolve(), entry, stage, out, names)
        for target in ("spirv", "dxil"):
            (nb, nh), (db, dh) = result[(target, "native")], result[(target, "dxc")]
            print("%s %s: native %d bytes, %d ops; DXC %d bytes, %d ops" % (entry, target, nb, sum(nh.values()), db, sum(dh.values())))
            for k in sorted(set(nh) | set(dh), key=lambda k: -abs(nh[k] - dh[k]))[:25]:
                if nh[k] != dh[k]:
                    print("   %-36s native %6d  DXC %6d  %+d" % (k, nh[k], dh[k], nh[k] - dh[k]))
        print("binaries and listings in", out)
        return

    only = a.args[0] if a.args else None
    print("| file | entry | target | native bytes | DXC bytes | ratio | native ops | DXC ops | ratio |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|")
    for rel in FILES:
        file = ROOT / rel
        for m in ENTRY.finditer(file.read_text(encoding="utf-8")):
            entry, stage = m.group(2), STAGES.get(m.group(1), "lib")
            if only and only not in entry and only not in rel:
                continue
            try:
                result = compare(sgl, file, entry, stage, out, names)
            except RuntimeError as e:
                print("| %s | %s | failed: %s |" % (file.name, entry, str(e).splitlines()[-1][:100]))
                continue
            for target in ("spirv", "dxil"):
                (nb, nh), (db, dh) = result[(target, "native")], result[(target, "dxc")]
                no, do = sum(nh.values()), sum(dh.values())
                print("| %s | %s | %s | %d | %d | %.2f | %d | %d | %.2f |" % (
                    file.name, entry, target, nb, db, nb / db, no, do, no / do if do else 0.0))


if __name__ == "__main__":
    main()
