#include "serialized_format.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/record/desc.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/serialize.hh>
#include <clean-core/record/writer.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace cc::primitive_defines;

// How the arena is split between the fixed tables.
// Generous on descriptors, which a long run accumulates; thin on domains and units, of which a program has a handful.
constexpr isize domain_capacity = 128;
constexpr isize unit_capacity = 256;
constexpr isize relation_capacity = 256;
constexpr isize field_capacity = 4096;
constexpr isize desc_capacity = 8192;
constexpr isize thread_capacity = 512;

/// A process maps tens to low hundreds of modules; the cap is generous rather than tuned, and overflowing it costs
/// names for the frames in the modules that did not fit.
constexpr isize module_capacity = 512;

/// Four strings per descriptor is the worst case: name, unit or relation spellings, file and function.
constexpr isize string_key_capacity = desc_capacity * 4;

/// How many distinct pinned payloads one dump records.
/// The table costs 24 bytes each and holds only addresses, so this is generous rather than tuned; overflowing it means
/// the events past that point are dropped rather than written pointing at nothing.
constexpr isize blob_capacity = 4096;

/// Below this the fixed tables alone do not fit, so there is nothing to build into.
constexpr isize min_arena_bytes = 1 << 20;

isize align_up(isize v, isize alignment)
{
    return (v + alignment - 1) / alignment * alignment;
}
} // namespace

cc::rec::impl::dump_builder::dump_builder(cc::span<byte> arena) : _arena(arena)
{
    if (arena.size() < min_arena_bytes)
    {
        _overflowed = true;
        return;
    }

    _domain_capacity = domain_capacity;
    _unit_capacity = unit_capacity;
    _relation_capacity = relation_capacity;
    _field_capacity = field_capacity;
    _desc_capacity = desc_capacity;
    _thread_capacity = thread_capacity;
    _module_capacity = module_capacity;
    _string_key_capacity = string_key_capacity;

    _domains = reinterpret_cast<serialized_domain*>(_alloc(_domain_capacity * isize(sizeof(serialized_domain)), 8));
    _units = reinterpret_cast<serialized_unit*>(_alloc(_unit_capacity * isize(sizeof(serialized_unit)), 8));
    _relations = reinterpret_cast<serialized_relation_type*>(
        _alloc(_relation_capacity * isize(sizeof(serialized_relation_type)), 8));
    _fields = reinterpret_cast<serialized_field*>(_alloc(_field_capacity * isize(sizeof(serialized_field)), 8));
    _descs = reinterpret_cast<serialized_desc*>(_alloc(_desc_capacity * isize(sizeof(serialized_desc)), 8));
    _threads = reinterpret_cast<serialized_thread*>(_alloc(_thread_capacity * isize(sizeof(serialized_thread)), 8));
    _modules = reinterpret_cast<serialized_module*>(_alloc(_module_capacity * isize(sizeof(serialized_module)), 8));
    _blobs = reinterpret_cast<blob_source*>(_alloc(blob_capacity * isize(sizeof(blob_source)), 8));

    _domain_keys = reinterpret_cast<void const**>(_alloc(_domain_capacity * isize(sizeof(void*)), 8));
    _unit_keys = reinterpret_cast<void const**>(_alloc(_unit_capacity * isize(sizeof(void*)), 8));
    _relation_keys = reinterpret_cast<void const**>(_alloc(_relation_capacity * isize(sizeof(void*)), 8));
    _desc_keys = reinterpret_cast<void const**>(_alloc(_desc_capacity * isize(sizeof(void*)), 8));
    _string_keys = reinterpret_cast<char const**>(_alloc(_string_key_capacity * isize(sizeof(char*)), 8));
    _string_values = reinterpret_cast<serialized_str*>(_alloc(_string_key_capacity * isize(sizeof(serialized_str)), 8));

    if (_overflowed)
        return;

    // Whatever remains is split between the block table and the string arena.
    auto const remaining = _arena.size() - _used;
    auto const block_bytes = remaining / 2;
    _block_capacity = block_bytes / isize(sizeof(serialized_block) + sizeof(block_source));

    _blocks = reinterpret_cast<serialized_block*>(_alloc(_block_capacity * isize(sizeof(serialized_block)), 8));
    _block_sources = reinterpret_cast<block_source*>(_alloc(_block_capacity * isize(sizeof(block_source)), 8));

    if (_overflowed)
        return;

    _string_capacity = _arena.size() - _used;
    _strings = reinterpret_cast<char*>(_arena.data() + _used);
    _used = _arena.size();
}

byte* cc::rec::impl::dump_builder::_alloc(isize bytes, isize alignment)
{
    auto const start = align_up(_used, alignment);
    if (start + bytes > _arena.size())
    {
        _overflowed = true;
        return nullptr;
    }

    auto* const p = _arena.data() + start;
    for (isize i = 0; i < bytes; ++i)
        p[i] = byte{};

    _used = start + bytes;
    return p;
}

cc::rec::impl::serialized_str cc::rec::impl::dump_builder::_intern_string(cc::string_view s)
{
    if (s.empty())
        return {};

    // Keyed by the source pointer: a descriptor's strings are static, so identity is enough, and two spellings of the
    // same text simply cost two slots.
    for (isize i = 0; i < _string_key_count; ++i)
        if (_string_keys[i] == s.data())
            return _string_values[i];

    if (_string_key_count >= _string_key_capacity || _string_bytes + s.size() > _string_capacity)
    {
        _overflowed = true;
        return {};
    }

    auto const value = serialized_str{.offset = u32(_string_bytes), .length = u32(s.size())};
    for (isize i = 0; i < s.size(); ++i)
        _strings[_string_bytes + i] = s[i];
    _string_bytes += s.size();

    _string_keys[_string_key_count] = s.data();
    _string_values[_string_key_count] = value;
    ++_string_key_count;
    return value;
}

i32 cc::rec::impl::dump_builder::_intern_domain(rec::domain const* d)
{
    if (d == nullptr)
        return -1;

    for (isize i = 0; i < _domains_used; ++i)
        if (_domain_keys[i] == d)
            return i32(i);

    if (_domains_used >= _domain_capacity)
    {
        _overflowed = true;
        return -1;
    }

    auto const index = _domains_used++;
    _domain_keys[index] = d;
    _domains[index] = {.name = _intern_string(d->name()), .enabled_mask = d->enabled_mask()};
    return i32(index);
}

i32 cc::rec::impl::dump_builder::_intern_unit(rec::unit const* u)
{
    if (u == nullptr)
        return -1;

    for (isize i = 0; i < _units_used; ++i)
        if (_unit_keys[i] == u)
            return i32(i);

    if (_units_used >= _unit_capacity)
    {
        _overflowed = true;
        return -1;
    }

    auto const index = _units_used++;
    _unit_keys[index] = u;
    _units[index] = {
        .singular = _intern_string(u->singular),
        .plural = _intern_string(u->plural),
        .symbol = _intern_string(u->symbol),
        .prefix_base = u->prefix_base,
        .scale = u8(u->scale),
        .aggregate = u8(u->aggregate),
        .higher_is_better = u8(u->higher_is_better ? 1 : 0),
        .default_min = u->default_min,
        .default_max = u->default_max,
    };
    return i32(index);
}

i32 cc::rec::impl::dump_builder::_intern_relation(rec::relation_type const* t)
{
    if (t == nullptr)
        return -1;

    for (isize i = 0; i < _relations_used; ++i)
        if (_relation_keys[i] == t)
            return i32(i);

    if (_relations_used >= _relation_capacity)
    {
        _overflowed = true;
        return -1;
    }

    auto const index = _relations_used++;
    _relation_keys[index] = t;
    _relations[index] = {
        .name = _intern_string(t->name),
        .inverse_name = _intern_string(t->inverse_name),
        .is_symmetric = u8(t->is_symmetric ? 1 : 0),
        .is_transitive = u8(t->is_transitive ? 1 : 0),
        .is_equivalence = u8(t->is_equivalence ? 1 : 0),
    };
    return i32(index);
}

i32 cc::rec::impl::dump_builder::_intern_desc(rec::desc const* d)
{
    if (d == nullptr)
        return -1;

    for (isize i = 0; i < _descs_used; ++i)
        if (_desc_keys[i] == d)
            return i32(i);

    if (_descs_used >= _desc_capacity || _fields_used + isize(d->field_count) > _field_capacity)
    {
        _overflowed = true;
        return -1;
    }

    // Reserved before the field copy, so a descriptor that references itself through nothing still gets one slot.
    auto const index = _descs_used++;
    _desc_keys[index] = d;

    auto const field_first = _fields_used;
    for (u16 f = 0; f < d->field_count; ++f)
    {
        auto const& src = d->fields[f];
        _fields[_fields_used++] = {
            .name = _intern_string(src.name),
            .offset = src.offset,
            .size = src.size,
            .type = u8(src.type),
        };
    }

    _descs[index] = {
        .kind = u8(d->kind),
        .level = u8(d->lvl),
        .enable_bit = d->enable_bit,
        .name = _intern_string(d->name),
        .unit_index = _intern_unit(d->quantity),
        .domain_index = _intern_domain(d->dom),
        .relation_index = _intern_relation(d->relation),
        .site_file = _intern_string(d->site.file),
        .site_function = _intern_string(d->site.function),
        .site_line = d->site.line,
        .field_first = u32(field_first),
        .field_count = d->field_count,
        .fixed_payload_size = d->fixed_payload_size,
    };
    return i32(index);
}

i32 cc::rec::impl::dump_builder::_intern_thread(rec::thread_info const& t)
{
    for (isize i = 0; i < _threads_used; ++i)
        if (_threads[i].tid == u64(t.id) && _threads[i].index == t.index)
            return i32(i);

    if (_threads_used >= _thread_capacity)
    {
        _overflowed = true;
        return -1;
    }

    auto const index = _threads_used++;
    _threads[index] = {.tid = u64(t.id), .index = t.index, .name = _intern_string(t.name)};
    return i32(index);
}

i64 cc::rec::impl::dump_builder::desc_index_of_pointer(rec::desc const* d) const
{
    for (isize i = 0; i < _descs_used; ++i)
        if (_desc_keys[i] == d)
            return i64(i);
    return -1;
}

i64 cc::rec::impl::dump_builder::_intern_blob(byte const* data, u64 size)
{
    for (isize i = 0; i < _blobs_used; ++i)
        if (_blobs[i].data == data && _blobs[i].size == size)
            return i64(_blobs[i].offset);

    if (_blobs_used >= blob_capacity)
    {
        _overflowed = true;
        return -1;
    }

    auto const offset = _blob_bytes;
    _blobs[_blobs_used++] = {.data = data, .size = size, .offset = offset};
    _blob_bytes += size;
    return i64(offset);
}

void cc::rec::impl::dump_builder::rewrite_payload_pointers(rec::desc const& d, cc::span<byte> payload)
{
    for (isize f = 0; f < isize(d.field_count); ++f)
    {
        auto const& field = d.fields[f];
        if (isize(field.offset) + isize(sizeof(u64)) > payload.size())
            continue;

        if (field.type == rec::type_code::desc_ref)
        {
            rec::desc const* target = nullptr;
            cc::memcpy(&target, payload.data() + field.offset, sizeof(target));

            // A null slot stays null, and so does one whose target never made it into the table — a reader treats both
            // as "not named here", which is exactly what a preamble with fewer scopes than depth already means.
            auto const written = u64(target != nullptr ? desc_index_of_pointer(target) : i64(-1));
            cc::memcpy(payload.data() + field.offset, &written, sizeof(written));
        }
        else if (field.type == rec::type_code::cstring)
        {
            char const* text = nullptr;
            cc::memcpy(&text, payload.data() + field.offset, sizeof(text));

            // Into the string table, where the descriptors' own names already live, so a literal recorded a thousand
            // times costs the file one copy.
            auto const written = text != nullptr ? _intern_string(cc::string_view(text)) : serialized_str{};
            cc::memcpy(payload.data() + field.offset, &written, sizeof(written));
        }
        else if (field.type == rec::type_code::pinned_bytes)
        {
            // The size lives in the next eight bytes, which is what the pinned layout declares — the address alone
            // says nothing about how much to copy.
            byte const* data = nullptr;
            u64 size = 0;
            cc::memcpy(&data, payload.data() + field.offset, sizeof(data));
            if (isize(field.offset) + 16 > payload.size())
                continue;
            cc::memcpy(&size, payload.data() + field.offset + 8, sizeof(size));

            auto const offset = data != nullptr && size > 0 ? _intern_blob(data, size) : i64(0);
            auto const written = u64(offset < 0 ? 0 : offset);
            cc::memcpy(payload.data() + field.offset, &written, sizeof(written));
        }
    }
}

void cc::rec::impl::dump_builder::add_modules(cc::span<cc::loaded_module const> modules)
{
    for (auto const& m : modules)
    {
        if (_modules_used >= _module_capacity)
        {
            _overflowed = true;
            return;
        }

        // Dropping a module costs names for the frames inside it, never correctness for the rest — so this fills what
        // it can rather than failing the dump.
        _modules[_modules_used++] = {
            .base = m.base,
            .size = m.size,
            .path = _intern_string(m.path),
            .identity = _intern_string(m.identity),
        };
    }
}

bool cc::rec::impl::dump_builder::add_block(rec::chunk_view const& view)
{
    if (_overflowed || view.bytes.empty())
        return !_overflowed;

    if (_blocks_used >= _block_capacity)
    {
        _overflowed = true;
        return false;
    }

    auto const thread_index = _intern_thread(view.thread);

    // Every descriptor this block references has to make it into the table, or the events pointing at it would be
    // unreadable.
    // An overflow here abandons the block rather than writing a dangling index.
    for (auto it = view.begin(); it != view.end(); ++it)
    {
        auto const e = *it;
        if (_intern_desc(e.desc) < 0)
            return false;

        // Whatever a payload names by POINTER has to make it into a table too, alongside the event's own descriptor.
        // A chunk's preamble names the scopes that were open, and those sites may have no event of their own anywhere
        // in this dump — the whole point of the preamble is that their `scope_begin` is gone.
        for (isize f = 0; f < isize(e.desc->field_count); ++f)
        {
            auto const& field = e.desc->fields[f];
            if (isize(field.offset) + isize(sizeof(u64)) > e.payload.size())
                continue;

            if (field.type == rec::type_code::desc_ref)
            {
                rec::desc const* target = nullptr;
                cc::memcpy(&target, e.payload.data() + field.offset, sizeof(target));
                if (target != nullptr && _intern_desc(target) < 0)
                    return false;
            }
            else if (field.type == rec::type_code::cstring)
            {
                char const* text = nullptr;
                cc::memcpy(&text, e.payload.data() + field.offset, sizeof(text));
                if (text != nullptr)
                {
                    _intern_string(cc::string_view(text));
                    if (_overflowed)
                        return false;
                }
            }
            else if (field.type == rec::type_code::pinned_bytes)
            {
                if (isize(field.offset) + 16 > e.payload.size())
                    continue;

                byte const* data = nullptr;
                u64 size = 0;
                cc::memcpy(&data, e.payload.data() + field.offset, sizeof(data));
                cc::memcpy(&size, e.payload.data() + field.offset + 8, sizeof(size));

                if (data != nullptr && size > 0 && _intern_blob(data, size) < 0)
                    return false;
            }
        }
    }

    if (_overflowed || thread_index < 0)
        return false;

    auto const index = _blocks_used++;
    _blocks[index] = {
        .chunk_seq = view.chunk_seq,
        .base_cycles = view.base_cycles,
        .base_wall_secs = view.base_wall_secs,
        .seal_cycles = view.seal_cycles,
        .seal_wall_secs = view.seal_wall_secs,
        .event_bytes = u32(view.bytes.size()),
        .thread_index = u32(thread_index),
        .layer = view.layer,
    };
    _block_sources[index] = {.data = view.bytes.data(), .size = u32(view.bytes.size())};
    return true;
}

cc::rec::impl::dump_builder::dump_parts cc::rec::impl::dump_builder::finish()
{
    // The string arena is padded to eight bytes so that every table after it lands aligned; each table's entry size
    // is already a multiple of eight, so nothing else needs padding.
    while ((_string_bytes % 8) != 0 && _string_bytes < _string_capacity)
        _strings[_string_bytes++] = '\0';

    auto const strings_at = isize(sizeof(serialized_header));
    auto const domains_at = strings_at + _string_bytes;
    auto const units_at = domains_at + _domains_used * isize(sizeof(serialized_domain));
    auto const relations_at = units_at + _units_used * isize(sizeof(serialized_unit));
    auto const fields_at = relations_at + _relations_used * isize(sizeof(serialized_relation_type));
    auto const descs_at = fields_at + _fields_used * isize(sizeof(serialized_field));
    auto const threads_at = descs_at + _descs_used * isize(sizeof(serialized_desc));
    auto const modules_at = threads_at + _threads_used * isize(sizeof(serialized_thread));
    auto const blocks_at = modules_at + _modules_used * isize(sizeof(serialized_module));
    auto const events_at = blocks_at + _blocks_used * isize(sizeof(serialized_block));

    // Only the final layout knows where a block's bytes land, so the offsets are filled in here.
    auto offset = u64(events_at);
    u64 total_event_bytes = 0;
    for (isize i = 0; i < _blocks_used; ++i)
    {
        _blocks[i].event_offset = offset;
        offset += _blocks[i].event_bytes;
        total_event_bytes += _blocks[i].event_bytes;
    }

    auto parts = dump_parts{};
    parts.header = {
        .version = rec::serialized_version,
        .flags = _overflowed ? serialized_flag_truncated : 0u,
        .dumped_at_wall_secs = _wall_secs,
        .cycles_per_second = _rate,
        .string_bytes = u32(_string_bytes),
        .domain_count = u32(_domains_used),
        .unit_count = u32(_units_used),
        .relation_count = u32(_relations_used),
        .field_count = u32(_fields_used),
        .desc_count = u32(_descs_used),
        .thread_count = u32(_threads_used),
        .block_count = u32(_blocks_used),
        .module_count = u32(_modules_used),
        .total_event_bytes = total_event_bytes,
        .offset_strings = u64(strings_at),
        .offset_domains = u64(domains_at),
        .offset_units = u64(units_at),
        .offset_relations = u64(relations_at),
        .offset_fields = u64(fields_at),
        .offset_descs = u64(descs_at),
        .offset_threads = u64(threads_at),
        .offset_modules = u64(modules_at),
        .offset_blocks = u64(blocks_at),
        .offset_events = u64(events_at),

        // After the events, because it is the one section with no bound on its size — so a reader that only wants the
        // stream never has to seek past it.
        .offset_blobs = u64(events_at) + total_event_bytes,
        .blob_bytes = _blob_bytes,
    };
    for (isize i = 0; i < 8; ++i)
        parts.header.magic[i] = serialized_magic[i];

    auto const as_bytes
        = [](void const* p, isize n) { return cc::span<byte const>(reinterpret_cast<byte const*>(p), n); };
    parts.strings = as_bytes(_strings, _string_bytes);
    parts.domains = as_bytes(_domains, _domains_used * isize(sizeof(serialized_domain)));
    parts.units = as_bytes(_units, _units_used * isize(sizeof(serialized_unit)));
    parts.relations = as_bytes(_relations, _relations_used * isize(sizeof(serialized_relation_type)));
    parts.fields = as_bytes(_fields, _fields_used * isize(sizeof(serialized_field)));
    parts.descs = as_bytes(_descs, _descs_used * isize(sizeof(serialized_desc)));
    parts.threads = as_bytes(_threads, _threads_used * isize(sizeof(serialized_thread)));
    parts.modules = as_bytes(_modules, _modules_used * isize(sizeof(serialized_module)));
    parts.blocks = as_bytes(_blocks, _blocks_used * isize(sizeof(serialized_block)));
    return parts;
}
