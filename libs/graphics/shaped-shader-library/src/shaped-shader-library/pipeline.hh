#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-shader-library/fwd.hh>

/// A pipeline an SGL `pipeline` declaration states, as slib builds it.
/// The generated symbol of each declaration hands one `pipeline_definition` to the functions below; a caller holds that
/// symbol and never these.
/// libs/graphics/shaped-graphics-language/docs/spec/pipelines.md is what the declaration means.

/// What a setting's value is; the SGL compiler's `check::setting_kind`, which slib does not link publicly.
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

/// One field of a pipeline's description, written once; the settings apply in order, over sg's defaults.
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
    cc::span<pipeline_setting const> settings;

    /// The frozen part as SGL names it, which a reloaded source is held to: the layout's bindings in group order,
    /// the `@inline` binding, the vertex input and the `@pixel struct`; empty where there is none.
    cc::span<cc::string_view const> layout;
    cc::string_view inline_constants;
    cc::string_view vertex_input_name;
    cc::string_view target_struct;
};

/// What a declared pipeline is configured with now, as its source says after the latest reload.
struct slib::pipeline_configuration
{
    /// What `acquire` builds with: the source's settings, or the last ones whose frozen part matched the build.
    cc::vector<pipeline_setting> settings;
    /// The source's newest settings, which `acquire_latest` builds with even where their frozen part moved.
    cc::vector<pipeline_setting> latest;
    /// Empty while the source's frozen part is the one the host was built against; otherwise what moved, one line each.
    cc::string frozen_moved;
};

namespace slib
{
/// Applied to the description last, after the settings and the open parts, on every build.
using pipeline_customize = cc::unique_function<void(sg::raster_pipeline_description&)>;

/// Writes `settings` over `desc` in order; `targets` names `desc.color_targets` by index.
/// An error names the setting a description cannot take: a path sg has no field for, or a case its enum lacks.
/// Every setting the SGL compiler produced takes, so an error means this table and the prelude's mirror have drifted.
[[nodiscard]] cc::result<cc::unit, cc::string> apply_settings(sg::raster_pipeline_description& desc,
                                                              cc::span<pipeline_setting const> settings,
                                                              cc::span<cc::string_view const> targets);

/// The paths `apply_settings` takes, with `*` standing for a target; for the test that pins it to the prelude.
[[nodiscard]] cc::span<cc::string_view const> settable_paths();

/// The enumerator names `apply_settings` maps an sg enum's cases by, in enumerator order; empty for an unknown enum.
[[nodiscard]] cc::span<cc::string_view const> enum_case_names(cc::string_view sg_enum);

/// The description `definition` states, with `open` stated and `customize` applied: its stages compiled for `ctx`.
/// Every open part of the definition must be in `open`, which is held by value since the coroutine runs later.
/// Cold, like every coroutine here: awaiting it is what starts the compiles.
/// `ctx` must outlive the result.
[[nodiscard]] cc::shared_async<sg::raster_pipeline_description> describe_raster_pipeline(sg::context* ctx,
                                                                                         pipeline_definition const* definition,
                                                                                         cc::vector<open_part> open,
                                                                                         pipeline_customize customize);

/// The configuration `definition`'s source states now.
/// Where hot reload moved a stage to a new generation, the source is described again: the configuration then follows
/// it, unless its frozen part moved, which keeps the last configuration that matched and says why in the log.
/// Without a reload it is the build's, and nothing is read.
[[nodiscard]] pipeline_configuration configuration_of(pipeline_definition const& definition);

/// The pipeline `describe_raster_pipeline` describes, built through `ctx.cached`.
/// A reload that moved the frozen part keeps handing out the last pipeline that built for these open parts, while
/// that pipeline is still alive somewhere.
[[nodiscard]] sg::async_raster_pipeline acquire_raster_pipeline(sg::context* ctx,
                                                                pipeline_definition const* definition,
                                                                cc::vector<open_part> open,
                                                                pipeline_customize customize);

/// The pipeline the source's newest settings describe, frozen part and all; for a host that follows a reload itself.
[[nodiscard]] sg::async_raster_pipeline acquire_latest_raster_pipeline(sg::context* ctx,
                                                                       pipeline_definition const* definition,
                                                                       cc::vector<open_part> open,
                                                                       pipeline_customize customize);
} // namespace slib
