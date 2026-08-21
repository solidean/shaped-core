#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/platform/module_table.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/fwd.hh>

// The on-disk layout, and the one builder both writers go through.
//
// There are two writers — cc::rec::serialize, which may allocate, and the crash dump, which may not — and exactly
// one format.
// Sharing the builder is what keeps them from drifting, since only one of the two is easy to test the day something
// changes.
//
// **The builder allocates nothing.** Everything it needs comes out of an arena the caller supplies, which is what
// lets a crash handler run it against memory reserved long before the crash.
// Event bytes are never copied into the arena: a block records where its bytes already live, and the caller writes
// them straight from the chunk.
//
// Every integer is little-endian, which is every platform we build for; a reader on a big-endian machine would have
// to byte-swap and none exists.

namespace cc::rec::impl
{
/// A slice of the string arena.
struct serialized_str
{
    u32 offset = 0;
    u32 length = 0;
};

/// One recording domain.
struct serialized_domain
{
    serialized_str name;
    u32 enabled_mask = 0;
    u32 reserved = 0;
};

/// One unit, by value: analysis is the only thing that reads one, and inlining it keeps the file self-describing.
struct serialized_unit
{
    serialized_str singular;
    serialized_str plural;
    serialized_str symbol;
    u32 prefix_base = 0;
    u8 scale = 0;
    u8 aggregate = 0;
    u8 higher_is_better = 0;
    u8 reserved = 0;
    f64 default_min = 0;
    f64 default_max = 0;
};

/// One relation type, by value, for the same reason a unit is: the file has to say what an edge MEANS.
struct serialized_relation_type
{
    serialized_str name;
    serialized_str inverse_name;
    u8 is_symmetric = 0;
    u8 is_transitive = 0;
    u8 is_equivalence = 0;
    u8 reserved[5] = {};
};

/// One payload field.
struct serialized_field
{
    serialized_str name;
    u16 offset = 0;
    u16 size = 0;
    u8 type = 0;
    u8 reserved[3] = {};
};

/// One recording site.
/// `unit_index` and `domain_index` are -1 when the site named none.
struct serialized_desc
{
    u8 kind = 0;
    u8 level = 0;
    u16 reserved = 0;
    u32 enable_bit = 0;
    serialized_str name;
    i32 unit_index = -1;
    i32 domain_index = -1;
    i32 relation_index = -1;
    i32 reserved2 = 0;
    serialized_str site_file;
    serialized_str site_function;
    u32 site_line = 0;
    u32 field_first = 0;
    u32 field_count = 0;
    u32 fixed_payload_size = 0;
};

/// One recording thread.
struct serialized_thread
{
    u64 tid = 0;
    u32 index = 0;
    u32 reserved = 0;
    serialized_str name;
};

/// One binary that was mapped when the recording was made, and where.
///
/// Without this a recording's addresses mean nothing outside the process that produced them — and a crash dump is
/// exactly a recording whose process is gone.
struct serialized_module
{
    u64 base = 0;
    u64 size = 0;
    serialized_str path;
    serialized_str identity;
};

/// One block of events, and where its bytes sit in the file.
struct serialized_block
{
    u64 chunk_seq = 0;
    u64 base_cycles = 0;
    f64 base_wall_secs = 0;
    u64 seal_cycles = 0;
    f64 seal_wall_secs = 0;
    u64 event_offset = 0;
    u32 event_bytes = 0;
    u32 thread_index = 0;
    u16 layer = 0;
    u16 reserved = 0;
    u32 reserved2 = 0;
};

/// The file header.
/// Every offset is from the start of the file.
struct serialized_header
{
    char magic[8] = {};
    u32 version = 0;
    u32 flags = 0;
    f64 dumped_at_wall_secs = 0;
    f64 cycles_per_second = 0;

    u32 string_bytes = 0;
    u32 domain_count = 0;
    u32 unit_count = 0;
    u32 relation_count = 0;
    u32 field_count = 0;
    u32 desc_count = 0;
    u32 thread_count = 0;
    u32 block_count = 0;
    u32 module_count = 0;

    u64 total_event_bytes = 0;

    u64 offset_strings = 0;
    u64 offset_domains = 0;
    u64 offset_units = 0;
    u64 offset_relations = 0;
    u64 offset_fields = 0;
    u64 offset_descs = 0;
    u64 offset_threads = 0;
    u64 offset_modules = 0;
    u64 offset_blocks = 0;
    u64 offset_events = 0;

    /// The pinned bytes, last in the file because they are the only unbounded section.
    ///
    /// A `pinned_bytes` payload holds an ADDRESS live and an offset into here once written, which is what lets the
    /// event stream stay byte-for-byte the size it was while the bytes themselves travel.
    u64 offset_blobs = 0;
    u64 blob_bytes = 0;
};

// A layout change that does not bump the version is the one mistake this format makes easy.
static_assert(sizeof(serialized_str) == 8);
static_assert(sizeof(serialized_domain) == 16);
static_assert(sizeof(serialized_unit) == 48);
static_assert(sizeof(serialized_field) == 16);
static_assert(sizeof(serialized_relation_type) == 24);
static_assert(sizeof(serialized_desc) == 64);
static_assert(sizeof(serialized_thread) == 24);
static_assert(sizeof(serialized_module) == 32);
static_assert(sizeof(serialized_block) == 64);
static_assert(sizeof(serialized_header) == 176);

/// The eight bytes a reader checks before anything else.
inline constexpr char serialized_magic[8] = {'C', 'C', 'R', 'E', 'C', 'O', 'R', 'D'};

/// Set when the writer hit its byte cap and stopped early.
inline constexpr u32 serialized_flag_truncated = 1 << 0;

/// **In the file, an event header's `desc` slot holds a descriptor INDEX rather than a pointer.**
/// The rest of the event stream is byte-identical to the live one, so a reader copies the bytes and patches that one
/// word per event — no re-encoding, and no second decoder to keep in step with the first.
[[nodiscard]] constexpr u64 desc_index_of(rec::desc const* const* slot)
{
    return u64(reinterpret_cast<uintptr_t>(*slot));
}

/// Builds the tables for one dump into a caller-supplied arena, allocating nothing.
///
/// Fails rather than growing: `is_overflowed()` says the arena was too small, and the caller either retries with a
/// bigger one (serialize) or writes what it has and marks the result truncated (a crash dump).
struct dump_builder
{
    /// One block's bytes, still where they were written.
    struct block_source
    {
        byte const* data = nullptr;
        u32 size = 0;
    };

    /// One pinned payload's bytes, still behind their pin.
    ///
    /// The builder holds the ADDRESS rather than a copy, for the same reason a block does: the arena is a fixed
    /// reservation and pinned data is the one thing in a recording with no bound on its size.
    /// A writer streams these straight from the pin, so a hundred megabytes of them costs the arena sixteen bytes.
    struct blob_source
    {
        byte const* data = nullptr;
        u64 size = 0;
        u64 offset = 0; ///< where it lands in the blob section
    };

    /// The finished file, as the pieces to write in order.
    ///
    /// Deliberately NOT one assembled buffer: the tables are scattered through the arena in build order, and packing
    /// them into a contiguous prefix would mean moving one over another that had not been copied yet.
    /// A writer emits these back to back, then each block's event bytes.
    struct dump_parts
    {
        serialized_header header;
        cc::span<byte const> strings;
        cc::span<byte const> domains;
        cc::span<byte const> units;
        cc::span<byte const> relations;
        cc::span<byte const> fields;
        cc::span<byte const> descs;
        cc::span<byte const> threads;
        cc::span<byte const> modules;
        cc::span<byte const> blocks;
    };

    explicit dump_builder(cc::span<byte> arena);

    void set_meta(f64 dumped_at_wall_secs, f64 cycles_per_second)
    {
        _wall_secs = dumped_at_wall_secs, _rate = cycles_per_second;
    }

    /// Records the modules this recording's addresses are relative to.
    /// Anything past the arena's capacity is dropped, which costs names rather than correctness.
    void add_modules(cc::span<cc::loaded_module const> modules);

    /// Records one block and everything its events reference.
    /// Returns false when the arena ran out, in which case the dump is finished with what it already has.
    bool add_block(rec::chunk_view const& view);

    [[nodiscard]] bool is_overflowed() const { return _overflowed; }
    [[nodiscard]] isize block_count() const { return _blocks_used; }

    /// Finishes the tables and returns the pieces of the file, in write order.
    /// The caller writes those, then each block's bytes, in order, straight from `block_at`.
    [[nodiscard]] dump_parts finish();

    /// Where block `i`'s bytes live.
    /// Valid only after finish().
    [[nodiscard]] block_source block_at(isize i) const { return _block_sources[i]; }

    /// The pinned payloads, in the order they occupy the blob section.
    /// A writer emits these last, after every block's events, straight from the addresses they name.
    [[nodiscard]] isize blob_count() const { return _blobs_used; }
    [[nodiscard]] blob_source blob_at(isize i) const { return _blobs[i]; }

    /// The table index a descriptor was interned at, or -1.
    /// This is what a writer rewrites into each event's descriptor slot.
    [[nodiscard]] i64 desc_index_of_pointer(rec::desc const* d) const;

    /// Rewrites every payload field that holds a POINTER into this process, so the bytes mean something in a file.
    ///
    /// `desc_ref` becomes a descriptor-table index, `cstring` becomes a `serialized_str` into the string table — both
    /// eight bytes, so the event stream stays byte-for-byte the size it was.
    ///
    /// **Call with the event's ORIGINAL descriptor, before its own slot is overwritten** — the layout comes from `d`,
    /// so patching the header first would leave nothing to read the field list off.
    /// A no-op for the overwhelming majority of events, which have no such field.
    void rewrite_payload_pointers(rec::desc const& d, cc::span<byte> payload);

private:
    /// Copies `s` into the string arena once per distinct source pointer.
    serialized_str _intern_string(cc::string_view s);

    /// Interns a value keyed by its address, returning its table index; -1 on overflow.
    i32 _intern_domain(rec::domain const* d);
    i32 _intern_unit(rec::unit const* u);
    i32 _intern_relation(rec::relation_type const* t);
    i32 _intern_desc(rec::desc const* d);
    i32 _intern_thread(rec::thread_info const& t);

    /// Reserves a blob's place in the section, keyed by its address and size; -1 on overflow.
    /// Deduplicated, so two events recording the same pinned data cost the file one copy.
    i64 _intern_blob(byte const* data, u64 size);

    /// Bumps `_used` and hands back zeroed, aligned space; null on overflow.
    byte* _alloc(isize bytes, isize alignment);

    cc::span<byte> _arena;
    isize _used = 0;
    bool _overflowed = false;

    f64 _wall_secs = 0;
    f64 _rate = 0;

    // Every table is a fixed-capacity region carved out of the arena up front, because growing one would mean moving
    // it, and everything already handed out an index into it.
    char* _strings = nullptr;
    isize _string_bytes = 0;
    isize _string_capacity = 0;

    serialized_domain* _domains = nullptr;
    serialized_unit* _units = nullptr;
    serialized_relation_type* _relations = nullptr;
    serialized_field* _fields = nullptr;
    serialized_desc* _descs = nullptr;
    serialized_thread* _threads = nullptr;
    serialized_module* _modules = nullptr;
    serialized_block* _blocks = nullptr;
    block_source* _block_sources = nullptr;
    blob_source* _blobs = nullptr;

    isize _domains_used = 0;
    isize _units_used = 0;
    isize _relations_used = 0;
    isize _fields_used = 0;
    isize _descs_used = 0;
    isize _threads_used = 0;
    isize _modules_used = 0;
    isize _blocks_used = 0;
    isize _blobs_used = 0;
    u64 _blob_bytes = 0;

    isize _domain_capacity = 0;
    isize _unit_capacity = 0;
    isize _relation_capacity = 0;
    isize _field_capacity = 0;
    isize _desc_capacity = 0;
    isize _thread_capacity = 0;
    isize _module_capacity = 0;
    isize _block_capacity = 0;

    // Interning keys, parallel to the tables above.
    // Linear scan: a dump has tens of descriptors, not thousands, and a hash table in an arena would be more machinery
    // than the scan costs.
    void const** _domain_keys = nullptr;
    void const** _unit_keys = nullptr;
    void const** _relation_keys = nullptr;
    void const** _desc_keys = nullptr;
    char const** _string_keys = nullptr;
    serialized_str* _string_values = nullptr;
    isize _string_key_count = 0;
    isize _string_key_capacity = 0;
};
} // namespace cc::rec::impl
