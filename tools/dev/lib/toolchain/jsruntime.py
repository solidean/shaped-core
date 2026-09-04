"""Locating the JavaScript runtime that executes a WASM artifact.

A preset says what to compile; this says what runs the result.
Node and Deno both execute the same `.js` + `.wasm`, so the choice is orthogonal to the preset and belongs on the
command rather than in CMakePresets.json.

They are not interchangeable, though, which is the whole reason both are supported: Node's WebGPU binding is Dawn,
what Chrome ships, while Deno's is wgpu, what Firefox ships.
Running a wasm graphics build under both is therefore how the two browser engines get covered from the CLI, without
a browser.

Node is the default, and Deno is opt-in rather than auto-detected when present.
A default that depended on what happened to be installed would mean two machines silently exercised two different
WebGPU implementations, which is exactly the divergence the pair is meant to catch.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

KINDS = ("node", "deno")
DEFAULT_KIND = "node"

# Env fallbacks, mirroring SC_EMSDK_PATH's role for the emsdk.
_PATH_ENV = {"node": "SC_NODE_PATH", "deno": "SC_DENO_PATH"}
_KIND_ENV = "SC_JS_RUNTIME"


@dataclass(frozen=True)
class JsRuntime:
    kind: str
    exe: Path
    version: str | None

    @property
    def launch_prefix(self) -> list[str]:
        """The argv prefix that runs an artifact — `[*prefix, artifact, ...args]`."""
        if self.kind == "deno":
            # Deno denies filesystem and env access by default, and a nexus binary needs both.
            # NODERAWFS reads and writes real paths, and the JUnit report is a file write.
            # `run` is also required before the script path.
            #
            # --unstable-detect-cjs because Emscripten's pthread builds emit a top-level require("node:worker_threads")
            # into a `.js`, which Deno reads as ESM and rejects before the module runs at all.
            # Single-threaded output emits no require and does not need it, but the flag is harmless there.
            return [str(self.exe), "run", "--allow-all", "--unstable-detect-cjs"]
        return [str(self.exe)]

    def describe(self) -> str:
        return f"{self.kind} {self.version or '(version unknown)'} at {self.exe}"


def _version_of(exe: Path) -> str | None:
    try:
        proc = subprocess.run([str(exe), "--version"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    first = (proc.stdout or proc.stderr).strip().splitlines()
    return first[0].strip() if first else None


def _locate(kind: str, explicit: str | None, env: dict[str, str] | None) -> Path | None:
    """Find one runtime's executable: an explicit path, then its SC_*_PATH env var, then PATH.

    `env` is the environment the search runs against, so an emsdk overlay's PATH is honored rather than the
    inherited one.
    """
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    from_env = (env or os.environ).get(_PATH_ENV[kind])
    if from_env:
        candidates.append(Path(from_env))

    # emsdk ships a pinned node, and a wasm run should use that rather than whatever the machine happens to have.
    # Taken from the environment ahead of PATH rather than by relying on the emsdk overlay sorting first.
    if kind == "node":
        from_emsdk = (env or os.environ).get("EMSDK_NODE")
        if from_emsdk:
            candidates.append(Path(from_emsdk))

    for c in candidates:
        # A directory is accepted as the install dir, which is how these are usually spelled.
        if c.is_dir():
            hit = next((c / n for n in (f"{kind}.exe", kind) if (c / n).is_file()), None)
            if hit:
                return hit
        elif c.is_file():
            return c

    search_path = (env or os.environ).get("PATH") or (env or os.environ).get("Path")
    found = shutil.which(kind, path=search_path)
    return Path(found) if found else None


def resolve(
    *,
    kind: str | None = None,
    node_path: str | None = None,
    deno_path: str | None = None,
    env: dict[str, str] | None = None,
) -> JsRuntime | None:
    """Pick the runtime to launch WASM artifacts with, or None when it cannot be found.

    Precedence: an explicit --node-path / --deno-path (which also selects that runtime), then --runtime, then
    SC_JS_RUNTIME, then node.
    Passing a path for one runtime while naming the other is a caller error and raises.
    """
    if node_path and deno_path and not kind:
        raise ValueError("--node-path and --deno-path are both set; add --runtime to say which one to use")

    chosen = kind
    if chosen is None:
        if deno_path:
            chosen = "deno"
        elif node_path:
            chosen = "node"
        else:
            chosen = (env or os.environ).get(_KIND_ENV) or DEFAULT_KIND
    if chosen not in KINDS:
        raise ValueError(f"unknown JS runtime {chosen!r}: expected one of {', '.join(KINDS)}")

    explicit = deno_path if chosen == "deno" else node_path
    exe = _locate(chosen, explicit, env)
    if exe is None:
        return None
    return JsRuntime(kind=chosen, exe=exe, version=_version_of(exe))


@dataclass(frozen=True)
class JsRuntimeRequest:
    """What the command line asked for, before anything is looked up.

    Kept separate from JsRuntime so resolution can happen late, inside the pipeline, where the emsdk environment is
    already built -- node must be searched on that overlay's PATH rather than the inherited one.
    """

    kind: str | None = None
    node_path: str | None = None
    deno_path: str | None = None

    @classmethod
    def from_args(cls, args) -> "JsRuntimeRequest":
        return cls(
            kind=getattr(args, "runtime", None),
            node_path=getattr(args, "node_path", None),
            deno_path=getattr(args, "deno_path", None),
        )

    def resolve(self, env: dict[str, str] | None = None) -> JsRuntime | None:
        return resolve(kind=self.kind, node_path=self.node_path, deno_path=self.deno_path, env=env)


# Artifact suffixes that are not directly runnable and must be launched by a JS runtime.
# Emscripten emits a `<name>.js` loader next to the `.wasm`.
WASM_LAUNCH_SUFFIXES = {".js", ".mjs", ".wasm"}


def needs_launcher(is_emscripten: bool, artifact: Path) -> bool:
    """Whether this artifact has to be handed to a JS runtime rather than executed directly."""
    return is_emscripten or artifact.suffix.lower() in WASM_LAUNCH_SUFFIXES


class LazyLauncher:
    """The launch prefix for WASM artifacts, resolved at most once and only when one is actually launched.

    Native runs must not pay for -- or fail on -- a runtime lookup they never use, which is why this is lazy
    rather than resolved up front alongside the preset.
    """

    def __init__(self, request: "JsRuntimeRequest | None" = None, env: dict[str, str] | None = None):
        self._request = request or JsRuntimeRequest()
        self._env = env
        self._runtime: JsRuntime | None = None
        self._resolved = False

    @property
    def runtime(self) -> JsRuntime | None:
        if not self._resolved:
            self._runtime = self._request.resolve(self._env)
            self._resolved = True
        return self._runtime

    def prefix(self) -> list[str]:
        rt = self.runtime
        if rt is None:
            wanted = self._request.kind or DEFAULT_KIND
            raise RuntimeError(
                f"no {wanted} found to run the WASM artifact; pass --{wanted}-path or put {wanted} on PATH"
            )
        return rt.launch_prefix
