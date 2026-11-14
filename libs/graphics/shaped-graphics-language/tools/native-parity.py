#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""SGL's own SPIR-V and DXIL against the same SGL's text through DXC: size and opcodes, entry point by entry point.

    uv run libs/graphics/shaped-graphics-language/tools/native-parity.py table [filter]
    uv run libs/graphics/shaped-graphics-language/tools/native-parity.py diff <file.sgl> <entry> <cs|vs|ps|lib>
    uv run libs/graphics/shaped-graphics-language/tools/native-parity.py lines <file.sgl> <entry> <cs|vs|ps|lib>
    uv run libs/graphics/shaped-graphics-language/tools/native-parity.py driver [filter]

`table` prints libs/graphics/shaped-graphics-language/docs/native-parity.md's size table over the repo's real shaders,
and leaves each entry's binaries in a folder of --out per file, which shader-analysis' calibrate-cost.py reads.
`diff` prints one entry's opcode histogram difference, and leaves both binaries and their disassemblies in --out.
`lines` counts one entry's DXIL arithmetic per `.sgl` file and line on both routes, and prints where they differ most.
`driver` prints what the vulkan driver made of every compute entry on both SPIR-V routes, from its own statistics:
the machine instructions and blocks on Intel, the registers on AMD, and the scratch either spills to.
It runs `pipeline-stats` (`uv run dev.py build -t pipeline-stats`) on the GPU `SC_GPU` picks, one process per file.
`check` counts SGL's own operations alone and compares them with native-size-baseline.json beside this script:
an entry that grew more than 2% and more than 4 operations fails, and so does one the baseline does not name.
`update` writes the baseline from what SGL writes now, which a change that shrinks or knowingly grows an entry commits.
`dev.py check`'s sgl-native-size gate runs `check`.
It needs sgl built (`uv run dev.py build -t sgl`) and DXC fetched; it runs on Windows, where DXC's validator is.
DXC gets slib's own arguments, so its side is what a context of the text route builds.
"""

import argparse
import json
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
    # generated at run time; a viewer test keeps this copy what the generator writes
    "libs/graphics/shaped-viewer/tests/data/pbr_shade_kernel.sgl",
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


ARITHMETIC = {"fmul", "fadd", "fsub", "fdiv", "select", "fcmp", "dx.dot2", "dx.dot3", "dx.dot4", "dx.unary", "dx.binary",
              "dx.tertiary"}


def lines_of_listing(listing, line_map=None):
    """{(file, line): arithmetic operations} of a DXIL listing with debug locations.

    A location is read at its own line, not the call it was inlined through.
    `line_map` turns a listing's line into the `.sgl` place it stands for, where the listing names one HLSL file.
    """
    md = {m.group(1): m.group(2) for m in re.finditer(r"^!(\d+) = (?:distinct )?!\w+\((.*)\)$", listing, re.M)}

    def file_of(scope):
        for _ in range(64):
            body = md.get(scope, "")
            f = re.search(r"file: !(\d+)", body)
            if f:
                name = re.search(r'filename: "([^"]*)"', md.get(f.group(1), ""))
                return name.group(1).replace("\\5C", "/").replace("\\", "/").split("/")[-1] if name else "?"
            s = re.search(r"scope: !(\d+)", body)
            if not s:
                return "?"
            scope = s.group(1)
        return "?"

    counts = Counter()
    for text in listing.splitlines():
        at = re.search(r"!dbg !(\d+)", text)
        op = re.match(r"\s*(?:%[\w.]+ = )?(\w+)", text)
        if not at or not op:
            continue
        dx = re.search(r"@dx\.op\.(\w+)", text)
        name = "dx." + dx.group(1) if dx else op.group(1)
        if name not in ARITHMETIC:
            continue
        body = md.get(at.group(1), "")
        line = re.search(r"line: (\d+)", body)
        scope = re.search(r"scope: !(\d+)", body)
        place = (file_of(scope.group(1)) if scope else "?", int(line.group(1)) if line else 0)
        if line_map is not None:
            place = line_map[place[1] - 1] if 1 <= place[1] <= len(line_map) else ("?", place[1])
        counts[place] += 1
    return counts


def per_line(sgl, file, entry, stage, out):
    """Both routes' DXIL arithmetic per `.sgl` line: native's line table, and DXC compiled with `-Zi`.

    DXC keeps one file name for every `#line`, so its side compiles the HLSL stripped of them, and each line maps back.
    """
    base = out / ("%s-%s" % (Path(file).stem, entry))
    modules = [a for d in MODULES for a in ("--module-dir", ROOT / d)]
    emit = [sgl, "emit", file, "--entry", entry] + modules
    native = base.with_suffix(".lines.native.dxil")
    run(emit + ["--target", "dxil", "--dxil", native, "--native-line-table"])
    hlsl, line_map, place = [], [], ("?", 0)
    for text in run(emit + ["--target", "hlsl-dx12", "--line-directives"]).splitlines():
        m = re.match(r'#line (\d+) "([^"]*)"', text)
        if m:
            place = (m.group(2).replace("\\", "/").split("/")[-1], int(m.group(1)))
            continue
        hlsl.append(text)
        line_map.append(place)
        place = (place[0], place[1] + 1)
    plain = base.with_suffix(".lines.hlsl")
    plain.write_bytes("\n".join(hlsl).encode("utf-8"))
    profile = {"cs": "cs_6_8", "vs": "vs_6_8", "ps": "ps_6_8", "lib": "lib_6_8"}[stage]
    dxc = base.with_suffix(".lines.dxc.dxil")
    run([DXC, plain, "-E", entry, "-T", profile, "-O3", "-enable-16bit-types", "-Zi", "-Qembed_debug", "-Fo", dxc])
    return lines_of_listing(run([DXC, "-dumpbin", native])), lines_of_listing(run([DXC, "-dumpbin", dxc]), line_map)


BASELINE = Path(__file__).resolve().parent / "native-size-baseline.json"
GROWTH = 1.02
SLACK = 4


def native_counts(sgl, file, entry, stage, out, names, with_dxil):
    """{target: operations} of SGL's own binaries for one entry point."""
    base = out / ("%s-%s" % (Path(file).stem, entry))
    modules = [a for d in MODULES for a in ("--module-dir", ROOT / d)]
    emit = [sgl, "emit", file, "--entry", entry] + modules
    spv = base.with_suffix(".native.spv")
    run(emit + ["--target", "spirv", "--spirv", spv])
    counts = {"spirv": sum(spirv_histogram(spv, names).values())}
    if with_dxil:
        dxil = base.with_suffix(".native.dxil")
        run(emit + ["--target", "dxil", "--dxil", dxil])
        counts["dxil"] = sum(dxil_histogram(run([DXC, "-dumpbin", dxil])).values())
    return counts


def all_native_counts(sgl, out, names, with_dxil):
    from concurrent.futures import ThreadPoolExecutor

    jobs = []
    for rel in FILES:
        file = ROOT / rel
        for m in ENTRY.finditer(file.read_text(encoding="utf-8")):
            jobs.append(("%s::%s" % (rel, m.group(2)), file, m.group(2), STAGES.get(m.group(1), "lib")))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        futures = {key: pool.submit(native_counts, sgl, file, entry, stage, out, names, with_dxil)
                   for key, file, entry, stage in jobs}
    return {key: f.result() for key, f in futures.items()}


def check(sgl, out, names):
    with_dxil = DXC.exists()
    counts = all_native_counts(sgl, out, names, with_dxil)
    baseline = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
    failures, shrunk = [], []
    for key in sorted(set(counts) | set(baseline)):
        if key not in baseline:
            failures.append("%s: not in the baseline" % key)
            continue
        if key not in counts:
            failures.append("%s: in the baseline, but no such entry point" % key)
            continue
        for target, ops in counts[key].items():
            was = baseline[key].get(target)
            if was is None:
                failures.append("%s %s: not in the baseline" % (key, target))
            elif ops > was * GROWTH and ops > was + SLACK:
                failures.append("%s %s: %d operations, the baseline's %d (%+.1f%%)" % (key, target, ops, was, 100.0 * (ops - was) / was))
            elif ops < was / GROWTH and ops < was - SLACK:
                shrunk.append("%s %s: %d operations, the baseline's %d" % (key, target, ops, was))
    for line in shrunk:
        print("shrunk: " + line)
    if shrunk:
        print("`native-parity.py update` keeps what shrank from growing back")
    if not with_dxil:
        print("DXC is not fetched, so DXIL went uncounted")
    for line in failures:
        print("FAILED: " + line)
    if failures:
        print("a growth that is meant is committed with `uv run %s update`" % Path(__file__).relative_to(ROOT).as_posix())
        return 1
    print("%d entry points within %.0f%% of the baseline" % (len(counts), 100 * (GROWTH - 1)))
    return 0


def update(sgl, out, names):
    if not DXC.exists():
        sys.exit("update needs DXC, so the baseline holds DXIL too")
    counts = all_native_counts(sgl, out, names, True)
    BASELINE.write_bytes((json.dumps(dict(sorted(counts.items())), indent=1) + "\n").encode("utf-8"))
    print("wrote %d entry points to %s" % (len(counts), BASELINE.relative_to(ROOT).as_posix()))
    return 0


# what a driver reports that says what its code costs; Intel's and AMD's names
KEY_FIGURES = ("Instruction Count", "Basic Block Count", "Loop Count", "Scratch Memory Size", "Spill", "numUsedVgprs",
               "numUsedSgprs", "scratchMemUsageInBytes")


def driver(tool, only):
    print("| file | entry | figure | native | DXC | ratio |")
    print("|---|---|---|---:|---:|---:|")
    modules = []
    for m in MODULES:
        modules += ["--module-dir", str(ROOT / m)]
    for rel in FILES:
        file = ROOT / rel
        entries = [m.group(2) for m in ENTRY.finditer(file.read_text(encoding="utf-8")) if m.group(1) == "compute"]
        entries = [e for e in entries if not only or only in e or only in rel]
        if not entries:
            continue
        cmd = [str(tool), "compute", str(file)] + [x for e in entries for x in ("--entry", e)] + modules
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        for line in p.stdout.splitlines():
            parts = line.split("	")
            if len(parts) != 5 or not any(k in parts[2] for k in KEY_FIGURES):
                continue
            entry, executable, name, native, dxc = parts
            n, d = float(native), (float(dxc) if dxc != "-" else 0.0)
            print("| %s | %s | %s | %g | %s | %s |" % (file.name, entry, name, n, dxc, "%.2f" % (n / d) if d else "-"))
        for line in (p.stdout + p.stderr).splitlines():
            if " error " in line and "nexus" in line:
                print("| %s | | failed: %s |" % (file.name, line.split("error:")[-1].strip()[:120]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["table", "diff", "lines", "check", "update", "driver"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--build-dir", default="build/x64-windows-clang-ninja-relwithdebinfo")
    ap.add_argument("--out", default=None, help="where the binaries and listings go; a temporary folder by default")
    ap.add_argument("--sgl", default=None, help="the sgl tool to run; the --build-dir one by default")
    a = ap.parse_args()
    sgl = Path(a.sgl) if a.sgl else ROOT / a.build_dir / "libs/graphics/shaped-graphics-language/sgl.exe"
    if a.mode == "driver":
        tool = ROOT / a.build_dir / "libs/graphics/shaped-shader-library/pipeline-stats.exe"
        if not tool.exists():
            sys.exit("needs %s (uv run dev.py build -t pipeline-stats)" % tool)
        driver(tool, a.args[0] if a.args else None)
        return
    if a.mode in ("check", "update"):
        if not sgl.exists():
            sys.exit("needs %s (uv run dev.py build -t sgl)" % sgl)
        out = Path(a.out) if a.out else Path(os.environ.get("TEMP", "/tmp")) / "sgl-native-size"
        out.mkdir(parents=True, exist_ok=True)
        sys.exit(check(sgl, out, spirv_opcodes()) if a.mode == "check" else update(sgl, out, spirv_opcodes()))
    if not sgl.exists() or not DXC.exists():
        sys.exit("needs %s (uv run dev.py build -t sgl) and %s (fetched by a configure)" % (sgl, DXC))
    out = Path(a.out) if a.out else Path(os.environ.get("TEMP", "/tmp")) / "sgl-native-parity"
    out.mkdir(parents=True, exist_ok=True)
    names = spirv_opcodes()

    if a.mode == "lines":
        if len(a.args) != 3:
            sys.exit("lines takes <file.sgl> <entry> <cs|vs|ps|lib>")
        native, dxc = per_line(sgl, Path(a.args[0]).resolve(), a.args[1], a.args[2], out)
        print("%s dxil arithmetic: native %d, DXC %d" % (a.args[1], sum(native.values()), sum(dxc.values())))
        files_native, files_dxc = Counter(), Counter()
        for (f, _), n in native.items():
            files_native[f] += n
        for (f, _), n in dxc.items():
            files_dxc[f] += n
        for f in sorted(set(files_native) | set(files_dxc), key=lambda f: -(files_native[f] - files_dxc[f]))[:12]:
            print("   %-36s native %6d  DXC %6d  %+d" % (f, files_native[f], files_dxc[f], files_native[f] - files_dxc[f]))
        print()
        for k in sorted(set(native) | set(dxc), key=lambda k: -(native[k] - dxc[k]))[:25]:
            print("   %-36s %5d  native %5d  DXC %5d  %+d" % (k[0], k[1], native[k], dxc[k], native[k] - dxc[k]))
        return

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
                # a folder per file, since two files may name an entry alike
                (out / file.stem).mkdir(exist_ok=True)
                result = compare(sgl, file, entry, stage, out / file.stem, names)
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
