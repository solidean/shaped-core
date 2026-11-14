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

### A payload slot holding a pointer is always eight bytes

`desc_ref`, `cstring` and `pinned_bytes` all hold an address, and all three occupy **eight bytes regardless of the writer's pointer width**.

That is a rule rather than an accident of the machines we develop on.
A field's offset and size live in the descriptor, so letting them follow `sizeof(void*)` would make the wire layout depend on the architecture that wrote it.
wasm32 is what makes that concrete: four-byte pointers, against a file the rest of the tier reads as 64-bit.
See [platforms.md](../../../../../docs/platforms.md#64-bit-only), which is where that split is spelled out.

So a producer widens the address on the way in and a reader narrows it on the way out; neither ever memcpys a native pointer into or out of a payload.
`event_view::field_as_desc`, `field_as_text` and `field_as_bytes` are what do the narrowing, which is the other reason to go through them.

The stream state travels too, because it is an ordinary event rather than something alongside the stream.
Every chunk opens with an `event_kind::stream_state` naming the trace and the scopes already open, so it is written and read like any other event — see [recording.md](recording.md#the-chunk-preamble).

Its `desc_ref` fields are the one exception to "the event stream is byte-identical to the live one".
A `desc_ref` holds a `rec::desc const*`, which means nothing outside the process that wrote it.
So it is rewritten into a descriptor-table index on the way out and back into a pointer on the way in — the same treatment an event header's own descriptor gets.
That is why both writers patch payloads rather than only headers.

**Pinned payloads** travel in a blob section of their own, last in the file.
A `type_code::pinned_bytes` field holds an address live and an offset into that section once written, so the event stream stays byte-for-byte the size it was while the bytes themselves come along.
Last rather than beside the other tables because it is the only section with no bound on its size — a reader that only wants the event stream never has to seek past it.

Both writers stream those bytes straight from behind their pins rather than through the builder's arena.
Same reason the events go out from the chunks: the arena is a fixed reservation, and pinned data is exactly what would not fit in one.
The blob table is deduplicated by address and size, so two events recording the same pinned data cost the file one copy.

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
modules     which binaries were mapped where, and which build each was
threads     id, index, name
blocks      per block: thread, layer, chunk sequence, both time pairs, and where its bytes are
events      the event bytes themselves
```

**In the file, an event header's `desc` slot holds a descriptor INDEX rather than a pointer.**
Everything else about the event stream is byte-identical to the live one, so writing is a copy plus one word per event and reading is the same word patched back.
So there is no second decoder to keep in step with the first.

**A summary event's payload is a `rec::summary`, whatever its descriptor's fields say**, so neither direction patches pointers in it.
Its header carries `flag_summary`; that flag, new in version 5, is what a reader keys the event's kind and layout on.

### The compressed envelope

`compress_serialized` wraps a serialized recording for keeping: the magic `CCRECZST`, the recording's size as a u64, then one standard zstd frame of it.
The envelope has no version of its own, since the recording inside carries one.
Both readers recognize it by its magic, so `load_recording`, `deserialize` and `ccrec.load` take either form.
The size caps the decompression of an untrusted file, and a frame that holds more or less than it claims is refused.

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
On a path where nothing is going to run again, nothing else would ever see them.

An arena too small does not fail: it truncates, and the file says so through `loaded_recording::is_truncated()`.
A truncated dump that loads is worth more than a complete one that does not exist.

`cc::rec::write_dump_now(cc::rec::dump_mode::constrained)` runs the identical path on demand.
That is what makes the constrained writer testable at all, rather than something to find out about during a crash.

---

## The module table

A captured stack is addresses, and an address only means something against the module it fell in.
Inside the recording process that is implicit, because the modules are still loaded.
The moment a recording travels — to another machine, or merely past its process's death — it is the missing half, and **a crash dump is always that case**.

So a serialized recording carries the table: for every mapped binary its base, its size, its path, and its identity.
`cc::symbolizer(recording.modules())` resolves against it in a session of its own, loading each module at the base the recording used rather than wherever this process would have put it.

A recorded path that is a UNC path, or sits on a network drive, is deliberately not opened by default.
The debug-info library loads a module's image lazily, inside the first resolve rather than at load time.
So an unreachable path does not fail at load — it costs a network timeout per address, and the table already names the frame without it.
`cc::symbolize_options::load_remote_images` opts back in where the share is known to answer, and rewriting the path to a local copy is the alternative that needs no flag.

**The identity matters as much as the base.**
Two builds of the same path are different binaries, and resolving against the wrong one produces confident nonsense rather than an error.
On Windows it is the PE `TimeDateStamp` and `SizeOfImage` as hex — literally the key a symbol server URL is built from.
On macOS it is the image's LC_UUID, which is what a dSYM is matched on.
macOS resolves such a table to modules and offsets alone, since `dladdr` knows only this process's images; `atos` names the rest offline.

The two writers gather it at different times, for the same reason they differ everywhere else.
`cc::rec::serialize` enumerates the modules when it writes, once per process.
The crash dump enumerates at **install** time and keeps the result, because a crash handler may not allocate.
A module loaded after that is missing from the dump, which is the price of not allocating on the way down.

A recording that carries no table means "this process's own", and symbolizing it falls back to the live modules.

---

## Reading a recording in Python

Record once, query many times: [tools/ccrec/](../../tools/ccrec/__init__.py) decodes a `.ccrec` into Python objects, with the standard library alone.
It mirrors `load_recording`, so it reads everything the C++ loader reads: every table, every event, every payload field by its descriptor's layout, the pinned blobs and the module table.

**It reads exactly one format version, and refuses every other with `ccrec.FormatError`.**
So a version bump here means updating `reader.VERSION` and whatever the bump changed, never a reader that guesses.

```bash
uv run libs/base/clean-core/tools/ccrec-cli.py summary run.ccrec    # threads, and what each recorded
uv run libs/base/clean-core/tools/ccrec-cli.py scopes run.ccrec     # scope names by total time
uv run libs/base/clean-core/tools/ccrec-cli.py events run.ccrec --thread 3
```

A script imports the package from beside the CLI:

```python
import sys
sys.path.insert(0, "path/to/shaped-core/libs/base/clean-core/tools")
import ccrec

rec = ccrec.load("run.ccrec")
a = ccrec.analyze(rec)                         # one pass over every thread's stream
for s in a.scopes_named("upload-pass"):        # intervals: thread, begin/end cycles, depth, parent
    print(s.thread.name, rec.secs(s.duration_cycles), a.scope_path(s.index))
slow = [v for v in a.values if a.scope_path(v.scope).endswith("upload-pass")]   # values, by enclosing scope
busy = ccrec.concurrency(a, ["task"], begin, end)   # how many threads were inside "task", over time
off = a.off_cpu()                              # the record.thread_run stretches a thread mostly sat off the CPU
```

`analyze` also attributes every value, stat, marker and log to the innermost scope open around it, and to the trace and owner in effect.
A sample comes with the sampled thread's scopes open at its time, which name the frames a sample stops short of.
Its addresses stay unsymbolized, and `rec.modules` is the table to resolve them against.
`ccrec.columns` and `ccrec.to_numpy` turn any of the tables into columns, the second for a script that declares numpy itself.

`dev.py check` keeps the reader in step through its `ccrec-reader` gate.
clean-core-test records a fixture of every event kind and payload type, and writes what the C++ loader reads back out of it as a manifest.
[ccrec-self-test.py](../../tools/ccrec-self-test.py) must read the same file to the same manifest, and checks the reader's struct sizes and enums against these headers.

---

## Known gaps

- **Nexus does not install a dump yet.** It belongs with the rest of the nexus wiring, which waits on ambient filtering.
- **A dump is not a minidump.** It carries what was recorded, not the machine state; `cc::install_crash_handler` still owns the stack traces.
- **A block does not record where in its chunk it starts.**
  A sample's anchor is a chunk offset, so it resolves against a file's blocks only where a block is a whole chunk from its start.
  The Python reader places a sample by time instead.
- **Stacktrace events carry return addresses, never names.**
  Symbolizing them needs [the module table](#the-module-table) plus the matching binaries, which is why analysis happens offline and a capture stays cheap.
