"""`android` — build an APK and run it on a connected device, with no gradle.

`android devices` lists what adb sees, and `android run <target>` builds `<target>-apk`, installs it, starts it and streams its log.
It needs `dev.py install android-sdk`; lib/toolchain/android_sdk.py is where adb comes from.
An APK target is one `sc_add_android_apk` declares, see tools/cmake/AndroidApk.cmake.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.toolchain import android_sdk as sdk

from . import args as a
from .context import Context

NAME = "android"
DEFAULT_PRESET = "android-ndk-arm64-relwithdebinfo"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Build an APK and run it on a connected Android device or headset")
    a.preset(p)
    a.build_overrides(p)
    p.add_argument("--serial", help="The device to use, as `android devices` lists it, where several are connected")
    p.add_argument("--no-build", action="store_true", help="Install what is already built")
    p.add_argument("--no-log", action="store_true", help="Start the app and return, rather than streaming its log")
    p.add_argument("action", choices=["devices", "run"], help="devices: list them; run: build, install, start and stream")
    p.add_argument("target", nargs="?", help="run only: the APK's target, such as android-hello")
    return p


def _adb(ctx: Context) -> Path:
    root = sdk.installed_root()
    if root is None:
        ctx.die("no Android SDK: `uv run dev.py install android-sdk --accept-license` installs one")
    return root / "platform-tools" / ("adb.exe" if sdk.host_name() == "windows" else "adb")


def _devices(adb: Path) -> list[sdk.device]:
    listing = subprocess.run([str(adb), "devices"], capture_output=True, text=True, errors="replace")
    return sdk.parse_devices(listing.stdout)


def _list_devices(adb: Path) -> None:
    devices = _devices(adb)
    if not devices:
        print(console.dim("  no device connected"))
    for d in devices:
        state = console.green(d.state) if d.state == "device" else console.yellow(d.state)
        print(f"  {d.serial}  {state}")


def _run(args: argparse.Namespace, ctx: Context, adb: Path) -> None:
    if args.target is None:
        ctx.die("android run needs a target, such as android-hello")
    if args.preset is None:
        args.preset = [DEFAULT_PRESET]
    preset = ctx.resolve_build_presets(args)[0]

    if not args.no_build:
        results = dev.build([preset], [f"{args.target}-apk"], root=ctx.root, auto_configure=True,
                            mirror=args.mirror_output, verbose=args.verbose)
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])
    apk = preset.build_dir / "apk" / f"{args.target}.apk"
    if not apk.is_file():
        ctx.die(f"no {apk}: is {args.target} an sc_add_android_apk target?")
    package = (preset.build_dir / "apk" / f"{args.target}.package").read_text(encoding="utf-8").strip()
    print(f"  {console.green('built')} {apk}")

    chosen, problem = sdk.choose_device(_devices(adb), args.serial)
    if chosen is None:
        ctx.die(problem)
    device = [str(adb), "-s", chosen.serial]

    # Cleared before the start, so the stream below is this run's.
    subprocess.run([*device, "logcat", "-c"], check=False)
    if subprocess.run([*device, "install", "-r", str(apk)]).returncode != 0:
        ctx.die(f"installing {apk.name} failed; a different signature needs `adb uninstall {package}` first")
    activity = f"{package}/android.app.NativeActivity"
    if subprocess.run([*device, "shell", "am", "start", "-W", "-n", activity]).returncode != 0:
        ctx.die(f"starting {activity} failed")
    if args.no_log:
        return

    pid = subprocess.run([*device, "shell", "pidof", package], capture_output=True, text=True).stdout.strip()
    if not pid:
        ctx.die(f"{package} is not running after starting it; `adb -s {chosen.serial} logcat` says why")
    print(console.dim(f"  streaming {package}'s log, ctrl+c stops it and leaves the app running"), flush=True)
    try:
        subprocess.run([*device, "logcat", f"--pid={pid}"], check=False)
    except KeyboardInterrupt:
        pass


def run(args: argparse.Namespace, ctx: Context) -> None:
    adb = _adb(ctx)
    if args.action == "devices":
        _list_devices(adb)
    else:
        _run(args, ctx, adb)
