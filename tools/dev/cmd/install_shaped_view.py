"""`install shaped-view` — the standalone viewer, as a frozen copy that the model formats open in.

The copy lives in ~/tools/shaped-view rather than in a build directory.
A file association pointing into build/ would break on a failed build, on a reconfigure, and whenever a running viewer locks the binary a rebuild wants to replace.
Running the install again is the update.

The copy is a frozen build (SC_FROZEN_SHADERS): it reads no shader file, and carries the bytecode its common cases need.
That bytecode is recorded by running the frozen binary itself over a small corpus of models, then built in, so the build happens twice.
A shader no recording reached still compiles at run time, from the sources the binary embeds —
libs/graphics/shaped-shader-library/docs/frozen-shaders.md has the mechanism.

The association is per user, under HKCU\\Software\\Classes, so it needs no admin rights.
Windows lets no program make itself the default for an extension: the registration makes shaped-view a candidate,
and choosing it once under "Open with", with "Always" ticked, is the step only the user can take.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

from tools import dev
from tools.dev import console

from .context import Context

NAME = "shaped-view"
DESCRIPTION = "the standalone model viewer, registered for the formats it opens"

# The preset the copy is built with when none is named: optimized and frozen.
_DEFAULT_PRESET = "frozen-clang"

_TARGET = "shaped-view"
# What the recording run opens: one file per common case, so what it compiles is what opening such a file needs.
_CORPUS = Path("libs/graphics/shaped-viewer/tools/shaped-view/corpus")
_PROG_ID = "ShapedCode.ShapedView"
# What sv::asset_loader reads; asset_format_of_uri is the authority.
_EXTENSIONS = (".obj", ".stl", ".gltf", ".glb")


def install_dir() -> Path:
    return Path.home() / "tools" / "shaped-view"


def _exe() -> Path:
    return install_dir() / f"{_TARGET}.exe"


def _installed_commit() -> str | None:
    marker = install_dir() / ".commit"
    return marker.read_text(encoding="utf-8").strip() if marker.is_file() else None


def list_state() -> None:
    print(f"{console.bold(NAME)}  {DESCRIPTION}")
    if platform.system() != "Windows":
        print(f"  {console.dim('Windows only so far')}")
        return
    if not _exe().is_file():
        print(f"  {console.dim('not installed')}")
        return
    commit = _installed_commit()
    print(f"  {console.green(f'installed at {_exe()}')}{console.dim(f', built from {commit}' if commit else '')}")


def _head_commit(root: Path) -> str | None:
    git = shutil.which("git")
    if git is None:
        return None
    result = subprocess.run([git, "-C", str(root), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip() or None


def _record_bundle(artifact: Path, preset: dev.Preset, ctx: Context) -> bool:
    """Runs `artifact` headless over the corpus, writing what it compiled where sc_embed_shader_bundle reads it.

    The recording goes to a staged file first, so a failed run leaves the last good bundle in place.
    """
    bundle = preset.build_dir / "shader-bundles" / f"{_TARGET}.slibbundle"
    staged = bundle.with_name(bundle.name + ".new")
    bundle.parent.mkdir(parents=True, exist_ok=True)
    files = sorted(str(p) for p in (ctx.root / _CORPUS).iterdir() if p.suffix.lower() in _EXTENSIONS)
    env = dict(os.environ,
               SC_SHADER_RECORD=str(staged),
               SC_CAPTURE="1",
               SC_CAPTURE_OUT=str(bundle.parent / "recording.png"),
               SC_CAPTURE_SIZE="640x360")
    print(console.dim(f"  recording the shaders {len(files)} corpus files need"), flush=True)
    result = subprocess.run([str(artifact), *files], env=env, capture_output=True, text=True, timeout=900)
    if result.returncode != 0 or not staged.is_file():
        print(f"  {console.yellow('warning')}: the recording run failed, so the copy compiles every shader at run time:",
              file=sys.stderr)
        print(console.dim((result.stdout + result.stderr).strip()), file=sys.stderr)
        return False
    os.replace(staged, bundle)
    print(console.dim(f"  recorded {bundle.stat().st_size // 1024} KiB of shaders"), flush=True)
    return True


def _copy(artifact: Path, commit: str | None, ctx: Context) -> None:
    """The binary and the libraries staged beside it, swapped in whole so a failed copy leaves the old install working."""
    target = install_dir()
    staged = target.with_name("shaped-view.new")
    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir(parents=True)

    # The output directory is shared with every other binary, so only the viewer and the libraries are taken from it.
    shutil.copy2(artifact, staged / artifact.name)
    for dll in sorted(artifact.parent.glob("*.dll")):
        shutil.copy2(dll, staged / dll.name)
    if commit:
        (staged / ".commit").write_text(commit + "\n", encoding="utf-8")

    if target.exists():
        try:
            shutil.rmtree(target)
        except PermissionError:
            shutil.rmtree(staged)
            ctx.die(f"{target} is in use: close every shaped-view window, then install again")
    os.replace(staged, target)


def _register(exe: Path) -> None:
    import winreg

    command = f'"{exe}" "%1"'
    classes = r"Software\Classes"

    def set_value(path: str, name: str, value: str) -> None:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, f"{classes}\\{path}") as key:
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)

    set_value(_PROG_ID, "", "3D model")
    set_value(f"{_PROG_ID}\\DefaultIcon", "", f'"{exe}",0')
    set_value(f"{_PROG_ID}\\shell\\open\\command", "", command)

    application = f"Applications\\{exe.name}"
    set_value(application, "FriendlyAppName", "shaped-view")
    set_value(f"{application}\\shell\\open\\command", "", command)

    for ext in _EXTENSIONS:
        set_value(f"{application}\\SupportedTypes", ext, "")
        set_value(f"{ext}\\OpenWithProgids", _PROG_ID, "")
        # The per-user default, which is what opens the file wherever no "Always" choice in Explorer overrides it.
        set_value(ext, "", _PROG_ID)

    _notify_association_change()


def _unregister() -> None:
    import winreg

    classes = r"Software\Classes"

    def delete_tree(path: str) -> None:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, f"{classes}\\{path}", 0, winreg.KEY_ALL_ACCESS) as key:
                while True:
                    try:
                        child = winreg.EnumKey(key, 0)
                    except OSError:
                        break
                    delete_tree(f"{path}\\{child}")
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, f"{classes}\\{path}")
        except FileNotFoundError:
            pass

    delete_tree(_PROG_ID)
    delete_tree(f"Applications\\{_TARGET}.exe")
    for ext in _EXTENSIONS:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, f"{classes}\\{ext}", 0, winreg.KEY_ALL_ACCESS) as key:
                # Only a default this install wrote is removed; another program's stays.
                try:
                    if winreg.QueryValueEx(key, "")[0] == _PROG_ID:
                        winreg.DeleteValue(key, "")
                except FileNotFoundError:
                    pass
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, f"{classes}\\{ext}\\OpenWithProgids", 0,
                                winreg.KEY_ALL_ACCESS) as key:
                winreg.DeleteValue(key, _PROG_ID)
        except FileNotFoundError:
            pass

    _notify_association_change()


def _notify_association_change() -> None:
    """Tells Explorer the associations changed, so its icons and "Open with" lists refresh without a sign-out."""
    import ctypes

    shcne_assocchanged = 0x08000000
    ctypes.windll.shell32.SHChangeNotify(shcne_assocchanged, 0, None, None)


def run(args: argparse.Namespace, ctx: Context) -> None:
    if platform.system() != "Windows":
        ctx.die("shaped-view installs on Windows only so far")

    if args.uninstall:
        _unregister()
        print(f"  removed the association of {', '.join(_EXTENSIONS)} with shaped-view")
        if install_dir().exists():
            try:
                shutil.rmtree(install_dir())
            except PermissionError:
                ctx.die(f"{install_dir()} is in use: close every shaped-view window, then uninstall again")
            print(f"  removed {install_dir()}")
        return

    presets = ctx.resolve_presets(args.preset or [_DEFAULT_PRESET])
    preset = presets[0]

    def build() -> Path:
        if not args.no_build:
            results = dev.build([preset], [_TARGET], root=ctx.root, auto_configure=True,
                                mirror=args.mirror_output, verbose=args.verbose, emsdk_path=None, keep_going=False)
            if not all(r.ok for r in results):
                ctx.fail_build(results, [preset])
        targets = ctx.discover(preset)
        built = next((t.artifact for t in targets if t.name == _TARGET and t.artifact), None)
        if built is None or not built.is_file():
            ctx.die(f"{_TARGET} has no built binary for preset {preset.name!r}; its build needs dx12, DXC and SDL3")
        return built

    # Built, recorded with, and built again: the second build only embeds the new recording and relinks.
    artifact = build()
    if not args.no_build and _record_bundle(artifact, preset, ctx):
        artifact = build()

    _copy(artifact, _head_commit(ctx.root), ctx)
    _register(_exe())
    print(f"  {console.green('installed')} {_exe()}, built with {preset.name}")
    print(f"  registered for {', '.join(_EXTENSIONS)}")
    print(console.dim("  if a double-click still opens another program: right-click a file, Open with, Choose another app,"))
    print(console.dim("  pick shaped-view and tick Always - Windows reserves that choice for the user"))
    sys.stdout.flush()
