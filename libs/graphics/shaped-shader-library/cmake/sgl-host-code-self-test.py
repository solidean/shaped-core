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

import sgl_host_code  # noqa: E402
from sgl_description import SglFile  # noqa: E402

REPO = Path(__file__).resolve().parents[4]

# sg's typed view aliases, one per sg::texture_view_dimension: what view_traits must name for every dimension.
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


# ---- view_traits ----------------------------------------------------------------------------------------------------


@test
def every_texture_view_dimension_names_sgs_own_alias():
    aliases = re.findall(r"using (tv_\w+) = texture_view_traits<texture_view_dimension::(\w+)>;",
                         VIEWS_HH.read_text(encoding="utf-8"))
    if len(aliases) < 9:
        raise AssertionError(f"read only {len(aliases)} tv_ alias(es) from {VIEWS_HH} -- the pattern is stale")
    for alias, dimension in aliases:
        expect_equal(sgl_host_code.view_traits({"texture_dimension": dimension}), f"sg::{alias}",
                     f"view_traits of '{dimension}'")


# ---- binding_entry --------------------------------------------------------------------------------------------------


def resource(kind: str, **fields) -> dict:
    return {"kind": kind, "name": "r", "host_name": "g_r", "slot": 3, **fields}


@test
def a_texture_entry_carries_its_sample_type():
    for sample_type in ("filterable_float", "unfilterable_float", "depth", "sint", "uint"):
        got = sgl_host_code.binding_entry(resource("texture", texture_dimension="cube", sample_type=sample_type))
        expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::readonly_texture, '
                          ".texture_dimension = sg::texture_view_dimension::cube, "
                          f".sample_type = sg::texture_sample_type::{sample_type}}}",
                     f"a texture of sample type '{sample_type}'")


@test
def an_image_entry_carries_its_format_and_access():
    got = sgl_host_code.binding_entry(resource("image", texture_dimension="tex_2d_array", storage_format="r32_uint",
                                               storage_access="write"))
    expect_equal(got, '{.name = "g_r", .index = 3u, .count = 1u, .type = sg::binding_type::readwrite_texture, '
                      ".texture_dimension = sg::texture_view_dimension::tex_2d_array, "
                      ".storage_format = sg::pixel_format::r32_uint, .storage_access = sg::storage_access::write}",
                 "an image")


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
         "texture_dimension": "tex_3d", "storage_format": "r32_uint", "storage_access": "read_write"},
        {"kind": "sampler", "name": "picked", "type": "sampler", "host_name": "shadow_picked", "slot": 2,
         "sampler_type": "non_filtering"},
        {"kind": "sampler", "name": "compare", "type": "sampler", "host_name": "shadow_compare", "slot": 3,
         "sampler_type": "comparison", "static_sampler": {"min_filter": "linear", "compare": "less"}},
    ],
}

FILE = SglFile(path="shadow.sgl")


@test
def a_group_has_a_field_per_view_and_per_dynamic_sampler():
    header = sgl_host_code.emit_group("pkg", "ns", FILE, GROUP)
    expect_in("sg::texture_view<sg::tv_cube> depth_map;", header, "a cube texture's field")
    expect_in("sg::image_view<sg::tv_3d> counts;", header, "a 3d image's field")
    expect_in("sg::sampler picked;", header, "a dynamic sampler's field")
    # A static sampler is the layout's, so the group the host fills has nothing to set for it.
    expect_not_in(" compare;", header, "a static sampler")


@test
def a_groups_static_sampler_is_declared_and_its_dynamic_one_gathered():
    source = sgl_host_code.emit_group_impl("pkg", "ns", FILE, GROUP)
    expect_in('{.name = "shadow_compare", .sampler = {.min_filter = sg::sampler_filter::linear, '
              ".compare = sg::compare_op::less}}", source, "the static sampler's table entry")
    expect_in("return k_sgl_samplers_shadow;", source, "declared_samplers")
    expect_in('samplers.push_back({.name = "shadow_picked", .sampler = picked});', source, "the dynamic sampler")
    expect_not_in('.sampler = compare}', source, "a static sampler gathered as a dynamic one")
    expect_in("views.reserve(2);", source, "only views are gathered as views")


# ---- the runner -----------------------------------------------------------------------------------------------------


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
