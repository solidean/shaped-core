# System info and resource metrics

What machine this is, how loaded it is, and what this process is consuming.

The framing is **"build your own system resource dashboard with cc"**, not "build your own task manager".
What the machine is and what it is doing, with no enumeration of other processes — that is deferred, not designed out.

## Three concepts, and why they are apart

Everything here is one of three things, and mixing them is what makes a resource API dishonest.

| | what it is | shape | example |
|---|---|---|---|
| **description** | what cannot change while the process runs | memoized, returned by reference | `cc::get_system_info()` |
| **snapshot** | a level that is true right now | a plain query | `cc::query_memory_usage()` |
| **sampler** | what changed since last time | an object holding a baseline | `cc::cpu_load_sampler` |

**A rate has no value at an instant.**
Every OS stores CPU time as counters that only climb, and a load is the difference of two readings over the wall time
between them.
So a sampler holds its own previous reading rather than a hidden process-wide one, and two subsystems sampling at
different cadences cannot corrupt each other's numbers.

`cc::take_resource_snapshot()` gathers every level at one instant and **excludes samplers on purpose** — including a
rate there would mean either a zero that reads like an idle machine or a hidden baseline somewhere.

## Two rules the headers state

**Never synthesize a plausible value.**
A field the platform cannot answer stays absent.
No defaulting a core count to 1, no zero-filling a load, no reporting a wasm heap size as total RAM.
A zero-filled dashboard reads as a working dashboard on an idle machine, and nobody looks twice.

Every defect this API produced during development was a plausible-looking number, not a crash:
Windows' `ProductName` still reads "Windows 10" on Windows 11; a CPUID hypervisor bit is set on bare metal because
Windows enables VBS; `ullTotalPageFile` is a commit limit rather than a page file, so a swap figure derived from it
read 100% on an idle machine; NDIS stacks a filter pseudo-interface per adapter, so one NIC appeared four times and a
sum over interfaces quadrupled the machine's traffic.

**A load of 1 is the whole machine.**
Never one core.
A process using two cores of thirty-two reports `0.0625`, not `2.0`; `cores_used` carries the other reading beside it so
neither costs the caller a multiply.

## The surfaces

| header | what it answers |
|---|---|
| `platform/system_info.hh` | CPU topology and caches, memory, OS — the description |
| `platform/system_identifier.hh` | hostname, user, machine id — behind a required flags argument |
| `platform/resource_limits.hh` | cgroup quota, job object, affinity, and `recommended_worker_count()` |
| `platform/system_metrics.hh` | CPU load, memory usage, and the `query_error` every live query reports through |
| `platform/process_metrics.hh` | this process's own usage |
| `platform/storage_devices.hh` | mounts (usage) and devices (I/O) |
| `platform/network_devices.hh` | interfaces and traffic |
| `platform/resource_snapshot.hh` | every level at one instant |

GPU load and VRAM are **not here**.
There is no portable "how busy is the GPU" syscall anywhere, and the memory figures come from the graphics API rather
than the OS, so `sg` owns them and cc never mentions GPUs — see `shaped-graphics/context/gpu_metrics.hh`.

## Sizing a thread pool

`cc::get_system_info().logical_cores()` is the **machine**, and is never clamped to a container.
That is deliberate: a stamp wants "this was a 64-core host" and a thread pool wants "you may use 4", and folding them
together destroys the first.

**Size from `cc::recommended_worker_count()`**, which folds the machine, the affinity mask and the CPU quota, whichever
binds first.
On a CI runner with a 4-CPU cgroup on a 64-core host, the two differ by 16x.

## Identity is a separate type on purpose

`cc::system_identifier` carries the hostname, user name, machine id, hardware addresses and disk serials, and every
field of it is personal data.

Keeping it out of `cc::system_info` makes the safe default **structural rather than a matter of discipline**: a caller
holding a `cc::system_info` cannot leak a hostname into a recording, because the field is not reachable from what it is
holding.
The flags argument has no default, so a call site says what it collected.

## Stamping a recording

`cc::rec::emit_stamp` writes the machine description and a resource snapshot into whatever is recording, as `key=value`
lines that need no codec and no schema.
A higher library adds its own section with `cc::rec::register_stamp_contributor` — that is how `sg` stamps which GPU is
in the machine without cc knowing GPUs exist.

**It is explicit rather than automatic**, and record/stamp.hh says why: a stamp is an ordinary event, so it reaches
every listener registered at that moment rather than only the recording being opened.

## What each platform answers

| | Windows | Linux | macOS | Android | wasm / WASI |
|---|---|---|---|---|---|
| description | full | full | full | most | logical core count |
| CPU load, memory | yes | yes | yes | yes | no |
| process usage | yes | yes | yes | yes | no |
| mounts | yes | yes | yes | yes | no |
| disk I/O | yes | yes | **no** (IOKit) | yes | no |
| network | yes | yes | yes | yes | no |
| machine id | yes | yes | **no** (IOKit) | yes | no |

Absent is reported as absent everywhere; no platform returns a fabricated value.

## Known gaps

- **GPU load** is declared on `sg::context` and refuses on every backend.
  The routes are `D3DKMTQueryStatistics` on Windows, `gpu_busy_percent` on Linux, IOKit on macOS, and none is
  implemented.
- **Open file descriptors** are absent on Linux: counting them means listing `/proc/self/fd`, and cc has no directory
  walk.
  `FDSize` is not it — that is allocated table slots, a much larger number.
- **RAM speed** is only ever reported where SMBIOS is readable, which in practice means nowhere yet.
- `cc::impl::trimmed` and `next_line` in `platform/impl/text_file.hh` belong on `cc::string_view` and are local until it
  grows them.

## See also

- [logging.md](../logging.md) and [profiling.md](../profiling.md) — the recording surfaces a stamp lands in
- [systems/recording.md](recording.md) — the mechanism underneath
- `clean-core/system-info` and `clean-core/system-monitor` — the one-shot dump and the live dashboard
