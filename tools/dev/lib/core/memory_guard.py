"""A memory guard over everything dev.py starts: no process past a cap, the whole of them past a budget, and none of them once RAM runs low.

A build or test that swaps takes the machine with it, and parallel links and runaway tools have done so; a failed run is the better outcome.
On Windows dev.py puts itself into a Job Object, which every process it starts inherits:
- each process may commit at most the per-process cap, 30 GB by default;
- all of them together at most the total, half of physical RAM by default;
- an allocation past either fails in the process that asked, so a runaway ends on its own failed allocation.
On every platform a watchdog thread reads the free physical memory, and below the floor, 5 GB by default, it ends dev.py and everything it started.
Elsewhere than Windows only the watchdog runs: a per-process address-space limit would also refuse the sanitizers' shadow reservations.

The environment overrides each, in GB, 0 turning it off: SC_DEV_MEMORY_PROCESS_GB, SC_DEV_MEMORY_TOTAL_GB, SC_DEV_MEMORY_FREE_GB.
The user chose the defaults (2026-10-31): gates failing on them is accepted, swapping is not.
"""

from __future__ import annotations

import os
import platform
import sys
import threading
import time

GB = 1 << 30
DEFAULT_PROCESS_GB = 30.0
DEFAULT_FREE_GB = 5.0
WATCH_SECONDS = 0.5

_installed = False


def _limit(name: str, default: float) -> float:
    """The limit in GB from the environment, or `default`; 0 or less turns it off."""
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return float(raw)
    except ValueError:
        print(f"warning: {name}={raw!r} is not a number of GB; using {default:g}", file=sys.stderr)
        return default


def _report(message: str) -> None:
    try:
        print(f"\ndev.py memory guard: {message}", file=sys.stderr, flush=True)
    except OSError:
        pass


if platform.system() == "Windows":
    import ctypes
    from ctypes import wintypes

    class _MemoryStatus(ctypes.Structure):
        _fields_ = [
            ("dwLength", wintypes.DWORD),
            ("dwMemoryLoad", wintypes.DWORD),
            ("ullTotalPhys", ctypes.c_ulonglong),
            ("ullAvailPhys", ctypes.c_ulonglong),
            ("ullTotalPageFile", ctypes.c_ulonglong),
            ("ullAvailPageFile", ctypes.c_ulonglong),
            ("ullTotalVirtual", ctypes.c_ulonglong),
            ("ullAvailVirtual", ctypes.c_ulonglong),
            ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
        ]

    class _BasicLimits(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_longlong),
            ("PerJobUserTimeLimit", ctypes.c_longlong),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class _IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount", "ReadTransferCount",
                     "WriteTransferCount", "OtherTransferCount")]

    class _ExtendedLimits(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _BasicLimits),
            ("IoInfo", _IoCounters),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    _JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x100
    _JOB_OBJECT_LIMIT_JOB_MEMORY = 0x200
    _JobObjectExtendedLimitInformation = 9
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    _kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    _kernel32.SetInformationJobObject.restype = wintypes.BOOL
    _kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    _kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    _kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _kernel32.TerminateJobObject.restype = wintypes.BOOL
    _kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    _kernel32.GlobalMemoryStatusEx.restype = wintypes.BOOL
    _kernel32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(_MemoryStatus)]

    def _memory() -> tuple[int, int]:
        """Physical memory in bytes: (total, available)."""
        status = _MemoryStatus()
        status.dwLength = ctypes.sizeof(_MemoryStatus)
        if not _kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return 0, 0
        return int(status.ullTotalPhys), int(status.ullAvailPhys)

    def _job(process_bytes: int, total_bytes: int):
        """A job with the caps, this process in it, or None where Windows refused; what dev.py starts after this inherits it."""
        handle = _kernel32.CreateJobObjectW(None, None)
        if not handle:
            return None
        limits = _ExtendedLimits()
        if process_bytes > 0:
            limits.BasicLimitInformation.LimitFlags |= _JOB_OBJECT_LIMIT_PROCESS_MEMORY
            limits.ProcessMemoryLimit = process_bytes
        if total_bytes > 0:
            limits.BasicLimitInformation.LimitFlags |= _JOB_OBJECT_LIMIT_JOB_MEMORY
            limits.JobMemoryLimit = total_bytes
        if not _kernel32.SetInformationJobObject(handle, _JobObjectExtendedLimitInformation, ctypes.byref(limits),
                                                 ctypes.sizeof(limits)):
            return None
        if not _kernel32.AssignProcessToJobObject(handle, _kernel32.GetCurrentProcess()):
            return None
        return handle

    def _end_everything(job) -> None:
        if job is not None:
            _kernel32.TerminateJobObject(job, 3)
        os._exit(3)

else:

    def _memory() -> tuple[int, int]:
        """Physical memory in bytes: (total, available), from /proc/meminfo; (0, 0) where there is none."""
        try:
            with open("/proc/meminfo", encoding="ascii") as f:
                fields = {line.split(":")[0]: int(line.split()[1]) * 1024 for line in f if line.split()[1:2]}
        except (OSError, ValueError, IndexError):
            return 0, 0
        return fields.get("MemTotal", 0), fields.get("MemAvailable", 0)

    def _job(process_bytes: int, total_bytes: int):
        return None

    def _parents() -> dict[int, int]:
        """Every process's parent, from /proc where there is one and from `ps` elsewhere (macOS)."""
        parents: dict[int, int] = {}
        if os.path.isdir("/proc"):
            for entry in os.listdir("/proc"):
                if not entry.isdigit():
                    continue
                try:
                    with open(f"/proc/{entry}/stat", encoding="ascii", errors="replace") as f:
                        # the name in parentheses may hold spaces; the parent is the second field after it
                        parents[int(entry)] = int(f.read().rsplit(")", 1)[1].split()[1])
                except (OSError, ValueError, IndexError):
                    pass
            return parents
        import subprocess
        try:
            out = subprocess.run(["ps", "-A", "-o", "pid=,ppid="], capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            return parents
        for line in out.splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[0].isdigit() and fields[1].isdigit():
                parents[int(fields[0])] = int(fields[1])
        return parents

    def _descendants() -> list[int]:
        children: dict[int, list[int]] = {}
        for pid, parent in _parents().items():
            children.setdefault(parent, []).append(pid)
        found: list[int] = []
        pending = [os.getpid()]
        while pending:
            for child in children.get(pending.pop(), []):
                found.append(child)
                pending.append(child)
        return found

    def _end_everything(job) -> None:
        # Never the process group: without job control, as in a CI step, it holds whoever started dev.py too.
        # The second pass catches what the first one's survivors started in between.
        for _ in range(2):
            for pid in _descendants():
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass
        os._exit(3)


def install() -> None:
    """Put the guard over this process and everything it starts from now on; once per process, the later calls do nothing."""
    global _installed
    if _installed:
        return
    _installed = True
    total_physical, _ = _memory()
    process_gb = _limit("SC_DEV_MEMORY_PROCESS_GB", DEFAULT_PROCESS_GB)
    total_gb = _limit("SC_DEV_MEMORY_TOTAL_GB", total_physical / 2 / GB)
    free_gb = _limit("SC_DEV_MEMORY_FREE_GB", DEFAULT_FREE_GB)
    job = _job(int(max(process_gb, 0) * GB), int(max(total_gb, 0) * GB))
    if free_gb <= 0 or total_physical == 0:
        return
    floor = int(free_gb * GB)

    def watch() -> None:
        while True:
            _, available = _memory()
            if 0 < available < floor:
                _report(f"only {available / GB:.1f} GB of RAM is free, below the {free_gb:g} GB floor; "
                        f"ending dev.py and everything it started rather than swap (SC_DEV_MEMORY_FREE_GB sets the floor)")
                _end_everything(job)
            time.sleep(WATCH_SECONDS)

    threading.Thread(target=watch, name="dev.py memory guard", daemon=True).start()
