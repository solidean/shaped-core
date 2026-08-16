#include <clean-core/common/endian.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <versioned-document-file/impl/snapshot_codec.hh>
#include <versioned-document/op.hh>
#include <versioned-document/value.hh>

#include <algorithm>

/// The layout is a header of four intern tables, then the document as indices into them.
///
/// Everything is u32 little-endian, matching the value codec's own length prefixes rather than introducing a varint
/// the repo has nowhere else.
/// At a few million properties a u32 writer index costs a handful of megabytes more than a varint would, against a new
/// primitive and a new class of decoder edge case — and compression sits above this anyway.

namespace vdoc::file::impl
{
namespace
{
using namespace cc::primitive_defines;

using snapshot_error = snapshot_decode_error;

void put_u32(cc::vector<byte>& out, u32 v)
{
    auto const at = out.size();
    out.resize_to_uninitialized(at + 4);
    cc::store_bytes_le<u32>(out, at, v);
}

void put_bytes(cc::vector<byte>& out, cc::span<byte const> bytes)
{
    auto const at = out.size();
    out.resize_to_uninitialized(at + bytes.size());
    cc::memcpy(out.data() + at, bytes.data(), size_t(bytes.size()));
}

void put_string(cc::vector<byte>& out, cc::string_view s)
{
    put_u32(out, u32(s.size()));
    put_bytes(out, cc::span<byte const>((byte const*)s.data(), s.size()));
}

/// Orders two names by their canonical bytes, which is the order every id level is stored in.
/// Spelled out because cc::string_view's own order is not promised to be the byte order an id commits to.
[[nodiscard]] i32 compare_names(cc::string_view a, cc::string_view b)
{
    auto const n = cc::min(a.size(), b.size());
    for (isize i = 0; i < n; ++i)
        if (a[i] != b[i])
            return u8(a[i]) < u8(b[i]) ? -1 : 1;

    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

/// Collects the distinct names of one id level, ascending by canonical bytes.
/// The index a name gets is its position here, which is why the order is part of the format rather than a convenience.
template <class IdT>
[[nodiscard]] cc::vector<cc::string_view> intern_names(cc::vector<IdT> const& ids)
{
    auto names = cc::vector<cc::string_view>();
    for (auto const& id : ids)
        names.push_back(id.as_string_view());

    std::sort(names.begin(), names.end(), [](cc::string_view a, cc::string_view b) { return compare_names(a, b) < 0; });

    auto unique = cc::vector<cc::string_view>();
    for (auto const& n : names)
        if (unique.empty() || unique.back() != n)
            unique.push_back(n);

    return unique;
}

[[nodiscard]] u32 index_of_name(cc::vector<cc::string_view> const& table, cc::string_view name)
{
    auto const it = std::lower_bound(table.begin(), table.end(), name,
                                     [](cc::string_view a, cc::string_view b) { return compare_names(a, b) < 0; });
    return u32(it - table.begin());
}

/// Reads the header's fixed-width fields, refusing anything that runs off the end.
struct cursor
{
    cc::span<byte const> data;
    isize at = 0;

    [[nodiscard]] bool has(isize n) const { return at + n <= data.size(); }

    [[nodiscard]] cc::result<u32, snapshot_error> u32_value()
    {
        if (!has(4))
            return cc::error(snapshot_error::truncated);

        auto const v = cc::load_bytes_le<u32>(data, at);
        at += 4;
        return v;
    }

    [[nodiscard]] cc::result<cc::span<byte const>, snapshot_error> byte_run(isize n)
    {
        if (n < 0 || !has(n))
            return cc::error(snapshot_error::truncated);

        auto const out = data.subspan({.offset = at, .size = n});
        at += n;
        return out;
    }
};

/// Reads one ascending, deduplicated name table.
[[nodiscard]] cc::result<cc::vector<cc::string_view>, snapshot_error> read_name_table(cursor& c)
{
    auto const count = c.u32_value();
    CC_RETURN_IF_ERROR(count);

    auto out = cc::vector<cc::string_view>();
    for (u32 i = 0; i < count.value(); ++i)
    {
        auto const len = c.u32_value();
        CC_RETURN_IF_ERROR(len);
        auto const bytes = c.byte_run(isize(len.value()));
        CC_RETURN_IF_ERROR(bytes);

        auto const name = cc::string_view((char const*)bytes.value().data(), bytes.value().size());
        if (!out.empty())
        {
            auto const order = compare_names(out.back(), name);
            if (order == 0)
                return cc::error(snapshot_error::duplicate_table_entry);
            if (order > 0)
                return cc::error(snapshot_error::unsorted_table);
        }

        out.push_back(name);
    }

    return out;
}
} // namespace

cc::vector<byte> encode_snapshot(vdoc::raw_document const& doc)
{
    // ---- the four intern tables ---------------------------------------------------------------------------------
    auto writers = cc::vector<vdoc::op_id>();
    auto entity_ids = cc::vector<vdoc::entity_id>();
    auto component_ids = cc::vector<vdoc::component_type_id>();
    auto property_ids = cc::vector<vdoc::property_id>();

    for (auto const& e : doc.entities)
    {
        entity_ids.push_back(e.entity);
        for (auto const& c : e.value.components)
        {
            component_ids.push_back(c.component);
            for (auto const& p : c.value.properties)
            {
                property_ids.push_back(p.property);
                for (auto const& w : p.value.writers)
                    writers.push_back(w.writer);
            }
        }
    }

    std::sort(writers.begin(), writers.end(), vdoc::op_id::by_bytes{});
    auto unique_writers = cc::vector<vdoc::op_id>();
    for (auto const& w : writers)
        if (unique_writers.empty() || !(unique_writers.back() == w))
            unique_writers.push_back(w);

    auto const entity_names = intern_names(entity_ids);
    auto const component_names = intern_names(component_ids);
    auto const property_names = intern_names(property_ids);

    auto const writer_index = [&](vdoc::op_id const& id)
    {
        auto const it = std::lower_bound(unique_writers.begin(), unique_writers.end(), id, vdoc::op_id::by_bytes{});
        return u32(it - unique_writers.begin());
    };

    auto out = cc::vector<byte>();

    put_u32(out, u32(unique_writers.size()));
    for (auto const& w : unique_writers)
    {
        auto const at = out.size();
        out.resize_to_uninitialized(at + vdoc::op_id::byte_size);
        w.to_bytes(cc::span<byte>(out).subspan({.offset = at, .size = vdoc::op_id::byte_size}));
    }

    for (auto const* table : {&entity_names, &component_names, &property_names})
    {
        put_u32(out, u32(table->size()));
        for (auto const& n : *table)
            put_string(out, n);
    }

    // ---- the document, as indices -------------------------------------------------------------------------------
    put_u32(out, u32(doc.entities.size()));
    for (auto const& e : doc.entities)
    {
        put_u32(out, index_of_name(entity_names, e.entity.as_string_view()));
        put_u32(out, u32(e.value.components.size()));

        for (auto const& c : e.value.components)
        {
            put_u32(out, index_of_name(component_names, c.component.as_string_view()));
            put_u32(out, u32(c.value.properties.size()));

            for (auto const& p : c.value.properties)
            {
                put_u32(out, index_of_name(property_names, p.property.as_string_view()));
                put_u32(out, u32(p.value.writers.size()));

                for (auto const& w : p.value.writers)
                {
                    put_u32(out, writer_index(w.writer));

                    // The value carries its own EXTENT, exactly as an assignment does.
                    // vdoc::try_decode rejects trailing bytes, so a value can only be validated against a slice that
                    // is already known to be exactly it — and encoded_size may not be asked before that validation.
                    put_u32(out, u32(w.value.bytes().size()));

                    // The bytes go in verbatim: they are already canonical, and re-encoding them would be a second
                    // encoder to keep in agreement with the first.
                    put_bytes(out, w.value.bytes());
                }
            }
        }
    }

    return out;
}

cc::result<vdoc::snapshot_document, snapshot_decode_error> try_decode_snapshot(cc::string_view encoding,
                                                                               cc::span<byte const> data)
{
    if (encoding != snapshot_encoding_v1)
        return cc::error(snapshot_error::unknown_encoding);

    auto c = cursor{.data = data};

    auto const writer_count = c.u32_value();
    CC_RETURN_IF_ERROR(writer_count);

    auto writers = cc::vector<vdoc::op_id>();
    for (u32 i = 0; i < writer_count.value(); ++i)
    {
        auto const bytes = c.byte_run(vdoc::op_id::byte_size);
        CC_RETURN_IF_ERROR(bytes);

        auto const id = vdoc::op_id::from_bytes(bytes.value());
        if (!writers.empty())
        {
            auto const order = writers.back().compare_bytes(id);
            if (order == 0)
                return cc::error(snapshot_error::duplicate_table_entry);
            if (order > 0)
                return cc::error(snapshot_error::unsorted_table);
        }

        writers.push_back(id);
    }

    auto const entity_names = read_name_table(c);
    CC_RETURN_IF_ERROR(entity_names);
    auto const component_names = read_name_table(c);
    CC_RETURN_IF_ERROR(component_names);
    auto const property_names = read_name_table(c);
    CC_RETURN_IF_ERROR(property_names);

    // The arena is the whole stored payload, so a value_view can point straight into it and the decode copies once.
    auto arena = cc::vector<byte>::create_copy_of(data);
    auto doc = vdoc::raw_document();

    auto const entity_count = c.u32_value();
    CC_RETURN_IF_ERROR(entity_count);

    for (u32 ei = 0; ei < entity_count.value(); ++ei)
    {
        auto const name_ix = c.u32_value();
        CC_RETURN_IF_ERROR(name_ix);
        if (name_ix.value() >= entity_names.value().size())
            return cc::error(snapshot_error::index_out_of_range);

        auto entry
            = vdoc::raw_document::entry{.entity = vdoc::entity_id::of(entity_names.value()[isize(name_ix.value())])};
        if (!doc.entities.empty() && doc.entities.back().entity.compare_bytes(entry.entity) >= 0)
            return cc::error(doc.entities.back().entity == entry.entity ? snapshot_error::duplicate_entry
                                                                        : snapshot_error::unsorted_entries);

        auto const component_count = c.u32_value();
        CC_RETURN_IF_ERROR(component_count);

        for (u32 ci = 0; ci < component_count.value(); ++ci)
        {
            auto const c_ix = c.u32_value();
            CC_RETURN_IF_ERROR(c_ix);
            if (c_ix.value() >= component_names.value().size())
                return cc::error(snapshot_error::index_out_of_range);

            auto component = vdoc::raw_entity::entry{
                .component = vdoc::component_type_id::of(component_names.value()[isize(c_ix.value())])};
            if (!entry.value.components.empty()
                && entry.value.components.back().component.compare_bytes(component.component) >= 0)
                return cc::error(entry.value.components.back().component == component.component
                                     ? snapshot_error::duplicate_entry
                                     : snapshot_error::unsorted_entries);

            auto const property_count = c.u32_value();
            CC_RETURN_IF_ERROR(property_count);

            for (u32 pi = 0; pi < property_count.value(); ++pi)
            {
                auto const p_ix = c.u32_value();
                CC_RETURN_IF_ERROR(p_ix);
                if (p_ix.value() >= property_names.value().size())
                    return cc::error(snapshot_error::index_out_of_range);

                auto property = vdoc::raw_component::entry{
                    .property = vdoc::property_id::of(property_names.value()[isize(p_ix.value())])};
                if (!component.value.properties.empty()
                    && component.value.properties.back().property.compare_bytes(property.property) >= 0)
                    return cc::error(component.value.properties.back().property == property.property
                                         ? snapshot_error::duplicate_entry
                                         : snapshot_error::unsorted_entries);

                auto const w_count = c.u32_value();
                CC_RETURN_IF_ERROR(w_count);

                for (u32 wi = 0; wi < w_count.value(); ++wi)
                {
                    auto const w_ix = c.u32_value();
                    CC_RETURN_IF_ERROR(w_ix);
                    if (w_ix.value() >= writers.size())
                        return cc::error(snapshot_error::index_out_of_range);

                    auto const size = c.u32_value();
                    CC_RETURN_IF_ERROR(size);

                    auto const at = c.at;
                    auto const value_bytes = c.byte_run(isize(size.value()));
                    CC_RETURN_IF_ERROR(value_bytes);

                    // Validated against exactly its own extent, because try_decode rejects trailing bytes — the same
                    // reason an assignment carries one.
                    if (vdoc::try_decode(value_bytes.value()).has_error())
                        return cc::error(snapshot_error::invalid_value);

                    // The view points into the ARENA, which outlives this call; `data` does not.
                    auto const view = vdoc::value_view::from_validated_bytes(
                        cc::span<byte const>(arena).subspan({.offset = at, .size = isize(size.value())}));

                    auto const& writer = writers[isize(w_ix.value())];
                    if (!property.value.writers.empty() && property.value.writers.back().writer.compare_bytes(writer) >= 0)
                        return cc::error(property.value.writers.back().writer == writer
                                             ? snapshot_error::duplicate_entry
                                             : snapshot_error::unsorted_entries);

                    property.value.writers.push_back({.writer = writer, .value = view});
                }

                component.value.properties.push_back(cc::move(property));
            }

            entry.value.components.push_back(cc::move(component));
        }

        doc.entities.push_back(cc::move(entry));
    }

    if (c.at != data.size())
        return cc::error(snapshot_error::trailing_bytes);

    return vdoc::snapshot_document::create_from_owned_arena(cc::move(arena), cc::move(doc));
}
} // namespace vdoc::file::impl
