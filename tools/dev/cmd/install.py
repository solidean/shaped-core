"""`install` — install a developer tool of this repo into the machine's own tools.

Two kinds.
An editor extension is a link into the checkout, so a `git pull` or a rebuild updates what is installed.
`--uninstall` removes only links into this checkout: never a folder of the user's, and never another checkout's link.
emsdk, the Emscripten SDK the WASM presets build with, is a checkout of its own outside this one, at the version `tools/emsdk.version` pins.
It is a folder rather than a link, so `--uninstall` names it and leaves deleting it to the user.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "install"


@dataclass(frozen=True)
class _editor:
    """A VS Code-family editor, by the folder it loads user extensions from."""

    name: str
    extensions_dir: Path


@dataclass(frozen=True)
class _installable:
    name: str
    description: str
    # the folder the link points at, relative to the repo root
    source: str
    # the link's name inside an editor's extensions folder, `publisher.name` as the extension's package.json says
    link_name: str
    # what has to be built for the install to work
    targets: tuple[str, ...]
    hint: str


_INSTALLABLES = [
    _installable(
        name="sgl-vscode",
        description="the SGL extension for VS Code: grammar, and the client of the language server `sgl lsp`",
        source="libs/graphics/shaped-graphics-language/tools/vscode-extension",
        link_name="shapedcode.sgl",
        targets=("sgl",),
        hint="run Developer: Reload Window, then open an .sgl file; the log is the 'SGL Language Server' output channel",
    ),
]


_EMSDK = "emsdk"
_EMSDK_REPO = "https://github.com/emscripten-core/emsdk.git"


def _editors() -> list[_editor]:
    home = Path.home()
    return [
        _editor("code", home / ".vscode" / "extensions"),
        _editor("insiders", home / ".vscode-insiders" / "extensions"),
        _editor("oss", home / ".vscode-oss" / "extensions"),
        _editor("cursor", home / ".cursor" / "extensions"),
        _editor("server", home / ".vscode-server" / "extensions"),
    ]


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Install a developer tool of this repo (no argument lists them)")
    a.preset(p)
    p.add_argument("name", nargs="?", help="What to install; without it, every installable and where it stands")
    p.add_argument("--editor", action="append", choices=[e.name for e in _editors()],
                   help="Only this editor, repeatable; default: every editor whose extensions folder exists")
    p.add_argument("--uninstall", action="store_true", help="Remove the links instead of making them")
    p.add_argument("--no-build", action="store_true", help="Link only, and build nothing first")
    a.emsdk(p)
    return p


def _is_junction(path: Path) -> bool:
    """A Windows junction, which `Path.is_symlink` does not report; Path.is_junction only exists from Python 3.12."""
    if os.name != "nt":
        return False
    import stat

    try:
        return bool(os.lstat(path).st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT) and not path.is_symlink()
    except OSError:
        return False


def _link_target(link: Path) -> Path | None:
    """Where `link` points when it is a symlink or a junction; None for anything else, a missing path included."""
    if link.is_symlink() or _is_junction(link):
        return Path(os.path.realpath(link))
    return None


def _make_link(link: Path, target: Path) -> None:
    if os.name == "nt":
        # a junction needs no admin rights and no developer mode, which a directory symlink on Windows does
        import _winapi

        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)


def _remove_link(link: Path) -> None:
    # rmdir removes a junction itself and never what it points at; unlink removes a symlink
    if _is_junction(link):
        os.rmdir(link)
    else:
        link.unlink()


def _selected_editors(args: argparse.Namespace) -> list[_editor]:
    editors = _editors()
    if args.editor:
        return [e for e in editors if e.name in args.editor]
    return [e for e in editors if e.extensions_dir.is_dir()]


def _list(ctx: Context) -> None:
    for item in _INSTALLABLES:
        print(f"{console.bold(item.name)}  {item.description}")
        source = (ctx.root / item.source).resolve()
        for editor in _editors():
            if not editor.extensions_dir.is_dir():
                continue
            link = editor.extensions_dir / item.link_name
            target = _link_target(link)
            if target == source:
                state = console.green("installed")
            elif target is not None:
                state = console.yellow(f"linked to another checkout: {target}")
            elif link.exists():
                state = console.yellow("a folder of that name is there, not a link")
            else:
                state = console.dim("not installed")
            print(f"  {editor.name:<9} {state}")
    _list_emsdk(ctx)
    print(console.dim(f"\n  uv run dev.py {NAME} <name> [--editor <e>] [--emsdk-path <dir>] [--uninstall]"))


def _emsdk_target(args: argparse.Namespace) -> Path:
    """The emsdk to install or update: the one named, else the one dev.py already uses, else the default place."""
    if args.emsdk_path:
        return Path(args.emsdk_path).expanduser().resolve()
    return dev.find_emsdk_root() or dev.default_emsdk_root()


def _list_emsdk(ctx: Context) -> None:
    pinned = dev.pinned_emsdk_version(ctx.root)
    print(f"{console.bold(_EMSDK)}  the Emscripten SDK the WASM presets build with, pinned to {pinned} in tools/emsdk.version")
    root = dev.find_emsdk_root()
    if root is None:
        print(f"  {console.dim('not installed')}")
        return
    installed = dev.installed_emsdk_version(root)
    if installed == pinned:
        state = console.green(f"{installed} at {root}")
    elif installed is None:
        state = console.yellow(f"no version activated at {root}")
    else:
        state = console.yellow(f"{installed} at {root}, not the pinned {pinned}")
    print(f"  {state}")


def _run_emsdk(emsdk_dir: Path, *command: str) -> bool:
    """emsdk's own script, under the Python dev.py runs in, so it needs no python on PATH; its output streams, since it downloads.

    Every line printed ahead of a child process is flushed, or it lands after the child's own output.
    """
    print(console.dim(f"  emsdk {' '.join(command)}"), flush=True)
    return subprocess.run([sys.executable, str(emsdk_dir / "emsdk.py"), *command], cwd=emsdk_dir).returncode == 0


def _install_emsdk(args: argparse.Namespace, ctx: Context) -> None:
    target = _emsdk_target(args)
    if args.uninstall:
        print(f"  emsdk is a folder rather than a link, so nothing here deletes it; remove {target} to uninstall it")
        return

    pinned = dev.pinned_emsdk_version(ctx.root)
    git = shutil.which("git")
    if git is None:
        ctx.die("emsdk installs from its git repository, and git is not on PATH")

    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        print(console.dim(f"  git clone {_EMSDK_REPO} {target}"), flush=True)
        if subprocess.run([git, "clone", _EMSDK_REPO, str(target)]).returncode != 0:
            ctx.die(f"cloning emsdk into {target} failed")
    elif not (target / "emsdk.py").is_file():
        ctx.die(f"{target} exists and is not an emsdk checkout; pass --emsdk-path to install elsewhere")
    else:
        # emsdk learns which versions exist from its own checkout, so an old one does not know a newer pin.
        print(console.dim(f"  git -C {target} pull --ff-only"), flush=True)
        if subprocess.run([git, "-C", str(target), "pull", "--ff-only"]).returncode != 0:
            print(f"  {console.yellow('warning')}: updating {target} failed; installing with what it knows", file=sys.stderr)

    if not _run_emsdk(target, "install", pinned) or not _run_emsdk(target, "activate", pinned):
        ctx.die(f"emsdk could not install and activate {pinned} in {target}")

    installed = dev.installed_emsdk_version(target)
    if installed != pinned:
        ctx.die(f"emsdk reports {installed} in {target} after activating {pinned}")
    print(f"  {console.green('installed')} Emscripten {pinned} at {target}")

    if dev.find_emsdk_root() == target:
        print(console.dim("  dev.py finds it there by itself; the emscripten-* presets need nothing else"))
    else:
        print(console.dim(f"  dev.py does not look there by itself: pass --emsdk-path {target}, or set SC_EMSDK_PATH to it"))


def run(args: argparse.Namespace, ctx: Context) -> None:
    if not args.name:
        _list(ctx)
        return
    if args.name == _EMSDK:
        _install_emsdk(args, ctx)
        return
    item = next((i for i in _INSTALLABLES if i.name == args.name), None)
    if item is None:
        ctx.die(f"nothing named {args.name!r} to install; `uv run dev.py {NAME}` lists them")
    source = (ctx.root / item.source).resolve()
    editors = _selected_editors(args)
    if not editors:
        ctx.die("no VS Code-family editor found: none of ~/.vscode, ~/.vscode-insiders, ~/.vscode-oss, ~/.cursor, "
                "~/.vscode-server has an extensions folder; pass --editor to create one")

    if args.uninstall:
        for editor in editors:
            link = editor.extensions_dir / item.link_name
            target = _link_target(link)
            if target is None:
                print(f"  {editor.name:<9} {console.dim('not installed' if not link.exists() else 'not a link, left alone')}")
                continue
            if target != source:
                print(f"  {editor.name:<9} {console.dim(f'links to {target}, not this checkout; left alone')}")
                continue
            _remove_link(link)
            print(f"  {editor.name:<9} removed the link to {target}")
        print(console.dim("  reload the editor's window for it to let go of the extension"))
        return

    if not args.no_build and item.targets:
        presets = ctx.resolve_presets(args.preset)
        results = dev.build(presets, list(item.targets), root=ctx.root, auto_configure=True,
                            mirror=args.mirror_output, verbose=args.verbose, emsdk_path=None, keep_going=False)
        if not all(r.ok for r in results):
            ctx.fail_build(results, presets)

    failed = False
    for editor in editors:
        editor.extensions_dir.mkdir(parents=True, exist_ok=True)
        link = editor.extensions_dir / item.link_name
        target = _link_target(link)
        if target == source:
            print(f"  {editor.name:<9} already installed")
            continue
        if target is not None:
            # a link into another checkout, or a stale one: ours to replace, since a link holds nothing
            _remove_link(link)
        elif link.exists():
            print(f"  {editor.name:<9} {console.red('refused')}: {link} is a real folder, not a link; move it away first",
                  file=sys.stderr)
            failed = True
            continue
        _make_link(link, source)
        print(f"  {editor.name:<9} linked {link} -> {source}")
    for editor in editors:
        # a VSIX install is a folder named with its version beside the link, and the editor would load both
        for packaged in sorted(editor.extensions_dir.glob(f"{item.link_name}-*")):
            print(f"  {editor.name:<9} {console.yellow('warning')}: {packaged} is a packaged install of the same extension; "
                  "uninstall it in the editor, or it runs twice", file=sys.stderr)
    print(console.dim(f"  {item.hint}"))
    if failed:
        sys.exit(1)
