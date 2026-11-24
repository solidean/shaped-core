"""Which processes hold a file open, for a build step that could not write its output.

A linker that cannot replace an executable says only "permission denied", and the process holding it is usually gone a
moment later, so the question is answered when the failure is reported, from the failed step's own log.
Windows answers it through the Restart Manager, which every process can ask about any file without elevation.
Elsewhere a file is replaced by a rename that no open handle blocks, so there is nothing to ask and nothing is reported.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

# lld-link's and MSVC link's spellings of an output they could not write; the path is relative to the build directory.
_UNWRITABLE = [
    re.compile(r"failed to write output '([^']+)': permission denied", re.IGNORECASE),
    re.compile(r"failed to write the output file: '?([^':]+?)'?:? permission denied", re.IGNORECASE),
    re.compile(r"LNK1104: cannot open file '([^']+)'"),
    re.compile(r"LNK1168: cannot open (\S+) for writing"),
]


@dataclass(frozen=True)
class Holder:
    pid: int
    name: str
    # The executable's full path, or empty where the process will not say: one of another user's, or a protected one.
    path: str
    # The service's short name for a process that is one, such as `WinDefend`; empty otherwise.
    service: str
    # What started it, `name (pid n)`, since a holder that is a tool of ours only says who ran it through its parent.
    parent: str = ""


def unwritable_outputs(log_text: str, build_dir: Path) -> list[Path]:
    """The outputs a link step's log says it could not write, each once, made absolute against `build_dir`."""
    found: list[Path] = []
    for pattern in _UNWRITABLE:
        for match in pattern.finditer(log_text):
            path = Path(match.group(1).strip())
            path = path if path.is_absolute() else build_dir / path
            if path not in found:
                found.append(path)
    return found


def holders_of(path: Path) -> list[Holder] | None:
    """The processes holding `path` open now; None where this platform or this call cannot say."""
    if sys.platform != "win32":
        return None
    try:
        return _restart_manager_holders(str(path))
    except OSError:
        return None


def _restart_manager_holders(path: str) -> list[Holder] | None:
    import ctypes
    from ctypes import wintypes

    class UniqueProcess(ctypes.Structure):
        _fields_ = [("pid", wintypes.DWORD), ("start", wintypes.FILETIME)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [
            ("process", UniqueProcess),
            ("app_name", wintypes.WCHAR * 256),
            ("service", wintypes.WCHAR * 64),
            ("app_type", ctypes.c_int),
            ("status", wintypes.ULONG),
            ("session", wintypes.DWORD),
            ("restartable", wintypes.BOOL),
        ]

    rm = ctypes.WinDLL("rstrtmgr")
    session = wintypes.DWORD()
    key = ctypes.create_unicode_buffer(33)
    if rm.RmStartSession(ctypes.byref(session), 0, key) != 0:
        return None
    try:
        files = (wintypes.LPCWSTR * 1)(path)
        if rm.RmRegisterResources(session, 1, files, 0, None, 0, None) != 0:
            return None
        error_more_data = 234
        needed = wintypes.UINT(0)
        count = wintypes.UINT(0)
        reasons = wintypes.DWORD()
        result = rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), None, ctypes.byref(reasons))
        if result == 0:
            return []
        if result != error_more_data:
            return None
        infos = (ProcessInfo * needed.value)()
        count = wintypes.UINT(needed.value)
        if rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), infos, ctypes.byref(reasons)) != 0:
            return None
        parents = _parents()
        holders = []
        for i in infos[: count.value]:
            pid = int(i.process.pid)
            parent = parents.get(pid)
            parent_text = ""
            if parent is not None:
                parent_path = _image_path(parent[0])
                parent_text = f"{parent[1]} (pid {parent[0]}{', ' + parent_path if parent_path else ''})"
            holders.append(Holder(pid=pid, name=i.app_name, path=_image_path(pid), service=i.service, parent=parent_text))
        return holders
    finally:
        rm.RmEndSession(session)


def _parents() -> dict[int, tuple[int, str]]:
    """Every process's parent pid and executable name, from one snapshot of them all."""
    import ctypes
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [
            ("size", wintypes.DWORD),
            ("usage", wintypes.DWORD),
            ("pid", wintypes.DWORD),
            ("heap", ctypes.c_size_t),
            ("module", wintypes.DWORD),
            ("threads", wintypes.DWORD),
            ("parent", wintypes.DWORD),
            ("priority", ctypes.c_long),
            ("flags", wintypes.DWORD),
            ("exe", wintypes.WCHAR * 260),
        ]

    snapshot_process = 0x2
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snapshot = kernel.CreateToolhelp32Snapshot(snapshot_process, 0)
    if not snapshot or snapshot == wintypes.HANDLE(-1).value:
        return {}
    try:
        names: dict[int, str] = {}
        parent_of: dict[int, int] = {}
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(ProcessEntry)
        ok = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            names[int(entry.pid)] = entry.exe
            parent_of[int(entry.pid)] = int(entry.parent)
            ok = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        # a parent that exited may have its pid reused, so a name is given only for one still listed
        return {pid: (ppid, names.get(ppid, "exited")) for pid, ppid in parent_of.items()}
    finally:
        kernel.CloseHandle(snapshot)


def _image_path(pid: int) -> str:
    import ctypes
    from ctypes import wintypes

    query_limited_information = 0x1000
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    handle = kernel.OpenProcess(query_limited_information, False, pid)
    if not handle:
        return ""
    try:
        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return ""
        return buffer.value
    finally:
        kernel.CloseHandle(handle)


def describe(holder: Holder) -> str:
    """`ccs.exe (pid 22972, C:/.../ccs.exe), started by ...`, the service's name where the process is one."""
    what = holder.path or holder.name
    service = f", service {holder.service}" if holder.service else ""
    started = f", started by {holder.parent}" if holder.parent else ""
    return f"{holder.name} (pid {holder.pid}{service}{', ' + what if what != holder.name else ''}){started}"
