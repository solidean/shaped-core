#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/context/cached.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-shader-library/fwd.hh>

/// A pipeline an SGL `pipeline` declaration states, as slib describes it.
/// The generated symbol of each declaration hands one `pipeline_definition` to the functions below, and is itself an
/// `sg::raster_pipeline_source`: a caller writes `ctx.cached.acquire_raster_pipeline(shaders::cube.pipeline, …)`.
/// libs/graphics/shaped-graphics-language/docs/spec/pipelines.md is what the declaration means.

/// What a reloaded setting's value is; the SGL compiler's `check::setting_kind`, which slib does not link publicly.
enum class slib::setting_kind : slib::u8
{
    boolean,
    integer,
    real,
    enum_case,
    /// Left to the host: it arrives as an `open_part` at acquire.
    host,
    /// `blend = .none`.
    none,
};

/// One field of a pipeline's description as a reloaded source states it; they apply in order, over sg's defaults.
/// The build's own settings are generated code instead, and never pass through here.
struct slib::pipeline_setting
{
    /// From the description down, with a target's member name where sg has an index: `color_targets.albedo.format`.
    cc::string_view path;
    setting_kind kind = setting_kind::boolean;
    i64 integer = 0;
    f64 real = 0;
    /// The name of an enumerator of the sg enum the field has.
    cc::string_view enum_case;
};

/// A part the pipeline left to the host, as the host states it at acquire.
struct slib::open_part
{
    /// `color_targets.<target>.format`, `depth_stencil_format` or `sample_count`.
    cc::string_view path;
    /// An `sg::pixel_format`'s value, or the sample count.
    i64 value = 0;
};

/// Everything a generated pipeline symbol knows about its declaration.
/// The build wrote it, so it is what the host's own code was built against.
struct slib::pipeline_definition
{
    /// The file and the pipeline's name, for what a diagnostic says.
    cc::string_view file;
    cc::string_view name;
    /// The package's handles, filled when the package is added to a library; `pixel` is null without a pixel stage.
    shader_asset_handle const* vertex = nullptr;
    shader_asset_handle const* pixel = nullptr;
    /// The layout its stages' binding lists state, from their generated group types.
    sg::pipeline_layout_handle (*acquire_layout)(sg::context& ctx) = nullptr;
    sg::vertex_input_layout (*vertex_input)() = nullptr;
    /// The `@pixel struct`'s qualified name and its members, in location order; both empty without a pixel stage.
    cc::string_view target_set;
    cc::span<cc::string_view const> targets;
    /// Writes the build's settings and then `open` over `desc`, as generated field writes.
    /// `open` is in the order the declaration left its parts open, which is the order the generated code reads it in.
    void (*apply)(sg::raster_pipeline_description& desc, cc::span<open_part const> open) = nullptr;
    /// The frozen part as the build saw it: `sgl::described_pipeline::frozen`, one `key = value` line each.
    cc::span<cc::string_view const> frozen;
};

/// What a declared pipeline is configured with now, as its source says after the latest reload.
struct slib::pipeline_configuration
{
    /// What a description builds with: empty while the build's own settings hold, which `apply` writes.
    /// Otherwise the source's settings, or the last ones whose frozen part matched the build.
    cc::optional<cc::vector<pipeline_setting>> settings;
    /// The source's newest settings, which a latest description builds with even where their frozen part moved.
    cc::optional<cc::vector<pipeline_setting>> latest;
    /// Empty while the source's frozen part is the one the host was built against; otherwise what moved, one line each.
    cc::string frozen_moved;
};

namespace slib
{
/// Writes a reloaded source's `settings` over `desc` in order; `targets` names `desc.color_targets` by index.
/// A `.host` setting is skipped: the host's value arrives as an `open_part`.
/// An error names the setting a description cannot take: a path sg has no field for, or a case its enum lacks.
/// Every setting the SGL compiler produced takes, so an error means the generated `impl/pipeline_fields.hh` is stale.
[[nodiscard]] cc::result<cc::unit, cc::string> apply_settings(sg::raster_pipeline_description& desc,
                                                              cc::span<pipeline_setting const> settings,
                                                              cc::span<cc::string_view const> targets);

/// The configuration `definition`'s source states now.
/// Where hot reload moved a stage to a new generation, the source is described again: the configuration then follows
/// it, unless its frozen part moved, which keeps the last configuration that matched and says why in the log.
/// Without a reload it is the build's, and nothing is read.
[[nodiscard]] pipeline_configuration configuration_of(pipeline_definition const& definition);

/// The description `definition` states, with `open` stated and `customize` applied last: its stages compiled for `ctx`.
/// Every open part of the definition must be in `open`, which is held by value since the coroutine runs later.
/// Cold, like every coroutine here: awaiting it is what starts the compiles.
/// `ctx` must outlive the result.
///
/// Where a reload moved the frozen part, the description keeps the stages it was last built with on `ctx` and the
/// configuration that went with them, so the host's code still fits what it draws with.
/// A source whose frozen part moved before it was ever described on `ctx` has nothing to keep, and fails.
/// `latest` builds the newest stages and settings instead, frozen part and all, for a host that follows a reload itself.
[[nodiscard]] cc::shared_async<sg::raster_pipeline_description> describe_raster_pipeline(
    sg::context* ctx,
    pipeline_definition const* definition,
    cc::vector<open_part> open,
    sg::raster_pipeline_customize customize,
    bool latest = false);
} // namespace slib
