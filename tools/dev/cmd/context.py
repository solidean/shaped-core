"""Per-command execution context: project policy plus the shared resolver glue.

`Policy` is pure project data — which presets each command reaches for — and is
declared in dev.py (the one place a downstream fork edits). `Context` bundles the
repo root with that policy and the cross-command helpers every command needs
(preset/target resolution, the platform default, the `*-test` convention, error
exit). dev.py builds one Context and hands it to each command's `run(args, ctx)`,
so commands never reach back into dev.py.
"""

from __future__ import annotations

import argparse
import fnmatch
import platform
import sys
from dataclasses import dataclass
from pathlib import Path

from tools import dev
from tools.dev import console


@dataclass(frozen=True)
class Policy:
    """Project policy: which presets each command uses, keyed by platform.system()."""

    default_build: dict[str, str]
    default_debug: dict[str, str]
    default_release: dict[str, str]
    default_sanitize: dict[str, str]
    coverage_build: dict[str, str]
    pgo_generate: dict[str, str]
    pgo_use: dict[str, str]
    pgo_baseline: dict[str, str]
    web_preset: str


@dataclass
class Context:
    """Everything a command needs from the project: the root, the policy, and glue."""

    root: Path
    policy: Policy

    def die(self, msg: str) -> None:
        print(console.red(f"ERROR: {msg}"), file=sys.stderr)
        sys.exit(1)

    def rel(self, p: Path) -> str:
        """Path relative to the repo root (posix style) for compact hints."""
        return dev.report.rel(p, self.root)

    def fail_build(self, results: list[dev.StepResult], presets: list[dev.Preset]) -> None:
        """Report a failed build phase and exit(1) (for the build/test commands)."""
        dev.report.print_build_failure(results, presets, self.root)
        sys.exit(1)

    def is_test_target(self, target: dev.Target) -> bool:
        """Project convention: test executables are named '*-test'."""
        return target.kind == "EXECUTABLE" and target.name.endswith("-test")

    def default_preset_name(self) -> str:
        name = self.policy.default_build.get(platform.system())
        if name is None:
            self.die(f"No default preset for {platform.system()!r}. Use --preset.")
        return name

    def resolve_presets(self, specs: list[str] | None) -> list[dev.Preset]:
        """Resolve --preset specs, falling back to the platform default."""
        try:
            presets = dev.resolve_presets(self.root, specs or [])
            if not presets:
                presets = dev.resolve_presets(self.root, [self.default_preset_name()])
            return presets
        except dev.PresetError as e:
            self.die(str(e))

    def resolve_build_presets(self, args: argparse.Namespace) -> list[dev.Preset]:
        """Resolve --preset and apply the --toolset / --build-suffix / --build-dir overrides.

        Used by the configure/build/test commands; validates a pinned toolset eagerly so an
        unresolvable one fails fast with a clean message instead of mid-build.
        """
        presets = self.resolve_presets(args.preset)
        try:
            return dev.apply_overrides(
                presets, root=self.root,
                toolset=args.toolset, build_suffix=args.build_suffix, build_dir=args.build_dir,
            )
        except dev.ToolsetError as e:
            self.die(str(e))

    def discover(self, preset: dev.Preset, emsdk_path: str | None = None) -> list[dev.Target]:
        """Discover targets for a preset, auto-configuring if needed."""
        try:
            return dev.discover_targets(preset.build_dir, preset.build_type)
        except dev.NotConfiguredError:
            result = dev.ensure_configured(preset, root=self.root, emsdk_path=emsdk_path)
            if result is not None and not result.ok:
                self.die(f"Configure failed for {preset.name!r}")
            return dev.discover_targets(preset.build_dir, preset.build_type)

    def resolve_target_names(
        self, preset: dev.Preset, specs: list[str] | None, emsdk_path: str | None = None
    ) -> list[str] | None:
        """Expand --target specs into concrete target names against a preset.

        Returns None when no specs were given (meaning 'build everything').
        """
        patterns: list[str] = []
        for spec in specs or []:
            patterns.extend(s.strip() for s in spec.split(",") if s.strip())
        if not patterns:
            return None

        available = self.discover(preset, emsdk_path)
        names = [t.name for t in available]
        selected: list[str] = []
        seen: set[str] = set()
        for pat in patterns:
            matches = [pat] if pat in names else [n for n in names if fnmatch.fnmatch(n, pat)]
            if not matches:
                self.die(f"No target matches {pat!r}. Available: {', '.join(sorted(names))}")
            for n in matches:
                if n not in seen:
                    seen.add(n)
                    selected.append(n)
        return selected
