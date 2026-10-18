"""`install` — install a developer tool of this repo into the machine's own tools.

Two kinds.
An editor extension is a link into the checkout, so a `git pull` or a rebuild updates what is installed.
`--uninstall` removes only links into this checkout: never a folder of the user's, and never another checkout's link.
emsdk, the Emscripten SDK the WASM presets build with, is a checkout of its own outside this one, at the version `tools/emsdk.version` pins.
deno, the second runtime for WASM artifacts, is the release binary at the version `tools/deno.version` pins, in ~/tools/deno.
sde, Intel's Software Development Emulator, runs a binary as an older or newer CPU; its archive is downloaded by hand, since Intel asks to accept a license first.
It is the version and SHA-256 `tools/sde.version` pins, unpacked in ~/tools/sde.
All three are folders rather than links, so `--uninstall` names them and leaves deleting them to the user.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.core.layout import DRIVER_ROOT
from tools.dev.lib.toolchain import jsruntime as jsr

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

_DENO = "deno"
_DENO_RELEASES = "https://github.com/denoland/deno/releases/download"
# deno's release asset per (system, machine), as platform reports them.
_DENO_TARGETS = {
    ("Windows", "AMD64"): "x86_64-pc-windows-msvc",
    ("Windows", "x86_64"): "x86_64-pc-windows-msvc",
    ("Linux", "x86_64"): "x86_64-unknown-linux-gnu",
    ("Linux", "aarch64"): "aarch64-unknown-linux-gnu",
    ("Darwin", "x86_64"): "x86_64-apple-darwin",
    ("Darwin", "arm64"): "aarch64-apple-darwin",
}

_SDE = "sde"
_SDE_PAGE = "https://www.intel.com/content/www/us/en/download/684897/intel-software-development-emulator.html"
# The kit's suffix per system; SDE runs on x86-64 hosts only, and has no macOS kit any more.
_SDE_KITS = {"Windows": "win", "Linux": "lin"}


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
    p.add_argument("--archive", metavar="FILE",
                   help="sde only: the kit downloaded from Intel's page, after accepting its license there")
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
    _list_deno()
    _list_sde(ctx)
    print(console.dim(f"\n  uv run dev.py {NAME} <name> [--editor <e>] [--emsdk-path <dir>] [--archive <file>] [--uninstall]"))


def _emsdk_target(args: argparse.Namespace) -> Path:
    """The emsdk to install or update: the one named, else the one dev.py already uses, else the default place."""
    if args.emsdk_path:
        return Path(args.emsdk_path).expanduser().resolve()
    return dev.find_emsdk_root() or dev.default_emsdk_root()


def _list_emsdk(ctx: Context) -> None:
    pinned = dev.pinned_emsdk_version()
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

    pinned = dev.pinned_emsdk_version()
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


def _installed_deno_version(exe: Path) -> str | None:
    try:
        out = subprocess.run([str(exe), "--version"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return jsr.deno_version_number(out.strip().splitlines()[0] if out.strip() else None)


def _list_deno() -> None:
    pinned = jsr.pinned_deno_version()
    print(f"{console.bold(_DENO)}  the second JS runtime for WASM artifacts (--runtime deno), pinned to {pinned} in tools/deno.version")
    exe = next((p for p in (jsr.default_deno_dir() / n for n in ("deno.exe", "deno")) if p.is_file()), None)
    if exe is None:
        print(f"  {console.dim('not installed')}")
        return
    installed = _installed_deno_version(exe)
    state = console.green(f"{installed} at {exe}") if installed == pinned \
        else console.yellow(f"{installed or 'unknown version'} at {exe}, not the pinned {pinned}")
    print(f"  {state}")


def _fetch(url: str) -> bytes:
    print(console.dim(f"  download {url}"), flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-dev"})
    with urllib.request.urlopen(request, timeout=300) as response:  # noqa: S310 (fixed github.com release host)
        return response.read()


def _install_deno(args: argparse.Namespace, ctx: Context) -> None:
    target_dir = jsr.default_deno_dir()
    if args.uninstall:
        print(f"  deno is a folder rather than a link, so nothing here deletes it; remove {target_dir} to uninstall it")
        return

    pinned = jsr.pinned_deno_version()
    target = _DENO_TARGETS.get((platform.system(), platform.machine()))
    if target is None:
        ctx.die(f"deno publishes no release for {platform.system()} {platform.machine()}")

    asset = f"deno-{target}.zip"
    base = f"{_DENO_RELEASES}/v{pinned}/{asset}"
    try:
        payload = _fetch(base)
        # The Windows checksum is Get-FileHash's table and the others sha256sum's line; both hold one 64-hex digest.
        digest = re.search(r"\b[0-9a-fA-F]{64}\b", _fetch(f"{base}.sha256sum").decode("utf-8", "replace"))
    except OSError as e:
        ctx.die(f"downloading deno {pinned} failed: {e}")
    if digest is None:
        ctx.die(f"{asset}.sha256sum holds no SHA-256 digest")
    if hashlib.sha256(payload).hexdigest() != digest.group(0).lower():
        ctx.die(f"{asset} does not match its published SHA-256; nothing was installed")

    exe_name = "deno.exe" if platform.system() == "Windows" else "deno"
    target_dir.mkdir(parents=True, exist_ok=True)
    exe = target_dir / exe_name
    with zipfile.ZipFile(io.BytesIO(payload)) as zf:
        # Written beside the old binary and swapped in, so a failed write leaves the old one working.
        staged = target_dir / f"{exe_name}.new"
        staged.write_bytes(zf.read(exe_name))
    staged.chmod(0o755)
    os.replace(staged, exe)

    installed = _installed_deno_version(exe)
    if installed != pinned:
        ctx.die(f"{exe} reports {installed} after installing {pinned}")
    print(f"  {console.green('installed')} deno {pinned} at {exe}")
    print(console.dim("  dev.py finds it there by itself: --runtime deno needs nothing else"))


def default_sde_dir() -> Path:
    """Where `dev.py install sde` unpacks the kit."""
    return Path.home() / "tools" / "sde"


def sde_executable() -> Path | None:
    """The installed `sde64`, which runs a 64-bit binary; None when it is not installed."""
    exe = default_sde_dir() / ("sde64.exe" if platform.system() == "Windows" else "sde64")
    return exe if exe.is_file() else None


def _sde_pin(ctx: Context) -> tuple[str, dict[str, str]]:
    """The pinned version, and the SHA-256 of its kit per suffix, from tools/sde.version."""
    # the driver's pin, like deno's: tools/ of shaped-core, also when a consumer runs it
    lines = (DRIVER_ROOT / "tools" / "sde.version").read_text(encoding="utf-8").split("\n")
    lines = [line.strip() for line in lines if line.strip()]
    return lines[0], dict(line.split() for line in lines[1:])


def _installed_sde_version() -> str | None:
    marker = default_sde_dir() / ".version"
    return marker.read_text(encoding="utf-8").strip() if marker.is_file() else None


def _list_sde(ctx: Context) -> None:
    version, _ = _sde_pin(ctx)
    print(f"{console.bold(_SDE)}  Intel's Software Development Emulator, to run a binary as another CPU, pinned to {version} in tools/sde.version")
    installed = _installed_sde_version()
    exe = sde_executable()
    if exe is None:
        print(f"  {console.dim('not installed')}")
    elif installed == version:
        print(f"  {console.green(f'{installed} at {exe}')}")
    else:
        print(f"  {console.yellow(f'{installed or 'unknown version'} at {exe}, not the pinned {version}')}")


def _install_sde(args: argparse.Namespace, ctx: Context) -> None:
    target_dir = default_sde_dir()
    if args.uninstall:
        print(f"  sde is a folder rather than a link, so nothing here deletes it; remove {target_dir} to uninstall it")
        return

    version, digests = _sde_pin(ctx)
    kit = _SDE_KITS.get(platform.system())
    if kit is None or platform.machine() not in ("AMD64", "x86_64"):
        ctx.die(f"Intel publishes no SDE kit for {platform.system()} {platform.machine()}")
    name = f"sde-external-{version}-{kit}.tar.xz"
    if not args.archive:
        # Intel asks to accept its license before the download, so the download is the user's, never this script's.
        print(f"  download {name} from {_SDE_PAGE}, accepting Intel's license there,")
        print(f"  then: uv run dev.py {NAME} sde --archive <path to {name}>")
        sys.exit(1)

    archive = Path(args.archive).expanduser().resolve()
    if not archive.is_file():
        ctx.die(f"{archive} is not a file")
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digests[kit]:
        ctx.die(f"{archive.name} is not the pinned {name}: its SHA-256 differs from tools/sde.version; nothing was installed")

    # Unpacked beside the old one and swapped in, so a failed unpack leaves the old one working.
    staged = target_dir.with_name("sde.new")
    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir(parents=True)
    with tarfile.open(archive, "r:xz") as tf:
        tf.extractall(staged, filter="data")
    # The kit holds one folder, sde-external-<version>-<kit>, whose contents are the install.
    (top,) = [p for p in staged.iterdir()]
    (top / ".version").write_text(version + "\n", encoding="utf-8")
    if target_dir.exists():
        shutil.rmtree(target_dir)
    os.replace(top, target_dir)
    shutil.rmtree(staged)

    exe = sde_executable()
    if exe is None:
        ctx.die(f"{target_dir} holds no sde64 after unpacking {name}")
    print(f"  {console.green('installed')} sde {version} at {exe}")
    print(console.dim(f"  e.g. {exe} -snb -- <program>: runs it as a Sandy Bridge, with AVX but no AVX2; `-help` lists the CPUs"))


def run(args: argparse.Namespace, ctx: Context) -> None:
    if not args.name:
        _list(ctx)
        return
    if args.name == _SDE:
        _install_sde(args, ctx)
        return
    if args.name == _EMSDK:
        _install_emsdk(args, ctx)
        return
    if args.name == _DENO:
        _install_deno(args, ctx)
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
