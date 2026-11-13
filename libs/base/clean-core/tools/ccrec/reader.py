"""Decodes a `.ccrec` file — clean-core's serialized recording — into Python objects.

The layout mirrors libs/base/clean-core/src/clean-core/record/impl/serialized_format.hh, and the decoding mirrors `load_recording` in serialize.cc.
The format has no stability guarantee, so a file whose magic or version this module does not know is refused with `FormatError`, never guessed at.
libs/base/clean-core/docs/systems/recording-formats.md is the format; libs/base/clean-core/docs/systems/recording.md is the event stream inside it.

Everything is little-endian, and every pointer-valued payload slot is eight bytes whatever the writer's pointer width.
"""

from __future__ import annotations

import enum
import struct
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

#: The one format version this reader understands; `cc::rec::serialized_version` in serialize.hh.
VERSION = 4
MAGIC = b"CCRECORD"

#: `serialized_flag_truncated`: the writer hit its byte cap and stopped early.
FLAG_TRUNCATED = 1 << 0

# Event header flags, from writer.hh.
FLAG_HAS_END_CYCLES = 1 << 0
FLAG_HAS_STACKTRACE = 1 << 1
FLAG_PAYLOAD_PINNED = 1 << 2
FLAG_EVENT_TRUNCATED = 1 << 3

#: `chunk::no_layer`: a block recorded by ordinary code rather than inside a listener.
NO_LAYER = 0xFFFF

#: `sample_unknown_thread`: a sample of a thread the recorder has no stream for.
SAMPLE_UNKNOWN_THREAD = 0xFFFFFFFF

#: `desc::variable_payload`.
VARIABLE_PAYLOAD = 0xFFFFFFFF


class FormatError(Exception):
    """The bytes are not a recording this reader can decode: bad magic, another version, or a malformed table."""


class EventKind(enum.IntEnum):
    """`cc::rec::event_kind`, in declaration order; the self-test compares it against fwd.hh."""

    invalid = 0
    log = 1
    scope_begin = 2
    scope_end = 3
    marker = 4
    value = 5
    stat_snapshot = 6
    stat_accumulate = 7
    ambient_changed = 8
    trace_relation = 9
    async_scope_begin = 10
    async_scope_end = 11
    sample = 12
    stream_state = 13
    gap = 14
    chunk_acquired = 15
    late_event = 16
    dropped_span = 17
    stamp = 18


class Level(enum.IntEnum):
    """`cc::rec::level`."""

    trace = 0
    debug = 1
    info = 2
    warning = 3
    error = 4


class Category(enum.IntEnum):
    """`cc::rec::category`; a non-log site's enable bit is `1 << (16 + category)`."""

    logging = 0
    profiling = 1
    values = 2
    stats = 3
    tracing = 4
    attribution = 5


class TypeCode(enum.IntEnum):
    """`cc::rec::type_code`, how a payload field is laid out."""

    none = 0
    boolean = 1
    i8_ = 2
    i16_ = 3
    i32_ = 4
    i64_ = 5
    u8_ = 6
    u16_ = 7
    u32_ = 8
    u64_ = 9
    f32_ = 10
    f64_ = 11
    pointer = 12
    cstring = 13
    inline_text = 14
    pinned_bytes = 15
    u64_array = 16
    desc_ref = 17


class Aggregation(enum.IntEnum):
    """`cc::rec::aggregation`."""

    sum = 0
    mean = 1
    last = 2
    maximum = 3
    minimum = 4


class AxisScale(enum.IntEnum):
    """`cc::rec::axis_scale`."""

    linear = 0
    logarithmic = 1


# The on-disk structs, each with the size serialized_format.hh static_asserts.
# Padding is spelled out with `x`, since `<` packs with none.
_HEADER = struct.Struct("<8sIIdd9I4xQ10QQQ")
_STR = struct.Struct("<II")
_DOMAIN = struct.Struct("<IIII")
_UNIT = struct.Struct("<6IIBBBBdd")
_RELATION = struct.Struct("<4IBBB5x")
_FIELD = struct.Struct("<IIHHB3x")
_DESC = struct.Struct("<BBHIIIiiiiIIIIIIII")
_THREAD = struct.Struct("<QIIII")
_MODULE = struct.Struct("<QQIIII")
_BLOCK = struct.Struct("<QQdQdQIIHHI")
_EVENT_HEADER = struct.Struct("<QQIHH")

#: The sizes serialized_format.hh and writer.hh pin with static_assert, by C++ struct name.
STRUCT_SIZES = {
    "serialized_str": _STR.size,
    "serialized_domain": _DOMAIN.size,
    "serialized_unit": _UNIT.size,
    "serialized_field": _FIELD.size,
    "serialized_relation_type": _RELATION.size,
    "serialized_desc": _DESC.size,
    "serialized_thread": _THREAD.size,
    "serialized_module": _MODULE.size,
    "serialized_block": _BLOCK.size,
    "serialized_header": _HEADER.size,
    "event_header": _EVENT_HEADER.size,
}

_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")

# How a fixed-width numeric field reads, by type code: the struct to unpack and nothing else.
_NUMERIC = {
    TypeCode.boolean: struct.Struct("<B"),
    TypeCode.i8_: struct.Struct("<b"),
    TypeCode.i16_: struct.Struct("<h"),
    TypeCode.i32_: struct.Struct("<i"),
    TypeCode.i64_: struct.Struct("<q"),
    TypeCode.u8_: struct.Struct("<B"),
    TypeCode.u16_: struct.Struct("<H"),
    TypeCode.u32_: struct.Struct("<I"),
    TypeCode.u64_: struct.Struct("<Q"),
    TypeCode.f32_: struct.Struct("<f"),
    TypeCode.f64_: struct.Struct("<d"),
    # An opaque address, read as eight bytes as event_view::field_as_u64 does.
    TypeCode.pointer: struct.Struct("<Q"),
}


def _padded(size: int) -> int:
    """`padded_payload`: a payload rounded up to the event stream's eight-byte grid."""
    return (size + 7) & ~7


def _decode(raw: bytes) -> str:
    # Lossless: a name that is not UTF-8 still encodes back to the bytes it came from.
    return raw.decode("utf-8", errors="surrogateescape")


@dataclass(frozen=True, slots=True)
class Domain:
    name: str
    enabled_mask: int


@dataclass(frozen=True, slots=True)
class Unit:
    """What a quantity means, carried by value; a unit's custom formatter does not survive a file."""

    singular: str
    plural: str
    symbol: str
    prefix_base: int
    scale: AxisScale | int
    aggregate: Aggregation | int
    higher_is_better: bool
    default_min: float
    default_max: float


@dataclass(frozen=True, slots=True)
class RelationType:
    name: str
    inverse_name: str
    is_symmetric: bool
    is_transitive: bool
    is_equivalence: bool


@dataclass(frozen=True, slots=True)
class Field:
    name: str
    type: TypeCode | int
    offset: int
    size: int


@dataclass(frozen=True, slots=True)
class Desc:
    """One recording site: everything a reader needs to interpret an event that points at it."""

    index: int
    kind: EventKind | int
    level: Level | int
    enable_bit: int
    name: str
    unit: Unit | None
    domain: Domain | None
    relation: RelationType | None
    file: str
    function: str
    line: int
    fields: tuple[Field, ...]
    fixed_payload_size: int

    @property
    def category(self) -> Category | None:
        """The category a non-log site gates on, from its enable bit; None for a log site."""
        bit = self.enable_bit.bit_length() - 1
        if bit < 16 or self.enable_bit != 1 << bit:
            return None
        try:
            return Category(bit - 16)
        except ValueError:
            return None

    @property
    def domain_name(self) -> str:
        # A site naming no domain was loaded against the default one.
        return self.domain.name if self.domain is not None else "default"

    def field(self, name: str) -> Field | None:
        for f in self.fields:
            if f.name == name:
                return f
        return None


@dataclass(frozen=True, slots=True)
class Thread:
    """A recording thread; `index` is unique for the process's life and is what samples and thread_run name."""

    tid: int
    index: int
    name: str


@dataclass(frozen=True, slots=True)
class Module:
    """A binary that was mapped when the recording was made: what a stack address is relative to."""

    base: int
    size: int
    path: str
    identity: str


class Event:
    """One event, borrowing its payload from the file's bytes.

    Fields decode on demand, by the descriptor's layout and exactly as event_view's accessors do.
    """

    __slots__ = ("block", "desc", "cycles", "core", "flags", "payload")

    def __init__(self, block: Block, desc: Desc, cycles: int, core: int, flags: int, payload: memoryview):
        self.block = block
        self.desc = desc
        self.cycles = cycles
        self.core = core
        self.flags = flags
        self.payload = payload

    @property
    def kind(self) -> EventKind | int:
        return self.desc.kind

    @property
    def level(self) -> Level | int:
        return self.desc.level

    @property
    def thread(self) -> Thread:
        return self.block.thread

    @property
    def name(self) -> str:
        """The site's name, or the payload's for a runtime-named site (`CC_RECORD_NAMED`), as event_view::name()."""
        if self.desc.name:
            return self.desc.name
        dynamic = self.field("name")
        return dynamic if isinstance(dynamic, str) else ""

    @property
    def is_truncated(self) -> bool:
        return (self.flags & FLAG_EVENT_TRUNCATED) != 0

    @property
    def wall_secs(self) -> float:
        return self.block.wall_secs_of(self.cycles)

    @property
    def text(self) -> str:
        """A log message as it would print: the payload, or the site's name when the site had no format arguments."""
        if self.payload.nbytes == 0:
            return self.name
        return _decode(bytes(self.payload))

    def field(self, name: str):
        """The named field decoded by its type, or None when the event has no such field or the payload cannot hold it."""
        f = self.desc.field(name)
        return None if f is None else self.block.recording._read_field(f, self.payload)

    def fields(self) -> dict[str, object]:
        """Every field of the payload, decoded."""
        read = self.block.recording._read_field
        return {f.name: read(f, self.payload) for f in self.desc.fields}

    def __repr__(self) -> str:
        kind = self.kind.name if isinstance(self.kind, EventKind) else self.kind
        return f"Event({kind}, {self.name!r}, cycles={self.cycles})"


class Block:
    """One block of one thread's events, with the (cycles, wall) pairs that map its timestamps to seconds."""

    __slots__ = ("recording", "index", "thread", "chunk_seq", "layer", "base_cycles", "base_wall_secs",
                 "seal_cycles", "seal_wall_secs", "file_offset", "_offsets", "_desc_indices", "_cycles")

    def __init__(self, recording: Recording, index: int, thread: Thread, raw: tuple):
        (self.chunk_seq, self.base_cycles, self.base_wall_secs, self.seal_cycles, self.seal_wall_secs,
         self.file_offset, _event_bytes, _thread_index, self.layer, _r, _r2) = raw
        self.recording = recording
        self.index = index
        self.thread = thread
        self._offsets = array("Q")
        self._desc_indices = array("I")
        self._cycles = array("Q")

    def __len__(self) -> int:
        return len(self._offsets)

    def __iter__(self) -> Iterator[Event]:
        data = self.recording._data
        descs = self.recording.descs
        unpack = _EVENT_HEADER.unpack_from
        for at, d in zip(self._offsets, self._desc_indices):
            _, cycles, size, core, flags = unpack(data, at)
            start = at + _EVENT_HEADER.size
            yield Event(self, descs[d], cycles, core, flags, data[start:start + size])

    @property
    def is_listener_layer(self) -> bool:
        return self.layer != NO_LAYER

    def wall_secs_of(self, cycles: int) -> float:
        """Seconds since the Unix epoch, interpolated between the block's two pairs as chunk_view::wall_secs_of does.

        A block that was never sealed falls back to its base pair and the file's cycle rate.
        """
        if self.seal_wall_secs > 0 and self.seal_cycles > self.base_cycles:
            t = (cycles - self.base_cycles) / (self.seal_cycles - self.base_cycles)
            return self.base_wall_secs + t * (self.seal_wall_secs - self.base_wall_secs)
        rate = self.recording.cycles_per_second
        if rate <= 0:
            return self.base_wall_secs
        delta = (cycles - self.base_cycles) & 0xFFFFFFFFFFFFFFFF
        if delta >= 1 << 63:
            delta -= 1 << 64
        return self.base_wall_secs + delta / rate


class Recording:
    """A whole `.ccrec`: the tables, the module table, and the blocks of events.

    Holds the file's bytes, which every event's payload is a view into.
    """

    def __init__(self, data: bytes | bytearray | memoryview):
        self._data = memoryview(data).toreadonly() if not isinstance(data, memoryview) else data
        self._parse()

    @classmethod
    def load(cls, path: str | Path) -> Recording:
        return cls(Path(path).read_bytes())

    # what the header says
    version: int
    flags: int
    dumped_at_wall_secs: float
    cycles_per_second: float

    @property
    def is_truncated(self) -> bool:
        return (self.flags & FLAG_TRUNCATED) != 0

    def secs(self, cycles: int | float) -> float:
        """A span in cycles as seconds, at the rate the recording process measured; 0 when it measured none."""
        return cycles / self.cycles_per_second if self.cycles_per_second > 0 else 0.0

    def events(self) -> Iterator[Event]:
        """Every event, block by block in file order, each block's events in stream order."""
        for b in self.blocks:
            yield from b

    def sorted_events(self) -> list[Event]:
        """Every event ordered by timestamp, ties broken by file order, as the C++ queries order them."""
        indexed = list(enumerate(self.events()))
        indexed.sort(key=lambda p: (p[1].cycles, p[0]))
        return [e for _, e in indexed]

    def event_count(self) -> int:
        return sum(len(b) for b in self.blocks)

    def blocks_of_thread(self, thread_index: int) -> list[Block]:
        return [b for b in self.blocks if b.thread.index == thread_index]

    def thread_by_index(self, thread_index: int) -> Thread | None:
        return self._threads_by_index.get(thread_index)

    # decoding

    def _string(self, offset: int, length: int) -> str:
        # As the loader's take_string: a slice the string table cannot justify reads as empty.
        if length == 0 or offset + length > self._string_bytes:
            return ""
        at = self._string_base + offset
        return _decode(bytes(self._data[at:at + length]))

    def _parse(self) -> None:
        data = self._data
        size = data.nbytes
        if size < _HEADER.size:
            raise FormatError("not a recording: too short for a header")

        h = _HEADER.unpack_from(data, 0)
        magic, version, flags, dumped, rate = h[0:5]
        (string_bytes, domain_count, unit_count, relation_count, field_count, desc_count, thread_count, block_count,
         module_count) = h[5:14]
        total_event_bytes = h[14]
        (off_strings, off_domains, off_units, off_relations, off_fields, off_descs, off_threads, off_modules,
         off_blocks, off_events) = h[15:25]
        off_blobs, blob_bytes = h[25:27]

        if magic != MAGIC:
            raise FormatError("not a recording: bad magic")
        if version != VERSION:
            raise FormatError(f"recording written by format version {version}; this reader understands only version {VERSION}")

        def in_bounds(offset: int, count: int, stride: int) -> bool:
            return offset <= size and count * stride <= size - offset

        if not (in_bounds(off_strings, string_bytes, 1) and in_bounds(off_domains, domain_count, _DOMAIN.size)
                and in_bounds(off_units, unit_count, _UNIT.size) and in_bounds(off_relations, relation_count, _RELATION.size)
                and in_bounds(off_fields, field_count, _FIELD.size) and in_bounds(off_descs, desc_count, _DESC.size)
                and in_bounds(off_threads, thread_count, _THREAD.size) and in_bounds(off_modules, module_count, _MODULE.size)
                and in_bounds(off_blocks, block_count, _BLOCK.size) and in_bounds(off_events, total_event_bytes, 1)
                and in_bounds(off_blobs, blob_bytes, 1)):
            raise FormatError("recording is malformed: a table runs past the end of the file")

        self.version = version
        self.flags = flags
        self.dumped_at_wall_secs = dumped
        self.cycles_per_second = rate
        self._string_base = off_strings
        self._string_bytes = string_bytes
        self._blob_base = off_blobs
        self._blob_bytes = blob_bytes
        s = self._string

        self.domains: list[Domain] = []
        for i in range(domain_count):
            no, nl, mask, _ = _DOMAIN.unpack_from(data, off_domains + i * _DOMAIN.size)
            self.domains.append(Domain(s(no, nl), mask))

        self.units: list[Unit] = []
        for i in range(unit_count):
            so, sl, po, pl, yo, yl, base, scale, agg, hib, _, dmin, dmax = _UNIT.unpack_from(data, off_units + i * _UNIT.size)
            self.units.append(Unit(s(so, sl), s(po, pl), s(yo, yl), base, _enum(AxisScale, scale), _enum(Aggregation, agg),
                                   hib != 0, dmin, dmax))

        self.relations: list[RelationType] = []
        for i in range(relation_count):
            no, nl, io, il, sym, trans, eq = _RELATION.unpack_from(data, off_relations + i * _RELATION.size)
            self.relations.append(RelationType(s(no, nl), s(io, il), sym != 0, trans != 0, eq != 0))

        fields: list[Field] = []
        for i in range(field_count):
            no, nl, offset, fsize, ftype = _FIELD.unpack_from(data, off_fields + i * _FIELD.size)
            fields.append(Field(s(no, nl), _enum(TypeCode, ftype), offset, fsize))
        self.fields = fields

        self.descs: list[Desc] = []
        for i in range(desc_count):
            (kind, level, _, enable_bit, no, nl, unit_i, domain_i, relation_i, _, fo, fl, fno, fnl, line, field_first,
             field_n, fixed) = _DESC.unpack_from(data, off_descs + i * _DESC.size)
            if field_first + field_n > field_count:
                raise FormatError("recording is malformed: a descriptor names fields that are not there")
            if unit_i >= unit_count or domain_i >= domain_count or relation_i >= relation_count:
                raise FormatError("recording is malformed: a descriptor names a unit, domain or relation that is not there")
            self.descs.append(Desc(
                index=i, kind=_enum(EventKind, kind), level=_enum(Level, level), enable_bit=enable_bit, name=s(no, nl),
                unit=self.units[unit_i] if unit_i >= 0 else None,
                domain=self.domains[domain_i] if domain_i >= 0 else None,
                relation=self.relations[relation_i] if relation_i >= 0 else None,
                file=s(fo, fl), function=s(fno, fnl), line=line,
                # The loader keeps the count as a u16, as rec::desc does.
                fields=tuple(fields[field_first:field_first + (field_n & 0xFFFF)]),
                fixed_payload_size=fixed,
            ))

        self.threads: list[Thread] = []
        for i in range(thread_count):
            tid, index, _, no, nl = _THREAD.unpack_from(data, off_threads + i * _THREAD.size)
            self.threads.append(Thread(tid, index, s(no, nl)))
        self._threads_by_index = {t.index: t for t in self.threads}

        self.modules: list[Module] = []
        for i in range(module_count):
            base, msize, po, pl, io, il = _MODULE.unpack_from(data, off_modules + i * _MODULE.size)
            self.modules.append(Module(base, msize, s(po, pl), s(io, il)))

        # The blocks, validated as the loader validates them: every event inside its block, every descriptor index in
        # the table.
        self.blocks: list[Block] = []
        events_end = off_events + total_event_bytes
        for b in range(block_count):
            raw = _BLOCK.unpack_from(data, off_blocks + b * _BLOCK.size)
            event_offset, event_bytes, thread_index = raw[5], raw[6], raw[7]
            if event_offset < off_events or event_offset + event_bytes > events_end:
                raise FormatError("recording is malformed: a block points outside the event section")
            if thread_index >= thread_count:
                raise FormatError("recording is malformed: a block names a thread that is not there")

            block = Block(self, b, self.threads[thread_index], raw)
            at = event_offset
            end = event_offset + event_bytes
            unpack = _EVENT_HEADER.unpack_from
            while at < end:
                if at + _EVENT_HEADER.size > end:
                    raise FormatError("recording is malformed: an event header runs past its block")
                desc_index, cycles, payload_size, _, _ = unpack(data, at)
                if desc_index >= desc_count:
                    raise FormatError("recording is malformed: an event names a descriptor that is not there")
                total = _EVENT_HEADER.size + _padded(payload_size)
                if at + total > end:
                    raise FormatError("recording is malformed: an event payload runs past its block")
                block._offsets.append(at)
                block._desc_indices.append(desc_index)
                block._cycles.append(cycles)
                at += total
            self.blocks.append(block)

    def _read_field(self, f: Field, payload: memoryview):
        """One payload field, decoded by its type — the event_view accessors and the loader's pointer patching in one."""
        n = payload.nbytes
        t = f.type
        numeric = _NUMERIC.get(t)
        if numeric is not None:
            if f.offset + numeric.size > n:
                return None
            v = numeric.unpack_from(payload, f.offset)[0]
            return bool(v) if t == TypeCode.boolean else v

        if t == TypeCode.inline_text:
            if f.offset + 4 > n:
                return None
            length = _U32.unpack_from(payload, f.offset)[0]
            start = f.offset + 4
            if start + length > n:
                return None
            return _decode(bytes(payload[start:start + length]))

        if t == TypeCode.cstring:
            # A string-table slice in the file, which the loader turns into a pointer at an arena of its own.
            # A slice the table cannot justify becomes the empty string, as it does there.
            if f.offset + 8 > n:
                return None
            offset, length = _STR.unpack_from(payload, f.offset)
            if offset + length > self._string_bytes:
                return ""
            at = self._string_base + offset
            return _decode(bytes(self._data[at:at + length]))

        if t == TypeCode.pinned_bytes:
            # An offset into the blob section, then the size; anything the file cannot justify reads as empty.
            if f.offset + 16 > n:
                return b""
            blob_offset, blob_size = struct.unpack_from("<QQ", payload, f.offset)
            if blob_size == 0 or blob_offset + blob_size > self._blob_bytes:
                return b""
            at = self._blob_base + blob_offset
            return bytes(self._data[at:at + blob_size])

        if t == TypeCode.u64_array:
            if f.offset + 4 > n:
                return ()
            count = _U32.unpack_from(payload, f.offset)[0]
            start = f.offset + 4
            if start + count * 8 > n:
                return ()
            return struct.unpack_from(f"<{count}Q", payload, start)

        if t == TypeCode.desc_ref:
            # A descriptor-table index in the file; anything past the table, the writer's -1 included, names nothing.
            if f.offset + 8 > n:
                return None
            index = _U64.unpack_from(payload, f.offset)[0]
            return self.descs[index] if index < len(self.descs) else None

        return None


def _enum(kind: type[enum.IntEnum], value: int):
    """The enum member, or the bare integer for a value this reader's enum does not name."""
    try:
        return kind(value)
    except ValueError:
        return value


def load(path: str | Path) -> Recording:
    """Reads a `.ccrec` file; raises FormatError for anything but this reader's own format version."""
    return Recording.load(path)
