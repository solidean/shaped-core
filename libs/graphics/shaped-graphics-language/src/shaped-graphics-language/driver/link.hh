#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-graphics-language/driver/library_file.hh>

/// One entry of a link group: an entry point of a file of the library, built from that file (LINK-1).
struct sgl::link_entry
{
    /// The file, as `library_file::name` names it.
    cc::string_view source_name;
    cc::string_view entry_point;
    /// The stage the host expects; `none` takes whichever it is.
    check::stage stage = check::stage::none;
    /// The values the entry is built with, as `text_request::options` gives them.
    cc::span<check::option_value const> options;
};

/// A group of entries that share one numbering of samplers and callables tables: one pipeline (LINK-1).
struct sgl::link_request
{
    /// What a diagnostic and the map call the group.
    cc::string_view name = "<group>";
    /// The group, in the host's order, which LINK-3 numbers by.
    cc::span<link_entry const> entries;
    /// The files whose entries the group lists, and whose modules they may `use`.
    cc::span<library_file const> library;
    /// The targets the result emits for: each file is checked once for each (CHK-426), beside the check that numbers.
    /// Empty for a result that only maps.
    cc::span<emit::target const> targets;
    /// What the device has, as `text_request::device_features` gives it, for every target alike.
    check::feature_set device_features;
    check::float_level float_floor = check::float_level::fast;
};

/// What a host builds a group's pipeline from (LINK-8): its static samplers, its callables records and its ray set.
/// Plain data; `to_json` writes it.
struct sgl::link_map
{
    struct sampler
    {
        /// Qualified by its module, `samplers.clamp`, or a program file's own by its library path, `trace.sgl:gBorder`.
        cc::string symbol;
        i32 slot = 0;
        /// Its settings, as `describe` gives them; `index` is the slot.
        described_file_sampler settings;
    };
    struct table
    {
        cc::string symbol;
        i32 first_record = 0;
        /// Its callables, qualified as `symbol` is, in record order.
        cc::vector<cc::string> callables;
        /// It ends in `.host`, so the host's callables follow its last record.
        bool has_host = false;
    };
    struct ray
    {
        cc::string name;
        i32 contribution = 0;
        i32 miss = 0;
    };
    cc::string name;
    /// By slot, dense.
    cc::vector<sampler> samplers;
    /// The group's callable section, in order.
    cc::vector<table> tables;
    /// The last table takes `.host`.
    bool has_host_callables = false;
    /// Empty where no entry traces, misses or hits.
    cc::string ray_set;
    /// The ray set's ray count (CHK-329).
    i32 multiplier = 0;
    cc::vector<ray> rays;
    /// The entries whose numbers differ from those their file gives them, as `file:entry`, in the group's listing order:
    /// each is emitted as it is nowhere compiled alone (LINK-4).
    cc::vector<cc::string> renumbered;
};

/// How `link_result::emit` writes an entry; what `text_request` says of the same fields.
struct sgl::link_emit_options
{
    bool line_directives = false;
    bool diagnostics = false;
    check::undefined_guard guards_undefined = check::undefined_guard::none;
    bool software_trace = false;
    bool weakens_float = false;
    cc::span<check::feature_fill const> fills;
    check::core_fills core;
    emit::native_route native = emit::native_route::text;
    emit::debug_info native_debug_info = emit::debug_info::none;
};

/// A linked group: every file it lists checked, and the group's numbering of what its entries reach.
/// Move-only; it owns the checked files, so it outlives the request it was made from.
class sgl::link_result
{
public:
    struct state;

    explicit link_result(cc::unique_ptr<state> s);
    link_result(link_result&& rhs) noexcept;
    link_result& operator=(link_result&& rhs) noexcept;
    ~link_result();

    [[nodiscard]] link_map const& map() const;
    /// How many entries the group lists; an entry is a position in `link_request::entries`.
    [[nodiscard]] isize entry_count() const;
    /// The `file:entry` the map names entry `entry` by.
    [[nodiscard]] cc::string_view entry_name(isize entry) const;
    /// The numbers entry `entry` reaches, its samplers' slots and its tables' first records, hashed (LINK-7).
    /// What it reaches is the union over the check that numbers and every linked target's, branches on the target included.
    /// Two links of one entry that give it equal hashes emit it alike, so a cache keys its binary on the hash.
    /// Stable across processes and platforms.
    [[nodiscard]] u64 numbering_hash(isize entry) const;
    /// Entry `entry` written for `t`, one of `link_request::targets`, with the group's numbering (LINK-6).
    /// Const and checks nothing: entries may be emitted from several threads at once.
    [[nodiscard]] cc::result<emitted_source, cc::string> emit(isize entry,
                                                              emit::target t,
                                                              link_emit_options const& options = {}) const;

private:
    cc::unique_ptr<state> _state;
};

namespace sgl
{
/// Checks each file of the group once, and numbers the samplers and callables tables its entries reach as LINK-3 says.
///
/// Total: a file that does not check, an entry its file does not hold, and each refusal of LINK-9 are the error, as
/// text meant to be read, `<group>: error: link-ray-set-mismatch: ...`.
/// Deterministic, and it reads nothing but its arguments.
[[nodiscard]] cc::result<link_result, cc::string> link(link_request const& request);

/// The map as one JSON object, its fields named as `link_map`'s; deterministic.
[[nodiscard]] cc::string to_json(link_map const& map);
} // namespace sgl
