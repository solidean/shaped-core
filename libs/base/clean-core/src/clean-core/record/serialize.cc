#include "serialize.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/record/desc.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/impl/serialized_format.hh>
#include <clean-core/record/system.hh>
#include <clean-core/record/writer.hh>
#include <clean-core/streams/impl/native_file.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace cc::primitive_defines;
using cc::rec::impl::dump_builder;

namespace
{
/// This process's modules, taken once.
///
/// Once because a snapshot walks every module in the process and allocates, and a program serializing several
/// recordings would pay that each time for an answer that barely changes.
[[nodiscard]] cc::span<cc::loaded_module const> process_modules()
{
    static cc::vector<cc::loaded_module> const modules = cc::enumerate_loaded_modules();
    return modules;
}
} // namespace

/// The arena the table builder starts with, doubled on overflow.
/// A megabyte covers a few thousand descriptors, which is far more than any run has produced.
constexpr isize initial_arena_bytes = 1 << 20;
constexpr isize max_arena_bytes = 256 << 20;

void append(cc::vector<byte>& out, cc::span<byte const> bytes)
{
    if (bytes.empty())
        return;

    auto const at = out.size();
    out.resize_to_uninitialized(at + bytes.size());
    cc::memcpy(out.data() + at, bytes.data(), size_t(bytes.size()));
}

void append(cc::vector<byte>& out, void const* data, isize size)
{
    append(out, cc::span<byte const>(reinterpret_cast<byte const*>(data), size));
}
} // namespace

/// Rebuilds the owned tables behind a loaded_recording, and is the only thing allowed to touch its privates.
struct cc::rec::impl::recording_loader
{
    static cc::result<rec::loaded_recording> load(cc::span<byte const> file);
};

cc::vector<byte> cc::rec::serialize(cc::rec::recording const& r)
{
    auto arena = cc::vector<byte>();

    for (auto arena_bytes = initial_arena_bytes; arena_bytes <= max_arena_bytes; arena_bytes *= 2)
    {
        arena.resize_to_uninitialized(arena_bytes);

        auto builder = dump_builder(cc::span<byte>(arena));
        builder.set_meta(cc::current_time_wall_secs(), rec::cycles_per_second());

        // What the recording's addresses are relative to.
        // A recording that already carries a table keeps it — it came from a file, and re-snapshotting would replace
        // the modules the addresses actually mean with this process's.
        builder.add_modules(r.modules().empty() ? cc::span<cc::loaded_module const>(process_modules()) : r.modules());

        for (auto const& b : r.blocks())
            if (!builder.add_block(b.view()))
                break;

        // Growing rather than truncating: an in-process serialize has no reason to lose events, unlike a crash dump
        // working against a reservation it cannot extend.
        if (builder.is_overflowed())
            continue;

        auto const parts = builder.finish();

        auto out = cc::vector<byte>();
        out.reserve(isize(parts.header.offset_events + parts.header.total_event_bytes));

        append(out, &parts.header, isize(sizeof(parts.header)));
        append(out, parts.strings);
        append(out, parts.domains);
        append(out, parts.units);
        append(out, parts.relations);
        append(out, parts.fields);
        append(out, parts.descs);
        append(out, parts.threads);
        append(out, parts.modules);
        append(out, parts.blocks);

        for (isize i = 0; i < builder.block_count(); ++i)
        {
            auto const source = builder.block_at(i);

            // The events go out byte-for-byte, with only the descriptor POINTER rewritten into its table INDEX.
            // Keeping the rest identical is what lets the reader patch the same word back rather than run a second
            // decoder that would have to be kept in step with the first.
            auto const at = out.size();
            out.resize_to_uninitialized(at + isize(source.size));
            cc::memcpy(out.data() + at, source.data, size_t(source.size));

            isize offset = 0;
            while (offset < isize(source.size))
            {
                auto* const header = reinterpret_cast<impl::event_header*>(out.data() + at + offset);
                auto const payload = isize(header->payload_size);
                auto const* const original = header->desc;
                auto const index = builder.desc_index_of_pointer(original);

                // The payload's own descriptor references go first: they are read through `original`, which the line
                // below overwrites.
                builder.rewrite_payload_pointers(
                    *original, cc::span<byte>(out.data() + at + offset + isize(sizeof(*header)), payload));

                header->desc = reinterpret_cast<rec::desc const*>(uintptr_t(index));
                offset += impl::event_bytes_for(payload);
            }
        }

        // The pinned payloads last, straight from behind their pins.
        // They are copied HERE rather than into the arena, because pinned data is the one thing in a recording with no
        // bound on its size and the arena is a reservation.
        for (isize i = 0; i < builder.blob_count(); ++i)
        {
            auto const blob = builder.blob_at(i);
            append(out, cc::span<byte const>(blob.data, isize(blob.size)));
        }

        return out;
    }

    return {};
}

cc::result<cc::unit> cc::rec::save_recording(cc::rec::recording const& r, cc::string_view path)
{
    auto const bytes = rec::serialize(r);

    auto file = cc::impl::native_file::open(path, cc::impl::file_mode::write_truncate);
    CC_RETURN_IF_ERROR(file);

    isize written = 0;
    while (written < bytes.size())
    {
        auto n = file.value().write(cc::span<byte const>(bytes.data() + written, bytes.size() - written));
        CC_RETURN_IF_ERROR(n);
        if (n.value() <= 0)
            return cc::error("could not write the whole recording");
        written += n.value();
    }

    return cc::unit{};
}

cc::result<cc::rec::loaded_recording> cc::rec::impl::recording_loader::load(cc::span<byte const> file)
{
    using namespace cc::rec::impl;

    if (file.size() < isize(sizeof(serialized_header)))
        return cc::error("not a recording: too short for a header");

    serialized_header h = {};
    cc::memcpy(&h, file.data(), sizeof(h));

    for (isize i = 0; i < 8; ++i)
        if (h.magic[i] != serialized_magic[i])
            return cc::error("not a recording: bad magic");

    // Refusing rather than guessing: a format with no stability guarantee must never misread an older file.
    if (h.version != rec::serialized_version)
        return cc::error("recording written by a different format version");

    // Every table is bounds-checked before a single entry is read, because the bytes are untrusted the moment they
    // have been through a file.
    auto const in_bounds = [&](u64 offset, u64 count, u64 stride)
    { return offset <= u64(file.size()) && count * stride <= u64(file.size()) - offset; };

    if (!in_bounds(h.offset_strings, h.string_bytes, 1)
        || !in_bounds(h.offset_domains, h.domain_count, sizeof(serialized_domain))
        || !in_bounds(h.offset_units, h.unit_count, sizeof(serialized_unit))
        || !in_bounds(h.offset_relations, h.relation_count, sizeof(serialized_relation_type))
        || !in_bounds(h.offset_fields, h.field_count, sizeof(serialized_field))
        || !in_bounds(h.offset_descs, h.desc_count, sizeof(serialized_desc))
        || !in_bounds(h.offset_threads, h.thread_count, sizeof(serialized_thread))
        || !in_bounds(h.offset_modules, h.module_count, sizeof(serialized_module))
        || !in_bounds(h.offset_blocks, h.block_count, sizeof(serialized_block))
        || !in_bounds(h.offset_events, h.total_event_bytes, 1) || !in_bounds(h.offset_blobs, h.blob_bytes, 1))
        return cc::error("recording is malformed: a table runs past the end of the file");

    auto const* const s_domains = reinterpret_cast<serialized_domain const*>(file.data() + h.offset_domains);
    auto const* const s_units = reinterpret_cast<serialized_unit const*>(file.data() + h.offset_units);
    auto const* const s_relations = reinterpret_cast<serialized_relation_type const*>(file.data() + h.offset_relations);
    auto const* const s_fields = reinterpret_cast<serialized_field const*>(file.data() + h.offset_fields);
    auto const* const s_descs = reinterpret_cast<serialized_desc const*>(file.data() + h.offset_descs);
    auto const* const s_threads = reinterpret_cast<serialized_thread const*>(file.data() + h.offset_threads);
    auto const* const s_modules = reinterpret_cast<serialized_module const*>(file.data() + h.offset_modules);
    auto const* const s_blocks = reinterpret_cast<serialized_block const*>(file.data() + h.offset_blocks);
    auto const* const s_strings = reinterpret_cast<char const*>(file.data() + h.offset_strings);

    auto out = rec::loaded_recording();
    out._dumped_at_wall_secs = h.dumped_at_wall_secs;
    out._cycles_per_second = h.cycles_per_second;
    out._is_truncated = (h.flags & serialized_flag_truncated) != 0;

    // The descriptor fields are `char const*`, so every string has to be re-emitted NUL-terminated, and every
    // pointer handed out points INTO one arena.
    //
    // **That arena is sized exactly and never grows**, because a reallocation would silently dangle every pointer
    // already taken — invisible until something reads one, and then only sometimes.
    //
    // The file's string table is NOT an upper bound on what to reserve: the writer stores each distinct string once,
    // and several descriptors may reference the same one, so the total copied is the sum over REFERENCES rather than
    // over entries.
    // Measuring first is the only way to get that right.
    auto const measure = [&](serialized_str str) { return isize(str.length) + 1; };

    isize string_arena_bytes = 1;
    for (u32 i = 0; i < h.domain_count; ++i)
        string_arena_bytes += measure(s_domains[i].name);
    for (u32 i = 0; i < h.unit_count; ++i)
        string_arena_bytes += measure(s_units[i].singular) + measure(s_units[i].plural) + measure(s_units[i].symbol);
    for (u32 i = 0; i < h.relation_count; ++i)
        string_arena_bytes += measure(s_relations[i].name) + measure(s_relations[i].inverse_name);
    for (u32 i = 0; i < h.field_count; ++i)
        string_arena_bytes += measure(s_fields[i].name);
    for (u32 i = 0; i < h.desc_count; ++i)
        string_arena_bytes
            += measure(s_descs[i].name) + measure(s_descs[i].site_file) + measure(s_descs[i].site_function);
    for (u32 i = 0; i < h.thread_count; ++i)
        string_arena_bytes += measure(s_threads[i].name);
    for (u32 i = 0; i < h.module_count; ++i)
        string_arena_bytes += measure(s_modules[i].path) + measure(s_modules[i].identity);

    out._strings.resize_to_uninitialized(string_arena_bytes);
    isize string_at = 0;

    auto const take_string = [&](serialized_str str) -> char const*
    {
        auto const at = string_at;
        CC_ASSERT(at + isize(str.length) + 1 <= out._strings.size(), "the loader's string arena was under-sized");

        if (str.length != 0 && u64(str.offset) + str.length <= h.string_bytes)
            for (u32 i = 0; i < str.length; ++i)
                out._strings[string_at++] = s_strings[str.offset + i];

        out._strings[string_at++] = '\0';
        return out._strings.data() + at;
    };

    for (u32 i = 0; i < h.domain_count; ++i)
    {
        auto d = cc::make_unique<rec::domain>(take_string(s_domains[i].name));
        d->set_enabled_mask(s_domains[i].enabled_mask);
        out._domains.push_back(cc::move(d));
    }

    for (u32 i = 0; i < h.unit_count; ++i)
    {
        auto const& u = s_units[i];
        out._units.push_back({
            .singular = take_string(u.singular),
            .plural = take_string(u.plural),
            .symbol = take_string(u.symbol),
            .prefix_base = u.prefix_base,
            .scale = rec::axis_scale(u.scale),
            .aggregate = rec::aggregation(u.aggregate),
            .default_min = u.default_min,
            .default_max = u.default_max,
            .higher_is_better = u.higher_is_better != 0,
        });
    }

    for (u32 i = 0; i < h.relation_count; ++i)
    {
        auto const& t = s_relations[i];
        out._relations.push_back({
            .name = take_string(t.name),
            .inverse_name = take_string(t.inverse_name),
            .is_symmetric = t.is_symmetric != 0,
            .is_transitive = t.is_transitive != 0,
            .is_equivalence = t.is_equivalence != 0,
        });
    }

    for (u32 i = 0; i < h.field_count; ++i)
        out._fields.push_back({
            .name = take_string(s_fields[i].name),
            .type = rec::type_code(s_fields[i].type),
            .offset = s_fields[i].offset,
            .size = s_fields[i].size,
        });

    for (u32 i = 0; i < h.desc_count; ++i)
    {
        auto const& d = s_descs[i];
        if (u64(d.field_first) + d.field_count > h.field_count)
            return cc::error("recording is malformed: a descriptor names fields that are not there");
        if (d.unit_index >= i32(h.unit_count) || d.domain_index >= i32(h.domain_count)
            || d.relation_index >= i32(h.relation_count))
            return cc::error("recording is malformed: a descriptor names a unit, domain or relation that is not there");

        out._descs.push_back({
            .kind = rec::event_kind(d.kind),
            .lvl = rec::level(d.level),
            .enable_bit = d.enable_bit,
            .name = take_string(d.name),
            .quantity = d.unit_index >= 0 ? out._units.data() + d.unit_index : nullptr,
            .relation = d.relation_index >= 0 ? out._relations.data() + d.relation_index : nullptr,
            .dom = d.domain_index >= 0 ? out._domains[d.domain_index].get() : &rec::g_default_domain,
            .site = {.file = take_string(d.site_file), .function = take_string(d.site_function), .line = d.site_line},
            .fields = d.field_count > 0 ? out._fields.data() + d.field_first : nullptr,
            .field_count = u16(d.field_count),
            .fixed_payload_size = d.fixed_payload_size,
        });
    }

    // One buffer for every block's events, patched in place, then pinned so the blocks can view into it.
    auto events = cc::vector<byte>();
    events.resize_to_uninitialized(isize(h.total_event_bytes));
    if (h.total_event_bytes > 0)
        cc::memcpy(events.data(), file.data() + h.offset_events, size_t(h.total_event_bytes));

    // Measured by the validating walk below, then used to size the payload string arena exactly once.
    isize payload_string_bytes = 0;

    for (u32 b = 0; b < h.block_count; ++b)
    {
        auto const& sb = s_blocks[b];
        if (sb.event_offset < h.offset_events || sb.event_offset + sb.event_bytes > h.offset_events + h.total_event_bytes)
            return cc::error("recording is malformed: a block points outside the event section");

        auto const at = isize(sb.event_offset - h.offset_events);

        // Walk the block's events, turning each descriptor index back into a pointer.
        isize offset = 0;
        while (offset < isize(sb.event_bytes))
        {
            if (offset + isize(sizeof(impl::event_header)) > isize(sb.event_bytes))
                return cc::error("recording is malformed: an event header runs past its block");

            auto* const header = reinterpret_cast<impl::event_header*>(events.data() + at + offset);
            auto const index = u64(reinterpret_cast<uintptr_t>(header->desc));
            if (index >= h.desc_count)
                return cc::error("recording is malformed: an event names a descriptor that is not there");

            auto const total = impl::event_bytes_for(isize(header->payload_size));
            if (offset + total > isize(sb.event_bytes))
                return cc::error("recording is malformed: an event payload runs past its block");

            header->desc = out._descs.data() + index;

            // The payload's own descriptor references, patched the same way and against the same table.
            // Validated rather than trusted: an index from a file names whatever it likes, and a preamble that points
            // at nothing reads as an unnamed scope, which the format already has a meaning for.
            //
            // A `cstring` slot is left alone for now and measured instead: its bytes need an arena of their own, and
            // that arena's size is exactly what this walk is working out.
            auto const& d = out._descs[isize(index)];
            for (isize f = 0; f < isize(d.field_count); ++f)
            {
                auto const& field = d.fields[f];
                if (isize(field.offset) + isize(sizeof(u64)) > isize(header->payload_size))
                    continue;

                auto* const slot = events.data() + at + offset + isize(sizeof(impl::event_header)) + field.offset;

                if (field.type == rec::type_code::desc_ref)
                {
                    u64 referenced = 0;
                    cc::memcpy(&referenced, slot, sizeof(referenced));

                    rec::desc const* target = referenced < h.desc_count ? out._descs.data() + referenced : nullptr;
                    cc::memcpy(slot, &target, sizeof(target));
                }
                else if (field.type == rec::type_code::cstring)
                {
                    impl::serialized_str str = {};
                    cc::memcpy(&str, slot, sizeof(str));

                    if (u64(str.offset) + str.length <= h.string_bytes)
                        payload_string_bytes += isize(str.length) + 1;
                }
            }

            offset += total;
        }
    }

    // Now that the walk has both validated the stream and measured it, the payload arena can be sized exactly once —
    // the same discipline as `_strings`, and for the same reason: the slots below hold pointers into it.
    out._payload_strings.resize_to_uninitialized(payload_string_bytes + 1);
    isize payload_string_at = 0;

    // The pinned bytes come over whole, so a loaded recording owns what a live one only borrowed.
    out._blobs.resize_to_uninitialized(isize(h.blob_bytes));
    if (h.blob_bytes > 0)
        cc::memcpy(out._blobs.data(), file.data() + h.offset_blobs, size_t(h.blob_bytes));

    for (u32 b = 0; b < h.block_count; ++b)
    {
        auto const& sb = s_blocks[b];
        auto const at = isize(sb.event_offset - h.offset_events);

        isize offset = 0;
        while (offset < isize(sb.event_bytes))
        {
            auto* const header = reinterpret_cast<impl::event_header*>(events.data() + at + offset);
            auto const& d = *header->desc;

            for (isize f = 0; f < isize(d.field_count); ++f)
            {
                auto const& field = d.fields[f];
                if (isize(field.offset) + isize(sizeof(u64)) > isize(header->payload_size))
                    continue;

                auto* const slot = events.data() + at + offset + isize(sizeof(impl::event_header)) + field.offset;

                if (field.type == rec::type_code::pinned_bytes)
                {
                    if (isize(field.offset) + 16 > isize(header->payload_size))
                        continue;

                    u64 blob_offset = 0;
                    u64 size = 0;
                    cc::memcpy(&blob_offset, slot, sizeof(blob_offset));
                    cc::memcpy(&size, slot + 8, sizeof(size));

                    // A slot the file cannot justify becomes null, which `field_as_bytes` already reads as empty.
                    byte const* data = nullptr;
                    if (size > 0 && blob_offset + size <= h.blob_bytes)
                        data = out._blobs.data() + blob_offset;
                    cc::memcpy(slot, &data, sizeof(data));
                    continue;
                }

                if (field.type != rec::type_code::cstring)
                    continue;

                impl::serialized_str str = {};
                cc::memcpy(&str, slot, sizeof(str));

                auto* const text = out._payload_strings.data() + payload_string_at;
                if (u64(str.offset) + str.length <= h.string_bytes)
                {
                    for (u32 i = 0; i < str.length; ++i)
                        out._payload_strings[payload_string_at++] = s_strings[str.offset + i];
                    out._payload_strings[payload_string_at++] = '\0';
                }

                // A slot the file could not justify becomes the empty string rather than an address.
                char const* const written
                    = u64(str.offset) + str.length <= h.string_bytes ? text : out._payload_strings.data();
                cc::memcpy(slot, &written, sizeof(written));
            }

            offset += impl::event_bytes_for(isize(header->payload_size));
        }
    }

    auto const pinned = cc::make_pinned_data(cc::move(events)).reinterpret_as<byte const>();

    // Thread names are taken ONCE per thread, not once per block: there are more blocks than threads, and taking
    // them per block would overrun the string arena's reservation.
    auto thread_names = cc::vector<cc::string>();
    for (u32 i = 0; i < h.thread_count; ++i)
        thread_names.push_back(cc::string(cc::string_view(take_string(s_threads[i].name))));

    for (u32 b = 0; b < h.block_count; ++b)
    {
        auto const& sb = s_blocks[b];
        if (sb.thread_index >= h.thread_count)
            return cc::error("recording is malformed: a block names a thread that is not there");

        auto const& t = s_threads[sb.thread_index];
        auto const at = u32(sb.event_offset - h.offset_events);

        out._events.append_block({
            .owned = pinned,
            .from = at,
            .to = at + sb.event_bytes,
            .thread_id = cc::thread_id(t.tid),
            .thread_index = t.index,
            .thread_name = thread_names[sb.thread_index],
            .chunk_seq = sb.chunk_seq,
            .layer = sb.layer,
            .base_cycles = sb.base_cycles,
            .base_wall_secs = sb.base_wall_secs,
            .seal_cycles = sb.seal_cycles,
            .seal_wall_secs = sb.seal_wall_secs,
        });
    }

    // The modules the addresses are relative to, which is what makes a loaded recording symbolizable at all — the
    // process that gave those addresses meaning is gone by definition.
    // Owned strings rather than arena slices: a module table outlives the file's bytes in every consumer that has one.
    auto modules = cc::vector<cc::loaded_module>();
    modules.reserve(isize(h.module_count));
    for (u32 i = 0; i < h.module_count; ++i)
        modules.push_back({
            .base = s_modules[i].base,
            .size = s_modules[i].size,
            .path = cc::string(cc::string_view(take_string(s_modules[i].path))),
            .identity = cc::string(cc::string_view(take_string(s_modules[i].identity))),
        });
    out._events.set_modules(cc::move(modules));

    return out;
}

cc::result<cc::rec::loaded_recording> cc::rec::deserialize(cc::span<byte const> bytes)
{
    return impl::recording_loader::load(bytes);
}

cc::result<cc::rec::loaded_recording> cc::rec::load_recording(cc::string_view path)
{
    auto file = cc::impl::native_file::open(path, cc::impl::file_mode::read);
    CC_RETURN_IF_ERROR(file);

    auto size = file.value().size();
    CC_RETURN_IF_ERROR(size);

    auto bytes = cc::vector<byte>();
    bytes.resize_to_uninitialized(isize(size.value()));

    isize read = 0;
    while (read < bytes.size())
    {
        auto n = file.value().read(cc::span<byte>(bytes.data() + read, bytes.size() - read));
        CC_RETURN_IF_ERROR(n);
        if (n.value() <= 0)
            break;
        read += n.value();
    }

    if (read != bytes.size())
        return cc::error("the recording file ended early");

    return impl::recording_loader::load(bytes);
}
