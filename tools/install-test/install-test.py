#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Install test: build a library subset with SC_INSTALL, install it, and build a consumer against the installed package.

What it proves, in order:
  - `-DSC_LIBRARIES=shaped-graphics-language` configures and builds that library and its dependencies alone;
  - `cmake --install` lays out the archives, the headers, clean-core/build_config.hh and the shaped-core package;
  - the exported targets carry no compile definition and no compile or link option but the MSVC preprocessor's
    conformance switch, which clean-core's headers need to parse;
  - consumer/, a project that only does find_package(shaped-core) and links shaped-core::shaped-graphics-language,
    builds, links and runs a compile of a compute shader through SGL.

It drives cmake directly rather than through dev.py, since installing and consuming are what it tests.
Kept out of `dev.py test`, which runs the nexus suites.

    uv run tools/install-test/install-test.py [--compiler cl|clang-cl|<path>] [--toolset 14.44] [--config Release] [--keep]

The work tree is build/install-test/<compiler>-<config>/, removed first, and removed again on success unless --keep.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

# The libraries `-DSC_LIBRARIES=shaped-graphics-language` builds, and so installs: nothing else may reach lib/.
EXPECTED_LIBRARIES = ["clean-core", "shaped-graphics-language"]
NOT_BUILT = ["babel-data", "babel-serializer", "typed-geometry", "nexus", "shaped-graphics"]

# The compile option the exported targets may carry: clean-core's headers use __VA_OPT__, which cl parses only with it.
ALLOWED_OPTION = "$<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>"


def run(cmd: list[str], *, env: dict[str, str] | None, cwd: Path | None = None) -> None:
    print("+ " + " ".join(cmd), flush=True)
    result = subprocess.run(cmd, env=env, cwd=cwd)
    if result.returncode != 0:
        sys.exit(f"install-test: failed ({result.returncode}): {' '.join(cmd)}")


def compiler_env(compiler: str, toolset: str | None) -> dict[str, str] | None:
    if platform.system() != "Windows" or compiler != "cl":
        return None
    from tools.dev.lib.core.process import msvc_env

    env = msvc_env(toolset)
    if env is None and shutil.which("cl") is None:
        sys.exit("install-test: no MSVC environment found; pass --toolset, or run from a developer prompt")
    return env


def check_layout(prefix: Path, config: str) -> None:
    lib = prefix / "lib"
    archives = sorted(p.name for p in lib.iterdir() if p.is_file())
    print(f"installed archives: {', '.join(archives)}")
    for name in EXPECTED_LIBRARIES:
        if not any(a.startswith(name + ".") or a.startswith("lib" + name + ".") for a in archives):
            sys.exit(f"install-test: lib/ has no archive of {name}")
    for name in NOT_BUILT:
        if any(a.startswith(name + ".") or a.startswith("lib" + name + ".") for a in archives):
            sys.exit(f"install-test: lib/ has {name}, which SC_LIBRARIES=shaped-graphics-language does not need")

    header = prefix / "include" / "clean-core" / "build_config.hh"
    if not header.is_file():
        sys.exit("install-test: include/clean-core/build_config.hh is missing")
    expected_define = {"Debug": "CC_DEBUG", "Release": "CC_RELEASE", "RelWithDebInfo": "CC_RELWITHDEBINFO"}.get(config)
    if expected_define and f"#define {expected_define} 1" not in header.read_text():
        sys.exit(f"install-test: build_config.hh does not define {expected_define}")
    for path in ["clean-core/common/macros.hh", "shaped-graphics-language/driver/compile_to_text.hh"]:
        if not (prefix / "include" / path).is_file():
            sys.exit(f"install-test: include/{path} is missing")

    package = prefix / "lib" / "cmake" / "shaped-core"
    for name in ["shaped-coreConfig.cmake", "shaped-coreConfigVersion.cmake", "shaped-core-targets.cmake"]:
        if not (package / name).is_file():
            sys.exit(f"install-test: lib/cmake/shaped-core/{name} is missing")
    if not (prefix / "share" / "shaped-core" / "LICENSE").is_file():
        sys.exit("install-test: share/shaped-core/LICENSE is missing")

    targets = (package / "shaped-core-targets.cmake").read_text()
    if "INTERFACE_COMPILE_DEFINITIONS" in targets:
        sys.exit("install-test: an exported target carries compile definitions")
    if "INTERFACE_LINK_OPTIONS" in targets:
        sys.exit("install-test: an exported target carries link options")
    for options in re.findall(r'INTERFACE_COMPILE_OPTIONS "([^"]*)"', targets):
        # The targets file escapes each `$` of a generator expression as `\$`.
        extra = [o for o in options.replace("\\$", "$").split(";") if o != ALLOWED_OPTION]
        if extra:
            sys.exit(f"install-test: an exported target carries compile options: {extra}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default="cl" if platform.system() == "Windows" else "clang++",
                        help="C++ compiler for both builds (default: cl on Windows, clang++ elsewhere)")
    parser.add_argument("--c-compiler", default=None, help="C compiler for the vendored externals (default: follows --compiler)")
    parser.add_argument("--toolset", default=None, help="MSVC toolset for cl, e.g. 14.44 (default: the ambient or latest one)")
    parser.add_argument("--config", default="Release", help="build type of both builds (default: Release)")
    parser.add_argument("--keep", action="store_true", help="keep the work tree after a pass")
    args = parser.parse_args()

    c_compiler = args.c_compiler or {"cl": "cl", "clang-cl": "clang-cl", "clang++": "clang", "g++": "gcc"}.get(
        args.compiler, args.compiler)
    tag = re.sub(r"[^A-Za-z0-9.+-]", "_", Path(args.compiler).stem)
    work = ROOT / "build" / "install-test" / f"{tag}-{args.config}"
    if work.exists():
        shutil.rmtree(work)
    build, prefix, consumer = work / "build", work / "prefix", work / "consumer"
    env = compiler_env(args.compiler, args.toolset)
    common = ["-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
              f"-DCMAKE_CXX_COMPILER={args.compiler}", f"-DCMAKE_C_COMPILER={c_compiler}"]

    vendored = [f"-DSC_USE_VENDORED_{x}=ON" for x in ["MIMALLOC", "XXHASH", "BLAKE3", "ZSTD", "LZ4", "ZLIB"]]
    run(["cmake", "-S", str(ROOT), "-B", str(build), *common,
         "-DSC_LIBRARIES=shaped-graphics-language", "-DSC_INSTALL=ON",
         "-DSC_BUILD_TESTS=OFF", "-DSC_BUILD_TOOLS=OFF", "-DSC_BUILD_EXAMPLES=OFF", *vendored], env=env)
    run(["cmake", "--build", str(build)], env=env)
    run(["cmake", "--install", str(build), "--prefix", str(prefix), "--config", args.config], env=env)
    check_layout(prefix, args.config)

    run(["cmake", "-S", str(ROOT / "tools" / "install-test" / "consumer"), "-B", str(consumer), *common,
         f"-DCMAKE_PREFIX_PATH={prefix}"], env=env)
    run(["cmake", "--build", str(consumer)], env=env)
    exe = consumer / ("consumer.exe" if platform.system() == "Windows" else "consumer")
    run([str(exe)], env=env)

    print("install-test: passed")
    if not args.keep:
        shutil.rmtree(work)


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
