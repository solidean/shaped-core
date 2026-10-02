#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Pin the C++ sgl_host_code.py writes for an SGL group's resources, fed a describe entry directly.

No shader package in the repo reaches every arm: a comparison sampler, a clamped LOD, a cube texture or an integer
texture each appear in some package or none, so a regression in an arm nobody uses would pass the build.
The cases below feed `sgl describe`'s JSON shape as plain dicts, which needs no compiler and no build.

Run by hand as `uv run libs/graphics/shaped-shader-library/cmake/sgl-host-code-self-test.py`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import sgl_description  # noqa: E402
import sgl_host_code  # noqa: E402
from sgl_description import SglFile  # noqa: E402

REPO = Path(__file__).resolve().parents[4]

# sg's typed view typedefs, one per sg::texture_view_dimension: what view_shape must name for every dimension.
VIEWS_HH = REPO / "libs" / "graphics" / "shaped-graphics" / "src" / "shaped-graphics" / "resource" / "views.hh"

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def expect_equal(got, wanted, what: str) -> None:
    if got != wanted:
        raise AssertionError(f"{what}\n    got    {got}\n    wanted {wanted}")


def expect_in(needle: str, haystack: str, what: str) -> None:
    if needle not in haystack:
        raise AssertionError(f"{what}: '{needle}' is missing from\n{haystack}")


def expect_not_in(needle: str, haystack: str, what: str) -> None:
    if needle in haystack:
        raise AssertionError(f"{what}: '{needle}' must not appear in\n{haystack}")


# ---- sampler_initializer --------------------------------------------------------------------------------------------


@test
def a_full_sampler_state_follows_sg_samplers_field_order():
    # Given out of order on purpose: a designated initializer must follow the declaration, whatever the JSON's order.
    state = {"compare": "less_equal", "max_lod": 4.5, "min_lod": 1, "max_anisotropy": 8, "mip_lod_bias": -0.25,
             "address_w": "mirror", "address_v": "clamp_edge", "address_u": "clamp_edge",
             "mip_filter": "nearest", "mag_filter": "nearest", "min_filter": "linear"}
    expect_equal(sgl_host_code.sampler_initializer(state),
                 "{.min_filter = sg::sampler_filter::linear, .mag_filter = sg::sampler_filter::nearest, "
                 ".mip_filter = sg::sampler_filter::nearest, .address_u = sg::sampler_address_mode::clamp_edge, "
                 ".address_v = sg::sampler_address_mode::clamp_edge, .address_w = sg::sampler_address_mode::mirror, "
                 ".mip_lod_bias = -0.25f, .max_anisotropy = 8u, .min_lod = 1.0f, .max_lod = 4.5f, "
                 ".compare = sg::compare_op::less_equal}",
                 "every field, in sg::sampler's order")


@test
def a_comparison_sampler_names_its_compare_op():
    got = sgl_host_code.sampler_initializer({"compare": "greater"})
    expect_equal(got, "{.compare = sg::compare_op::greater}", "the compare arm")


@test
def a_sampler_that_compares_nothing_writes_no_compare():
    got = sgl_host_code.sampler_initializer({"min_filter": "nearest"})
    expect_not_in(".compare", got, "a sampler with no compare key")


@test
def an_integer_json_number_is_still_a_float_literal():
    # `0f` is no C++ literal, so an int from the JSON must come out as `0.0f`.
    got = sgl_host_code.sampler_initializer({"mip_lod_bias": 0, "min_lod": 2, "max_lod": 7})
    expect_equal(got, "{.mip_lod_bias = 0.0f, .min_lod = 2.0f, .max_lod = 7.0f}", "integer LOD values")


@test
def a_max_lod_below_lod_max_is_written_as_a_number():
    got = sgl_host_code.sampler_initializer({"max_lod": 3.0e38})
    expect_equal(got, "{.max_lod = 3e+38f}", "a large max_lod that is still a clamp")


@test
def an_unclamped_max_lod_is_sg_samplers_own_sentinel():
    got = sgl_host_code.sampler_initializer({"max_lod": 3.4028234663852886e38})
    expect_equal(got, "{.max_lod = sg::sampler::lod_max}", "FLT_MAX as max_lod")


# ---- view_shape -----------------------------------------------------------------------------------------------------


@test
def every_texture_view_dimension_names_sgs_own_typedefs():
    text = VIEWS_HH.read_text(encoding="utf-8")
    aliases = dict(re.findall(r"using (tv_\w+) = texture_view_traits<texture_view_dimension::(\w+)>;", text))
    textures = dict(re.findall(r"using texture_view_(\w+) = texture_view<(tv_\w+)>;", text))
    images = dict(re.findall(r"using image_view_(\w+) = image_view<(tv_\w+), Format>;", text))
    if len(aliases) < 9 or len(textures) < 9 or len(images) < 5:
        raise AssertionError(f"read {len(aliases)} tv_ alias(es), {len(textures)} texture and {len(images)} image "
                             f"typedef(s) from {VIEWS_HH} -- the pattern is stale")
    for alias, dimension in aliases.items():
        shape = sgl_host_code.view_shape({"texture_dimension": dimension})
        expect_equal(textures.get(shape), alias, f"texture_view_{shape} for '{dimension}'")
        if "cube" not in dimension and "ms" not in dimension:
            expect_equal(images.get(shape), alias, f"image_view_{shape} for '{dimension}'")


# ---- binding_entry --------------------------------------------------------------------------------------------------


def resource(kind: str, **fields) -> dict:
    return {"kind": kind, "name": "r", "host_name": "g_r", "slot": 3, **fields}


@test
def a_texture_entry_carries_its_sample_type():
    for sample_type in ("filterable_float", "unfilterable_float", "depth", "sint", "uint"):
        got = sgl_host_code.binding_entry(resource("texture", texture_dimension="cube", sample_type=sample_type))
        expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::texture, '
                          ".texture_dimension = sg::texture_view_dimension::cube, "
                          f".sample_type = sg::texture_sample_type::{sample_type}}}",
                     f"a texture of sample type '{sample_type}'")


@test
def an_image_entry_carries_its_format_and_access():
    got = sgl_host_code.binding_entry(resource("image", texture_dimension="tex_2d_array", image_format="r32_uint",
                                               access="write"))
    expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::image, '
                      ".access = sg::access_mode::write, .texture_dimension = sg::texture_view_dimension::tex_2d_array, "
                      ".image_format = sg::pixel_format::r32_uint}",
                 "an image")


@test
def a_buffer_entry_states_access_only_when_written():
    got = sgl_host_code.binding_entry(resource("buffer", access="read"))
    expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::buffer}', "a read buffer")
    got = sgl_host_code.binding_entry(resource("buffer", access="read_write"))
    expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::buffer, '
                      ".access = sg::access_mode::read_write}",
                 "a mut buffer")


@test
def a_sampler_entry_carries_its_sampler_type():
    for sampler_type in ("filtering", "non_filtering", "comparison"):
        got = sgl_host_code.binding_entry(resource("sampler", sampler_type=sampler_type))
        expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::sampler, '
                          f".sampler_type = sg::sampler_binding_type::{sampler_type}}}",
                     f"a sampler of type '{sampler_type}'")


# ---- a group's fields and tables ------------------------------------------------------------------------------------


GROUP = {
    "name": "shadow",
    "inline": False,
    "members": [
        {"kind": "texture", "name": "depth_map", "type": "texture_cube[float]", "host_name": "shadow_depth_map",
         "slot": 0, "texture_dimension": "cube", "sample_type": "depth"},
        {"kind": "image", "name": "counts", "type": "image_3d[uint]", "host_name": "shadow_counts", "slot": 1,
         "texture_dimension": "tex_3d", "image_format": "r32_uint", "access": "read_write"},
        {"kind": "buffer", "name": "weights", "type": "float", "host_name": "shadow_weights", "slot": 4,
         "access": "read_write"},
        {"kind": "sampler", "name": "picked", "type": "sampler", "host_name": "shadow_picked", "slot": 2,
         "sampler_type": "non_filtering"},
        {"kind": "sampler", "name": "compare", "type": "sampler", "host_name": "shadow_compare", "slot": 3,
         "sampler_type": "comparison", "static_sampler": {"min_filter": "linear", "compare": "less"}},
    ],
}

FILE = SglFile(path="shadow.sgl")


@test
def a_group_has_a_field_per_view_and_per_dynamic_sampler():
    header = sgl_host_code.emit_group("pkg", {}, "ns", FILE, GROUP)
    expect_in("sg::texture_view_cube depth_map;", header, "a cube texture's field")
    expect_in("sg::image_view_3d<sg::pixel_format::r32_uint> counts;", header, "a 3d image's field, typed on its format")
    expect_in("sg::readwrite_buffer_view<float> weights;", header, "a mut buffer's field, its view by access")
    expect_in("sg::sampler picked;", header, "a dynamic sampler's field")
    # A static sampler is the layout's, so the group the host fills has nothing to set for it.
    expect_not_in(" compare;", header, "a static sampler")


@test
def a_groups_static_sampler_is_declared_and_its_dynamic_one_gathered():
    source = sgl_host_code.emit_group_impl("pkg", {}, "ns", FILE, GROUP)
    expect_in('{.name = "shadow_compare", .sampler = {.min_filter = sg::sampler_filter::linear, '
              ".compare = sg::compare_op::less}}", source, "the static sampler's table entry")
    expect_in("return k_sgl_samplers_shadow;", source, "declared_samplers")
    expect_in('samplers.push_back({.name = "shadow_picked", .sampler = picked});', source, "the dynamic sampler")
    expect_not_in('.sampler = compare}', source, "a static sampler gathered as a dynamic one")
    expect_in("views.reserve(3);", source, "only views are gathered as views")


ARRAYS = {
    "name": "materials",
    "inline": False,
    "members": [
        {"kind": "texture", "name": "albedo", "type": "texture_2d[float4]", "host_name": "materials_albedo",
         "slot": 0, "count": 8, "texture_dimension": "tex_2d", "sample_type": "filterable_float"},
        {"kind": "buffer", "name": "params", "type": "float4", "host_name": "materials_params", "slot": 8,
         "count": 2, "access": "read"},
    ],
}


@test
def a_binding_array_is_a_fixed_array_of_views_gathered_as_one_binding():
    header = sgl_host_code.emit_group("pkg", {}, "ns", FILE, ARRAYS)
    expect_in("cc::fixed_array<sg::texture_view_2d, 8> albedo;", header, "a texture array's field")
    expect_in("cc::fixed_array<sg::readonly_buffer_view<tg::vec4f>, 2> params;", header, "a buffer array's field")
    source = sgl_host_code.emit_group_impl("pkg", {}, "ns", FILE, ARRAYS)
    expect_in('{.name = "materials_albedo", .index = 0u, .count = 8u, .type = sg::binding_type::texture', source,
              "a binding array's count")
    expect_in("        for (auto const& element : albedo)\n"
              "            elements.push_back(element);\n"
              "        views.push_back({.slot = sg::binding_slot(0), .view = cc::move(elements)});\n", source,
              "every element gathered, in order")
    # sg keys a view by its position among the group's bindings, which is 1 here although `params` starts at register 8
    expect_in("        views.push_back({.slot = sg::binding_slot(1), .view = cc::move(elements)});\n", source,
              "a binding after an array keyed by its position, not its register")


# ---- a vertex input ---------------------------------------------------------------------------------------------------

MESH = {
    "name": "mesh_vertex",
    "edge": "vertex",
    "members": [
        {"name": "position", "type": "float3", "location": 0, "stream": "per_vertex", "per_instance": False,
         "semantic": "POSITION"},
        {"name": "color", "type": "float4", "location": 1, "stream": "per_vertex", "per_instance": False,
         "format": "rgba8_unorm", "semantic": "COLOR"},
        {"name": "material", "type": "uint", "location": 2, "stream": "per_vertex", "per_instance": False,
         "semantic": "MATERIAL"},
    ],
}


@test
def a_packed_vertex_member_is_its_bytes_on_the_host_and_its_format_in_the_layout():
    header = sgl_host_code.emit_vertex_input("pkg", "ns", FILE, MESH)
    # four bytes the host writes, whatever the shader reads them as
    expect_in("cc::u32 color;", header, "a packed member's host field")
    expect_in("cc::u32 material;", header, "a uint member's host field")
    source = sgl_host_code.emit_vertex_input_impl("pkg", "ns", FILE, MESH)
    expect_in(".format = sg::vertex_attribute_format::rgba8_unorm", source, "a packed member's format")
    expect_in(".format = sg::vertex_attribute_format::u32", source, "an integer member's format, from its type")
    expect_in(".format = sg::vertex_attribute_format::vec3f", source, "a float member's format, from its type")


# ---- a pipeline -------------------------------------------------------------------------------------------------------

# Every stage filled, and a vertex stage that draws from no vertex buffer.
TESSELLATED = {
    "name": "tessellated",
    "vertex": "vs",
    "tessellation_control": "tc",
    "tessellation_evaluation": "te",
    "geometry": "gs",
    "pixel": "ps",
    "vertex_input": "",
    "target_set": "",
    "targets": [],
    "layout": ["shadow"],
    "inline": "",
    "open": [],
    "settings": [],
    "frozen": ["layout = shadow@0", "inline constants = ", "vertex input = ", "target set = ",
               "stages = vs, tc, te, gs, ps", "features = geometry_shader, tessellation_shader"],
}


@test
def a_pipeline_names_every_stage_in_pipeline_definitions_field_order():
    entries = sgl_description.SglEntries(bindings=[(FILE, GROUP)], pipelines=[(FILE, TESSELLATED)])
    source = sgl_host_code.emit_pipelines_impl("pkg", "ns", entries, {FILE.path: "shadow"}, {})
    # a designated initializer follows the declaration, which is not the order a vertex passes the stages
    fields = [".vertex = &ns::shadow.vs,", ".pixel = &ns::shadow.ps,", ".geometry = &ns::shadow.gs,",
              ".tessellation_control = &ns::shadow.tc,", ".tessellation_evaluation = &ns::shadow.te,"]
    positions = []
    for f in fields:
        expect_in(f, source, "a stage of the pipeline")
        positions.append(source.index(f))
    expect_equal(positions, sorted(positions), "the stages in pipeline_definition's field order")
    expect_in('    "stages = vs, tc, te, gs, ps",\n', source, "the stages frozen, in the order a vertex passes them")
    expect_not_in(".vertex_input", source, "a vertex stage that draws from no vertex buffer")
    header = sgl_host_code.emit_pipelines(entries, {FILE.path: "shadow"})
    expect_in("vs, tc, te, gs and ps, writing depth alone", header, "the stages the doc comment names")


# ---- a file-scope sampler -------------------------------------------------------------------------------------------

EDGE = {"name": "edge", "index": 1, "sampler_type": "non_filtering", "shape": "0",
        "settings": {"min_filter": "nearest", "address_u": "clamp_edge"}}
EDGE_ROW = ('{.binding = {.name = "edge", .space = slib::bound_samplers_space, .index = 1u, .count = 1u, '
            ".type = sg::binding_type::sampler, .sampler_type = sg::sampler_binding_type::non_filtering},")


@test
def an_entry_points_layout_holds_the_file_samplers_its_code_reaches():
    entry = {"name": "cs", "stage": "compute", "bindings": ["shadow"], "samplers": ["edge"]}
    entries = sgl_description.SglEntries(bindings=[(FILE, GROUP)],
                                         described_entry_points={(FILE.path, "cs"): entry},
                                         file_samplers={FILE.path: [EDGE]})
    header = sgl_host_code.emit_entry_wrappers(entries, {FILE.path: "shadow"})
    expect_in(EDGE_ROW, header, "the sampler's binding, at its index in slib's space")
    expect_in(".sampler = {.min_filter = sg::sampler_filter::nearest, "
              ".address_u = sg::sampler_address_mode::clamp_edge}}", header, "the sampler's settings")
    expect_in("return ctx.cached.acquire_pipeline_layout<shadow>(samplers);", header, "the layout takes them")
    expect_in("<shaped-shader-library/binding/binding_groups.hh>", " ".join(sgl_host_code.includes(entries)),
              "slib::bound_samplers_space's header")


@test
def a_pipelines_layout_holds_the_file_samplers_any_stage_reaches():
    file = SglFile(path="shadow.sgl", samplers=[EDGE])
    pipeline = {**TESSELLATED, "samplers": ["edge"]}
    entries = sgl_description.SglEntries(bindings=[(file, GROUP)], pipelines=[(file, pipeline)])
    source = sgl_host_code.emit_pipelines_impl("pkg", "ns", entries, {file.path: "shadow"}, {})
    expect_in("sg::bound_sampler const k_shadow_tessellated_samplers[] = {\n    " + EDGE_ROW, source, "the table")
    expect_in("acquire_pipeline_layout<ns::shadow>(k_shadow_tessellated_samplers);", source, "the layout takes it")
    # a pipeline that reaches none passes none
    plain = sgl_host_code.emit_pipelines_impl("pkg", "ns", sgl_description.SglEntries(
        bindings=[(file, GROUP)], pipelines=[(file, TESSELLATED)]), {file.path: "shadow"}, {})
    expect_in("acquire_pipeline_layout<ns::shadow>();", plain, "no sampler")


# ---- options --------------------------------------------------------------------------------------------------------

OPTIONS = [{"name": "tile", "type": "int", "value": "8"}, {"name": "sharpen", "type": "bool", "value": "false"},
           {"name": "output_format", "type": "pixel_format", "value": ".rgba16_float"}]


@test
def a_files_options_are_one_struct_at_the_sources_defaults_with_a_modules_nested():
    options = OPTIONS + [{"name": "common.taps", "type": "int", "value": "4"},
                         {"name": "mode", "type": "blur_mode", "value": ".box"}]
    header = sgl_host_code.options_struct("pkg", "shadow.sgl", "shadow_options", options)
    expect_in("struct shadow_options\n{\n    int tile = 8;", header, "an int option, defaulted")
    expect_in("    bool sharpen = false;", header, "every option of the file, whoever reaches it")
    expect_in("    sg::pixel_format output_format = sg::pixel_format::rgba16_float;", header, "a format option")
    expect_in("    struct\n    {\n        int taps = 4;", header, "a module's option nests under the module")
    expect_in("    } common;\n", header, "the member named after the module")
    expect_in('slib::option_of("common.taps", common.taps)', header, "set by its qualified name")
    # an option the generator has no C++ type for is left out, and refused only where a wrapper reaches it
    expect_not_in("mode", header, "an enum option of the file's own")
    try:
        sgl_host_code.options_struct("pkg", "shadow.sgl", "shadow_options",
                                     OPTIONS + [{"name": "common", "type": "int", "value": "1"},
                                                {"name": "common.taps", "type": "int", "value": "4"}])
        raise AssertionError("an option named like a module whose options nest under that name generated a struct")
    except sgl_host_code.HostCodeError as e:
        expect_in("common", str(e), "the refusal names the clash")


@test
def an_entry_point_takes_its_files_options_and_states_the_ones_it_reaches():
    entry = {"name": "cs", "stage": "compute", "bindings": ["shadow"], "options": ["tile", "output_format"]}
    entries = sgl_description.SglEntries(bindings=[(FILE, GROUP)],
                                         described_entry_points={(FILE.path, "cs"): entry},
                                         file_options={FILE.path: OPTIONS},
                                         option_structs={FILE.path: "shadow_options"})
    header = sgl_host_code.emit_entry_wrappers(entries, {FILE.path: "shadow"}, "pkg")
    expect_in('inline constexpr cc::string_view shadow_cs_t_reached_options[] = {"tile", "output_format"};', header,
              "the names it reaches, in the order reached")
    expect_in("    using options = shadow_options;", header, "the wrapper names its file's struct")
    expect_in("static constexpr cc::span<cc::string_view const> reached_options = shadow_cs_t_reached_options;", header,
              "the wrapper names its table")
    expect_in("acquire_compute_pipeline(&ctx, asset, acquire_layout(ctx, values), values.values());", header,
              "the compute pipeline takes them")
    expect_in("<shaped-shader-library/compiler/shader_compiler.hh>", " ".join(sgl_host_code.includes(entries)),
              "slib::option_of's header")
    # one reaching none takes the struct too, so a host hands one value to every wrapper of the file
    other = {"name": "plain", "stage": "compute", "bindings": ["shadow"], "options": []}
    entries.described_entry_points[(FILE.path, "plain")] = other
    header = sgl_host_code.emit_entry_wrappers(entries, {FILE.path: "shadow"}, "pkg")
    expect_in("struct shadow_plain_t\n{\n    /// The options of shadow.sgl, which every wrapper of the file takes.\n"
              "    using options = shadow_options;\n", header, "a wrapper reaching no option")
    expect_in("static constexpr cc::span<cc::string_view const> reached_options = {};", header, "an empty table")


@test
def a_reached_name_the_file_does_not_declare_or_cannot_type_is_refused():
    for reached, wanted in (("missing", "does not list"), ("mode", "no C++ type")):
        entry = {"name": "cs", "stage": "compute", "bindings": ["shadow"], "options": [reached]}
        entries = sgl_description.SglEntries(
            bindings=[(FILE, GROUP)], described_entry_points={(FILE.path, "cs"): entry},
            file_options={FILE.path: OPTIONS + [{"name": "mode", "type": "blur_mode", "value": ".box"}]},
            option_structs={FILE.path: "shadow_options"})
        try:
            sgl_host_code.emit_entry_wrappers(entries, {FILE.path: "shadow"}, "pkg")
            raise AssertionError(f"a wrapper reaching '{reached}' generated")
        except sgl_host_code.HostCodeError as e:
            expect_in(wanted, str(e), f"the refusal of '{reached}'")


@test
def a_pipelines_options_ride_in_its_open_parts():
    file = SglFile(path="shadow.sgl", options=OPTIONS)
    pipeline = {**TESSELLATED, "options": ["sharpen"]}
    entries = sgl_description.SglEntries(bindings=[(file, GROUP)], pipelines=[(file, pipeline)],
                                         option_structs={file.path: "shadow_options"})
    header = sgl_host_code.emit_pipelines(entries, {file.path: "shadow"}, "pkg")
    expect_in("    shadow_options options;\n};", header, "the file's struct, a member of the open parts")
    expect_in("reached_options = shadow_tessellated_t_reached_options;", header, "the names its stages reach")
    source = sgl_host_code.emit_pipelines_impl("pkg", "ns", entries, {file.path: "shadow"}, {})
    expect_in("cc::move(customize), false, parts.options.values());", source, "the description takes them")


FORMATTED = {
    "name": "upscaled",
    "inline": False,
    "options": ["output_format"],
    "members": [
        {"kind": "buffer", "name": "weights", "type": "float", "host_name": "upscaled_weights", "slot": 0,
         "access": "read"},
        {"kind": "image", "name": "output", "type": "out image_2d[output_format]", "host_name": "upscaled_output",
         "slot": 1, "texture_dimension": "tex_2d", "image_format": "rgba16_float", "format_option": "output_format",
         "access": "write"},
    ],
}
FORMATTED_FILE = SglFile(path="upscale.sgl", options=OPTIONS)


@test
def an_image_whose_format_is_an_option_is_taken_format_erased_and_its_layout_per_value():
    header = sgl_host_code.emit_group("pkg", {}, "ns", FORMATTED_FILE, FORMATTED, "upscale_options")
    expect_in("    using options = upscale_options;", header, "the group names its file's struct")
    expect_in("reached_options = upscaled_reached_options;", header, "the formats it follows, by name")
    expect_in("    sg::any_texture_view<sg::tv_2d> output; ///< `out image_2d[output_format]`, an image of the format "
              "`output_format` names", header, "the image's field, its format taken at run time")
    expect_in("[[nodiscard]] static cc::vector<sg::binding> declared_bindings(options const& values);", header,
              "the bindings per set of values")
    expect_in("[[nodiscard]] static cc::span<sg::binding const> declared_bindings();", header,
              "the defaults' bindings, which keep it a declared_binding_set")
    try:
        sgl_host_code.emit_group("pkg", {}, "ns", FORMATTED_FILE, FORMATTED)
        raise AssertionError("a group following options generated without its file's options struct")
    except sgl_host_code.HostCodeError as e:
        expect_in("options struct", str(e), "the refusal says what is missing")
    source = sgl_host_code.emit_group_impl("pkg", {}, "ns", FORMATTED_FILE, FORMATTED)
    expect_in("cc::vector<sg::binding> ns::upscaled::declared_bindings(options const& values)\n{\n"
              "    auto bindings = cc::vector<sg::binding>::create_copy_of(k_sgl_bindings_upscaled);\n"
              "    bindings[1].image_format = values.output_format;\n"
              "    return bindings;\n}\n", source, "the image's entry takes the value's format")
    expect_in(".image_format = sg::pixel_format::rgba16_float}", source, "the defaults' table")


@test
def an_entry_point_reaching_a_groups_format_option_builds_its_layout_from_the_values():
    entry = {"name": "cs", "stage": "compute", "bindings": ["upscaled", "shadow"],
             "options": ["tile", "output_format"]}
    shadow_here = SglFile(path=FORMATTED_FILE.path)
    entries = sgl_description.SglEntries(bindings=[(FORMATTED_FILE, FORMATTED), (shadow_here, GROUP)],
                                         described_entry_points={(FORMATTED_FILE.path, "cs"): entry},
                                         file_options={FORMATTED_FILE.path: OPTIONS},
                                         option_structs={FORMATTED_FILE.path: "upscale_options"})
    header = sgl_host_code.emit_entry_wrappers(entries, {FORMATTED_FILE.path: "upscale"}, "pkg")
    expect_in("acquire_layout(sg::context& ctx, options const& values = {}) const", header, "the layout takes values")
    expect_in("        desc.groups.push_back(\n"
              "            ctx.cached.acquire_binding_group_layout(upscaled::declared_bindings(values), "
              "upscaled::declared_samplers()));\n", header, "the optioned group's layout, from the file's values")
    expect_in("        desc.groups.push_back(ctx.cached.acquire_binding_group_layout<shadow>());\n", header,
              "a plain group's layout, as declared")
    expect_in("acquire_compute_pipeline(&ctx, asset, acquire_layout(ctx, values), values.values());", header,
              "the pipeline's layout follows its values")
    expect_in("<shaped-graphics/binding/pipeline_layout.hh>", " ".join(sgl_host_code.includes(entries)),
              "sg::pipeline_layout_description's header")


@test
def another_modules_binding_in_a_layout_per_value_asks_its_type_whether_it_is_inline():
    entry = {"name": "cs", "stage": "compute", "bindings": ["upscaled", "common.lights"],
             "options": ["output_format"]}
    entries = sgl_description.SglEntries(bindings=[(FORMATTED_FILE, FORMATTED)],
                                         described_entry_points={(FORMATTED_FILE.path, "cs"): entry},
                                         file_options={FORMATTED_FILE.path: OPTIONS},
                                         option_structs={FORMATTED_FILE.path: "upscale_options"})
    header = sgl_host_code.emit_entry_wrappers(entries, {FORMATTED_FILE.path: "upscale"}, "pkg")
    expect_in("        if constexpr (sg::declared_inline_constants<::sgl_modules::common::lights>)\n"
              "            desc.inline_constants = ::sgl_modules::common::lights::inline_binding();\n"
              "        else\n"
              "            desc.groups.push_back(ctx.cached.acquire_binding_group_layout<::sgl_modules::common::lights>());\n",
              header, "the module's binding, inline or a group by its own generated type")
    expect_in("upscaled::declared_bindings(values)", header, "the optioned group beside it, from the values")


@test
def a_binding_array_sized_by_an_option_and_a_pipeline_over_an_optioned_group_are_refused():
    counted = {**ARRAYS, "options": ["layers"],
               "members": [{**ARRAYS["members"][0], "count_option": "layers"}, ARRAYS["members"][1]]}
    try:
        sgl_host_code.emit_group("pkg", {}, "ns", FILE, counted)
        raise AssertionError("a binding array sized by an option generated a type")
    except sgl_host_code.HostCodeError as e:
        expect_in("layers", str(e), "the refusal names the option")
    pipeline = {**TESSELLATED, "layout": ["upscaled"]}
    entries = sgl_description.SglEntries(bindings=[(FORMATTED_FILE, FORMATTED)],
                                         pipelines=[(FORMATTED_FILE, pipeline)])
    try:
        sgl_host_code.emit_pipelines_impl("pkg", "ns", entries, {FORMATTED_FILE.path: "upscale"}, {})
        raise AssertionError("a pipeline over a group with an option format generated a layout")
    except sgl_host_code.HostCodeError as e:
        expect_in("upscaled", str(e), "the refusal names the group")


@test
def an_inline_block_naming_an_option_is_refused_in_words_of_its_own():
    block = {"name": "push", "inline": True, "options": ["tile"], "block_size": 4,
             "members": [{"kind": "constant", "name": "scale", "type": "float", "offset": 0, "size": 4}]}
    try:
        sgl_host_code.emit_inline("pkg", {}, "ns", FILE, block)
        raise AssertionError("an inline block naming an option generated a type")
    except sgl_host_code.HostCodeError as e:
        expect_in("`@inline binding push` names the option(s) tile", str(e), "the inline block's own refusal")
        expect_not_in("binding array", str(e), "the binding array's refusal")


@test
def an_options_struct_is_a_generated_name_like_any_other():
    entries = sgl_description.SglEntries(option_structs={"shadow.sgl": "shadow_options"})
    try:
        sgl_host_code.check_names("pkg", entries, {"shadow_options": "`binding shadow_options` of 'other.sgl'"})
        raise AssertionError("an options struct took a name already generated")
    except sgl_host_code.HostCodeError as e:
        expect_in("shadow_options", str(e), "the clash names the struct")


# ---- the runner -----------------------------------------------------------------------------------------------------


@test
def a_16_bit_value_is_its_host_type_and_a_2_byte_gap_a_u16():
    members = [
        {"name": "weight", "type": "float", "offset": 0, "size": 4},
        {"name": "tint", "type": "half3", "offset": 4, "size": 6},
        {"name": "count", "type": "ushort", "offset": 12, "size": 2},
        {"name": "dir", "type": "float", "offset": 16, "size": 4},
    ]
    fields = sgl_host_code.padded_fields("p", {}, "here", members, "")
    expect_in("tg::vec<3, tg::f16> tint; ///< `half3`, at byte 4\n", fields, "a half3 is typed-geometry's")
    expect_in("cc::u16 _pad0 = {}; ///< 2 bytes SGL's layout leaves free\n", fields, "a 2-byte gap")
    expect_in("cc::u16 count;", fields, "a ushort is clean-core's")
    expect_in("cc::u16 _pad1 = {}; ///< 2 bytes", fields, "a gap of 2 after a ushort")


def main() -> int:
    failed = 0
    for fn in TESTS:
        try:
            fn()
        except AssertionError as e:
            failed += 1
            print(f"[sgl host code] '{fn.__name__}'\n  {e}", file=sys.stderr)

    if failed:
        print(f"\n{failed} of {len(TESTS)} case(s) failed", file=sys.stderr)
        return 1

    print(f"sgl host code: {len(TESTS)} case(s) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
