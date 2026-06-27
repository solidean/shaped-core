#!/usr/bin/env sh
# POSIX wrapper so CMAKE_<LANG>_COMPILER_LAUNCHER / _LINKER_LAUNCHER can point at
# the stdlib-only diag_launcher.py on Linux/macOS, mirroring how the Windows
# presets use the compiled diag-launcher.exe. It forwards every argument to the
# wrapped compiler/linker and preserves its exit code; the Python launcher writes
# the per-invocation .diag.json sidecar that build_diag reads.
#
# python3 is invoked directly (not the script's uv shebang) so each compile pays
# only interpreter startup, not an env resolve. diag_launcher.py is stdlib-only.
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if command -v python3 >/dev/null 2>&1; then
    exec python3 "$dir/diag_launcher.py" "$@"
fi
exec python "$dir/diag_launcher.py" "$@"
