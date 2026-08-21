# systems/recording-formats — getting a recording out of the process it was made in

**Nothing here is stable, and will not be for a good while.**
A reader checks the version and refuses a file it does not understand, rather than misreading one.
Durability comes from an exporter — [babel::chrome_trace](../../../../data/babel-serializer/src/babel-serializer/trace/chrome_trace.hh) — not from these bytes.

A live recording is process-local by construction: its events point at descriptors, and descriptors are static objects in the binary that produced them.
Serializing is exactly the job of resolving those pointers.

---

## One format, two writers

There are two situations that need to write a recording, and they have very different constraints.

`cc::rec::serialize` runs in a healthy process and may allocate.
The crash dump runs from a signal handler or an SEH filter and may not.

They produce **the same format**, through the same builder.
That is not tidiness: only one of the two is easy to exercise the day someone changes the layout, and a second writer that drifts would be discovered during a crash.

```cpp
#include <clean-core/record/serialize.hh>
auto const bytes = cc::rec::serialize(recording);      // -> cc::vector<byte>
cc::rec::save_recording(recording, path);              // -> cc::result<cc::unit>

auto loaded = cc::rec::load_recording(path);           // -> cc::result<loaded_recording>
auto const& r = loaded.value().events();               // an ordinary recording
```

A `loaded_recording` **owns** the descriptors, domains, units, field layouts and strings its events point at, so it must outlive whatever reads them.
That is the only way a loaded recording differs from a live one — the algebra, the queries and replay all work identically.

One consequence worth knowing: **`from_domain(domain const*)` matches nothing on a loaded recording**, because it owns its own domain objects.
`from_domain(string_view)` is the form that survives a round trip.

---

## What travels, and what does not

Everything a consumer needs to interpret an event without having heard of the code that produced it:

| carried | why |
|---|---|
| every descriptor: kind, level, name, source file/function/line, payload field layout | so a name is not the only thing that survives |
| the unit, **by value** | a reader that never linked against the recording binary still knows a byte from a second |
| the domain name and its enable mask | categories, and what was on at the time |
| the thread id, index and name | a multi-lane trace is unreadable without them |
| per block: the chunk sequence, layer, and both (cycles, wall) pairs | so a cycle count still maps to a time |
| the measured cycle rate | so a duration in cycles becomes one in seconds |

Two things do not travel.

**The stream state** — the ambient context and open scopes a chunk started under — is *derived*, and a loaded recording has no live producer to derive it from.
`chunk_view::state_at_start` is null on every loaded block.

**Pinned payloads** are process-local by definition.
Nothing produces one yet; when something does, this is where the decision goes.

`cc::rec::source_ref` exists for this reason and not for a stylistic one.
`cc::source_location` has no constructor, so a loader could never rebuild one, and a recording that had been through a file would lose the field that says where it came from.

---

## The layout

Little-endian, every table eight-byte aligned, every offset from the start of the file.
The structs are in [impl/serialized_format.hh](../../src/clean-core/record/impl/serialized_format.hh), each with a `static_assert` on its size.
A layout change that does not bump the version is the one mistake this format makes easy.

```text
header      magic "CCRECORD", version, flags, wall time, cycle rate, counts and offsets
strings     one arena; every name is a {offset, length} into it
domains     name + enable mask
units       inlined by value
fields      the payload layouts descriptors point into
descs       one per recording site; unit and domain by index, fields by range
threads     id, index, name
blocks      per block: thread, layer, chunk sequence, both time pairs, and where its bytes are
events      the event bytes themselves
```

**In the file, an event header's `desc` slot holds a descriptor INDEX rather than a pointer.**
Everything else about the event stream is byte-identical to the live one, so writing is a copy plus one word per event and reading is the same word patched back.
There is no second decoder to keep in step with the first, which is the point.

---

## The crash dump

```cpp
#include <clean-core/record/crash_dump.hh>
cc::install_crash_handler();                            // the hook list this rides on
cc::rec::install_crash_dump({.path = "crash.ccrec"});   // reserves its arena NOW
```

The constraint that shapes it: **the dump must not allocate.**
A crash inside the allocator is exactly the case where a dump is most wanted and an allocating writer would deadlock.
So the arena its tables are built in is reserved at install time, and the path is copied then too.

**No thread is suspended, and none needs to be.**
A chunk's committed watermark is release-stored after the bytes it covers, so reading up to it can never catch a torn event.
The worst a still-running thread costs the dump is its newest event — a far better trade than the deadlock risk of suspending threads that may hold the loader lock.

The dump reads the chunks directly rather than going through a listener, so **it sees events nothing ever drained**.
On a path where nothing is going to run again, that is the whole point.

An arena too small does not fail: it truncates, and the file says so through `loaded_recording::is_truncated()`.
A truncated dump that loads is worth more than a complete one that does not exist.

`cc::rec::write_crash_dump_now()` runs the identical path on demand.
That is what makes the constrained writer testable at all, rather than something to find out about during a crash.

---

## Known gaps

- **Nexus does not install a dump yet.** It belongs with the rest of the nexus wiring, which waits on ambient filtering.
- **A dump is not a minidump.** It carries what was recorded, not the machine state; `cc::install_crash_handler` still owns the stack traces.
- **Stacktrace events carry return addresses, never names.**
  Symbolizing them needs the module base table below plus the matching binaries, which is why analysis happens offline and a capture stays cheap.
