"""The C++ an SGL package's typed entries generate, from what `sgl describe` said about them.

sgl_description.py is how the compiler is asked; this is what the answer becomes on the host.
Everything SGL-shaped arrives already checked: a binding the emitter would refuse never reaches here.
What is decided here is only the host side -- which C++ type an SGL type is, and how sg is handed the result.

A group is data and never an API: the fields, the declarations sg reads, and `gather`.
It carries no group index, because SGL numbers a group by its position in each entry point's list; the caller binds it with
`bind_group(index, group)`, and sg refuses one bound where it does not fit by naming both layouts.
"""

from __future__ import annotations

from sgl_description import SglEntries, SglFile

# An SGL type as the host holds it: the C++ type, its size in bytes, and the vertex format it is read as (None where it is none).
# The size is checked against the one `describe` reports, so a host type that drifted from the shader's is a build error.
HOST_TYPES: dict[str, tuple[str, int, str | None]] = {
    "float": ("float", 4, "f32"),
    "int": ("cc::i32", 4, "i32"),
    "uint": ("cc::u32", 4, "u32"),
    "float2": ("tg::vec2f", 8, "vec2f"),
    "int2": ("tg::vec2i", 8, "vec2i"),
    "int4": ("tg::vec4i", 16, "vec4i"),
    "uint2": ("tg::vec<2, cc::u32>", 8, "vec2u"),
    "uint3": ("tg::vec<3, cc::u32>", 12, "vec3u"),
    "uint4": ("tg::vec<4, cc::u32>", 16, "vec4u"),
    "float3": ("tg::vec3f", 12, "vec3f"),
    "vec3": ("tg::vec3f", 12, "vec3f"),
    "pos3": ("tg::pos3f", 12, "vec3f"),
    "float4": ("tg::vec4f", 16, "vec4f"),
    "int3": ("tg::vec3i", 12, "vec3i"),
    "mat4": ("tg::mat4f", 64, None),
    # an atomic is a plain integer to the host, which never updates it while the shader does
    "atomic[uint]": ("cc::u32", 4, None),
    "atomic[int]": ("cc::i32", 4, None),
    "bool32": ("slib::gpu_bool", 4, None),
}


def dx12_semantic(member: dict) -> str:
    """The dx12 semantic of a vertex input member, which `sgl describe` states (EMIT-28).
    The text SGL emits takes it from the same place, so the input layout and the shader always agree."""
    return member["semantic"]


def memory_structs_of(entries: SglEntries) -> dict[str, int]:
    """Every struct the package places in GPU memory, by name, and its size: each is a host type named as SGL names it."""
    return {s["name"]: s["size"] for _, s in entries.memory_structs}


def host_size(memory: dict[str, int], sgl_type: str) -> int:
    return memory[sgl_type] if sgl_type in memory else HOST_TYPES[sgl_type][1]


class HostCodeError(Exception):
    """A declaration the compiler accepts and the host has no type for yet."""


def host_type(package: str, memory: dict[str, int], where: str, sgl_type: str) -> str:
    """The C++ spelling of `sgl_type`: one of `memory`, the package's structs in GPU memory, or a builtin's."""
    if sgl_type in memory:
        return sgl_type
    known = HOST_TYPES.get(sgl_type)
    if known is None:
        raise HostCodeError(
            f"shader package '{package}': {where} has type '{sgl_type}', which has no C++ type in the generator yet "
            f"(sgl_host_code.py knows: {', '.join(HOST_TYPES)})")
    return known[0]


def check_names(package: str, entries: SglEntries, taken: dict[str, str]) -> None:
    """Every generated type lives in the package's namespace, so two files declaring one name is a build error here.

    `taken` maps a C++ name already emitted to what emitted it, the per-file asset structs among them.
    """
    declared = [(file, b["name"], f"`binding {b['name']}`") for file, b in entries.bindings]
    declared += [(file, v["name"], f"`@vertex struct {v['name']}`") for file, v in entries.vertex_inputs]
    declared += [(file, t["name"], f"`@pixel struct {t['name']}`") for file, t in entries.render_targets]
    declared += [(file, s["name"], f"`struct {s['name']}`") for file, s in entries.memory_structs]
    for file, name, what in declared:
        if name in taken:
            raise HostCodeError(
                f"shader package '{package}': '{file.path}' declares {what}, and the generated C++ name "
                f"'{name}' is already {taken[name]}")
        taken[name] = f"{what} of '{file.path}'"


def includes(entries: SglEntries) -> list[str]:
    """The headers the generated SGL types need, beyond what every package header includes."""
    out = []
    # A layout's static samplers: an entry point's stated in the header, a pipeline's in the source that includes it.
    if (any(e.get("samplers") for e in entries.described_entry_points.values())
            or any(p.get("samplers") for _, p in entries.pipelines)):
        out += ["<shaped-graphics/binding/pipeline_layout.hh>", "<shaped-graphics/binding/sampler.hh>",
                "<shaped-shader-library/binding/binding_groups.hh>"]
    if not entries.bindings and not entries.vertex_inputs and not entries.render_targets:
        return out
    out += ["<clean-core/container/span.hh>", "<clean-core/container/vector.hh>",
           "<shaped-graphics/binding/binding.hh>", "<shaped-graphics/binding/binding_group.hh>"]
    if entries.vertex_inputs:
        out += ["<shaped-graphics/raster/vertex_input.hh>", "<shaped-graphics/resource/buffer.hh>",
                "<shaped-graphics/resource/vertex_buffer_view.hh>", "<clean-core/container/fixed_array.hh>"]
    if any(not b["inline"] for _, b in entries.bindings):
        out.append("<shaped-graphics/resource/views.hh>")
    if entries.render_targets:
        out += ["<clean-core/container/fixed_vector.hh>", "<clean-core/error/optional.hh>",
                "<clean-core/string/string_view.hh>", "<shaped-graphics/command_list/raster.hh>",
                "<shaped-graphics/raster/raster_pipeline.hh>"]
    if any(b["inline"] for _, b in entries.bindings):
        out += ["<clean-core/container/fixed_array.hh>", "<clean-core/fwd.hh>"]
    if any(m.get("count", 1) > 1 for _, b in entries.bindings for m in b["members"]):
        out += ["<clean-core/container/fixed_array.hh>"]

    # Every type a field names: a constant's own, a buffer's element, and a vertex attribute's.
    types = {m["type"] for _, b in entries.bindings for m in b["members"]}
    types |= {m["type"] for _, v in entries.vertex_inputs for m in v["members"]}
    types |= {m["type"] for _, s in entries.memory_structs for m in s["members"]}
    if types & {"int", "uint", "atomic[int]", "atomic[uint]"}:
        out.append("<clean-core/fwd.hh>")
    if types & {"float2", "float3", "vec3", "float4", "int2", "int3", "int4", "uint2", "uint3", "uint4"}:
        out.append("<typed-geometry/linalg/vec.hh>")
    if any(m["kind"] == "sampler" for _, b in entries.bindings for m in b["members"]):
        out.append("<shaped-graphics/binding/sampler.hh>")
    if "pos3" in types:
        out.append("<typed-geometry/linalg/pos.hh>")
    if "mat4" in types:
        out.append("<typed-geometry/linalg/mat.hh>")
    if "bool32" in types:
        out.append("<shaped-shader-library/gpu_bool.hh>")
    return list(dict.fromkeys(out))


# ---- a resource group ----------------------------------------------------------------------------------------------


def emit_group(package: str, memory: dict[str, int], namespace: str, file: SglFile, binding: dict) -> str:
    name = binding["name"]
    out = [f"\nnamespace {namespace}\n{{\n"]
    out.append(f"/// `binding {name}` of {file.path}, as the host fills it. Generated; do not edit.\n")
    out.append("///\n")
    out.append("/// It fixes no group index: SGL numbers a group by its position in each entry point's list, so bind it\n")
    out.append("/// with `bind_group(index, group)` at the index the pipeline has it at.\n")
    out.append("///\n")
    out.append(f"///     auto const layout = ctx.cached.acquire_binding_group_layout<{namespace}::{name}>();\n")
    out.append(f"///     auto const g = ctx.transient.create_binding_group(cmd, layout, {namespace}::{name}{{...}});\n")
    out.append(f"struct {name}\n{{\n")
    # One field per member, in the shader's order.
    # A buffer's field is the view its access takes, of the element the shader reads, so a read-only view of a `mut`
    # buffer, or a view of another element type, does not compile.
    # A plain member is a plain field: the group owns the constant buffer it lands in, and sg fills it at creation.
    for member in binding["members"]:
        where = f"'{file.path}' `binding {name}` member '{member['name']}'"
        # a binding array is one field holding a view per element
        count = member.get("count", 1)
        array = (lambda view: f"cc::fixed_array<{view}, {count}>") if count > 1 else (lambda view: view)
        if member["kind"] == "constant":
            out.append(f"    {host_type(package, memory, where, member['type'])} {member['name']}; ///< `{member['type']}`, "
                       f"at byte {member['offset']} of the group's constant buffer\n")
            continue
        if member["kind"] == "texture":
            out.append(f"    {array(f'sg::texture_view_{view_shape(member)}')} {member['name']}; ///< `{member['type']}`\n")
            continue
        if member["kind"] == "image":
            view = f"sg::image_view_{view_shape(member)}<sg::pixel_format::{member['image_format']}>"
            out.append(f"    {array(view)} {member['name']}; ///< `{member['type']}`\n")
            continue
        if member["kind"] == "acceleration_structure":
            out.append(f"    sg::tlas_view {member['name']}; ///< `{member['type']}`\n")
            continue
        if member["kind"] == "sampler":
            # A static sampler is the layout's, so the group has no field for it.
            if "static_sampler" not in member:
                out.append(f"    sg::sampler {member['name']}; ///< `{member['type']}`, which the group binds\n")
            continue
        element = host_type(package, memory, where, member["type"])
        is_written = member["access"] == "read_write"
        access = "readwrite" if is_written else "readonly"
        sgl_type = f"mut buffer[{member['type']}]" if is_written else f"buffer[{member['type']}]"
        out.append(f"    {array(f'sg::{access}_buffer_view<{element}>')} {member['name']}; ///< `{sgl_type}`\n")
    out.append("\n")
    if has_block(binding):
        out.append("    /// The group's constant buffer: its plain members, laid out as the shader reads them, at this slot.\n")
        out.append("    /// sg allocates and fills it when the group is created, so no caller supplies it.\n")
        out.append(f"    static constexpr cc::isize constants_size = {binding['block_size']};\n")
        out.append(f"    static constexpr int constants_slot = {binding['block_slot']};\n")
        out.append("    void write_constants(cc::span<cc::byte> block) const;\n")
        out.append("\n")
    out.append("    /// The group's bindings in slot order, as the compiled shader reflects them.\n")
    out.append("    [[nodiscard]] static cc::span<sg::binding const> declared_bindings();\n")
    out.append("\n")
    out.append("    /// The group's static samplers, the `sampler name:` blocks of the binding, which the layout carries.\n")
    out.append("    [[nodiscard]] static cc::span<sg::named_sampler const> declared_samplers();\n")
    out.append("\n")
    out.append("    /// The slot-keyed views the fields above amount to.\n")
    out.append("    void gather(cc::vector<sg::slotted_view>& views, cc::vector<sg::named_sampler>& samplers) const;\n")
    out.append("};\n")
    out.append("\n")
    out.append("// So a protocol mismatch names this group rather than a scope template's constraint.\n")
    out.append(f"static_assert(sg::declared_binding_set<{name}>);\n")
    out.append(f"}} // namespace {namespace}\n")
    return "".join(out)


def has_block(binding: dict) -> bool:
    """Whether a group has plain members, which the compiler writes as a constant buffer at the group's first slot."""
    return binding.get("block_slot", -1) >= 0


def view_shape(member: dict) -> str:
    """The shape suffix of sg's view typedefs for a texture or an image: `2d` for `tex_2d`, `cube` for `cube`."""
    return member["texture_dimension"].removeprefix("tex_")


# sg::sampler's fields in its own declaration order, which a designated initializer has to follow, and their C++ spelling.
SAMPLER_FIELDS: tuple[tuple[str, str], ...] = (
    ("min_filter", "sg::sampler_filter::{}"),
    ("mag_filter", "sg::sampler_filter::{}"),
    ("mip_filter", "sg::sampler_filter::{}"),
    ("address_u", "sg::sampler_address_mode::{}"),
    ("address_v", "sg::sampler_address_mode::{}"),
    ("address_w", "sg::sampler_address_mode::{}"),
    ("mip_lod_bias", "{}f"),
    ("max_anisotropy", "{}u"),
    ("min_lod", "{}f"),
    ("max_lod", "{}f"),
    ("compare", "sg::compare_op::{}"),
)


def sampler_initializer(state: dict) -> str:
    """One sg::sampler as a designated initializer, every field the description carries."""
    fields = []
    for key, pattern in SAMPLER_FIELDS:
        if key not in state:
            continue
        value = state[key]
        if key == "max_lod" and value >= 3.4e38:
            fields.append(".max_lod = sg::sampler::lod_max")
            continue
        # A JSON number may arrive as an int, and `0f` is no C++ literal.
        if pattern == "{}f":
            value = repr(float(value))
        fields.append(f".{key} = {pattern.format(value)}")
    return "{" + ", ".join(fields) + "}"


def bound_samplers(where: str, samplers: list[dict], names: list[str], indent: str) -> str:
    """The `sg::bound_sampler` rows of a layout's file-scope samplers, one per line, each at the index SGL gave it."""
    by_name = {s["name"]: s for s in samplers}
    out = []
    for name in names:
        if name not in by_name:
            raise HostCodeError(f"{where} names the sampler '{name}', which its file does not describe")
        s = by_name[name]
        out.append(f'{indent}{{.binding = {{.name = "{name}", .space = slib::bound_samplers_space, '
                   f".index = {s['index']}u, .count = 1u, .type = sg::binding_type::sampler, "
                   f".sampler_type = sg::sampler_binding_type::{s['sampler_type']}}},\n"
                   f"{indent} .sampler = {sampler_initializer(s['settings'])}}},\n")
    return "".join(out)


def binding_entry(member: dict) -> str:
    """The sg::binding a resource member is, its fields in sg::binding's declaration order."""
    # a binding array takes `count` consecutive slots from its own
    head = f'{{.name = "{member["host_name"]}", .index = {member["slot"]}u, .count = {member.get("count", 1)}u, '
    kind = member["kind"]
    if kind == "buffer":
        access = "" if member["access"] == "read" else f", .access = sg::access_mode::{member['access']}"
        return head + f".type = sg::binding_type::buffer{access}}}"
    if kind == "texture":
        return head + (f".type = sg::binding_type::texture, "
                       f".texture_dimension = sg::texture_view_dimension::{member['texture_dimension']}, "
                       f".sample_type = sg::texture_sample_type::{member['sample_type']}}}")
    if kind == "acceleration_structure":
        return head + ".type = sg::binding_type::acceleration_structure}"
    if kind == "image":
        return head + (f".type = sg::binding_type::image, .access = sg::access_mode::{member['access']}, "
                       f".texture_dimension = sg::texture_view_dimension::{member['texture_dimension']}, "
                       f".image_format = sg::pixel_format::{member['image_format']}}}")
    return head + f".type = sg::binding_type::sampler, .sampler_type = sg::sampler_binding_type::{member['sampler_type']}}}"


def emit_group_impl(package: str, memory: dict[str, int], namespace: str, file: SglFile, binding: dict) -> str:
    name = binding["name"]
    qualified = f"{namespace}::{name}"
    resources = [m for m in binding["members"] if m["kind"] != "constant"]
    views = [m for m in resources if m["kind"] in ("buffer", "texture", "image", "acceleration_structure")]
    statics = [m for m in resources if m["kind"] == "sampler" and "static_sampler" in m]
    dynamic = [m for m in resources if m["kind"] == "sampler" and "static_sampler" not in m]
    out = [f"\n// `binding {name}` of {file.path}: the table the shader's resources were numbered from.\n"]
    out.append(f"namespace\n{{\nsg::binding const k_sgl_bindings_{name}[] = {{\n")
    if has_block(binding):
        # A constants block is read in rows of 16 bytes, so that is what the shader reflects its size as.
        size = (binding["block_size"] + 15) // 16 * 16
        out.append(f'    {{.name = "{binding["block_host_name"]}", .index = {binding["block_slot"]}u, .count = 1u, '
                   f".type = sg::binding_type::constants_buffer, .block_size = {size}}},\n")
    for member in resources:
        out.append(f"    {binding_entry(member)},\n")
    out.append("};\n")
    if statics:
        # A static sampler matches its sampler binding by name, which is the host name the shader's binding is renamed to.
        out.append(f"sg::named_sampler const k_sgl_samplers_{name}[] = {{\n")
        for member in statics:
            out.append(f'    {{.name = "{member["host_name"]}", .sampler = {sampler_initializer(member["static_sampler"])}}},\n')
        out.append("};\n")
    out.append("} // namespace\n")

    out.append(f"\ncc::span<sg::binding const> {qualified}::declared_bindings()\n{{\n")
    out.append(f"    return k_sgl_bindings_{name};\n}}\n")
    out.append(f"\ncc::span<sg::named_sampler const> {qualified}::declared_samplers()\n{{\n")
    out.append(f"    return k_sgl_samplers_{name};\n}}\n" if statics else "    return {};\n}\n")

    out.append(f"\nvoid {qualified}::gather(cc::vector<sg::slotted_view>& views,\n")
    out.append(" " * (len("void ") + len(qualified) + len("::gather(")))
    out.append("cc::vector<sg::named_sampler>& samplers) const\n{\n")
    if not dynamic:
        out.append("    (void)samplers;\n")
    out.append(f"    views.reserve({len(views)});\n")
    # sg keys a view by its position in declared_bindings(), which a binding array's run of registers no longer matches.
    position = {m["name"]: i + (1 if has_block(binding) else 0) for i, m in enumerate(resources)}
    for member in views:
        slot = position[member["name"]]
        if member.get("count", 1) > 1:
            # one view per element, in element order
            out.append("    {\n")
            out.append("        auto elements = cc::vector<sg::raw_view>();\n")
            out.append(f"        for (auto const& element : {member['name']})\n")
            out.append("            elements.push_back(element);\n")
            out.append(f"        views.push_back({{.slot = sg::binding_slot({slot}), .view = cc::move(elements)}});\n")
            out.append("    }\n")
            continue
        out.append(f"    views.push_back({{.slot = sg::binding_slot({slot}), .view = {member['name']}}});\n")
    for member in dynamic:
        out.append(f'    samplers.push_back({{.name = "{member["host_name"]}", .sampler = {member["name"]}}});\n')
    out.append("}\n")

    if has_block(binding):
        out.append(f"\nvoid {qualified}::write_constants(cc::span<cc::byte> block) const\n{{\n")
        out.append("    CC_ASSERT(block.size() >= constants_size, \"the block is smaller than the group's constant buffer\");\n")
        out.append(pack_members(package, memory, file, name, binding))
        out.append("}\n")
    return "".join(out)


def pack_members(package: str, memory: dict[str, int], file: SglFile, name: str, binding: dict) -> str:
    """Each plain member copied to the offset the compiler placed it at, its size checked against the shader's."""
    out = []
    for member in binding["members"]:
        if member["kind"] != "constant":
            continue
        cpp = host_type(package, memory, f"'{file.path}' `binding {name}` member '{member['name']}'", member["type"])
        size = host_size(memory, member["type"])
        if size != member["size"]:
            raise HostCodeError(
                f"shader package '{package}': '{file.path}' `binding {name}` member '{member['name']}' is "
                f"{member['size']} bytes in the shader and {size} as {cpp}")
        out.append(f"    static_assert(sizeof({cpp}) == {size});\n")
        out.append(f"    cc::memcpy(block.data() + {member['offset']}, &{member['name']}, {size});\n")
    return "".join(out)


# ---- a struct in GPU memory ---------------------------------------------------------------------------------------


def padded_fields(package: str, memory: dict[str, int], where: str, members: list[dict], indent: str) -> str:
    """Each member as a field, with a named padding field wherever SGL's layout leaves a gap before one.

    Padding is visible rather than implied, and `= {}`, so a designated initializer never has to name it.
    Every gap is a whole number of 4-byte words, since every value in GPU memory is.
    """
    out = []
    end = 0
    padding = 0
    for member in members:
        gap = member["offset"] - end
        if gap > 0:
            words = gap // 4
            extent = "" if words == 1 else f"[{words}]"
            out.append(f"{indent}cc::u32 _pad{padding}{extent} = {{}}; ///< {gap} bytes SGL's layout leaves free\n")
            padding += 1
        cpp = host_type(package, memory, f"{where} member '{member['name']}'", member["type"])
        out.append(f"{indent}{cpp} {member['name']}; ///< `{member['type']}`, at byte {member['offset']}\n")
        end = member["offset"] + member["size"]
    return "".join(out)


def layout_asserts(qualified: str, members: list[dict], size: int) -> str:
    """The generated struct's size and every member's offset held to what SGL placed, so a drifted host type fails the build."""
    out = [f"static_assert(sizeof({qualified}) == {size});\n"]
    for member in members:
        out.append(f"static_assert(offsetof({qualified}, {member['name']}) == {member['offset']});\n")
    return "".join(out)


def emit_memory_struct(package: str, memory: dict[str, int], namespace: str, file: SglFile, struct: dict) -> str:
    name = struct["name"]
    space = "a constant block" if struct["space"] == "constants" else "a storage buffer"
    out = [f"\nnamespace {namespace}\n{{\n"]
    out.append(f"/// `struct {name}` of {file.path}, byte for byte as {space} holds it. Generated; do not edit.\n")
    out.append("/// Copy it into GPU memory as it is: every member sits where the shader reads it on every backend.\n")
    out.append(f"struct {name}\n{{\n")
    out.append(padded_fields(package, memory, f"'{file.path}' `struct {name}`", struct["members"], "    "))
    out.append("};\n")
    out.append(f"}} // namespace {namespace}\n")
    return "".join(out)


def emit_memory_struct_impl(namespace: str, struct: dict) -> str:
    return "\n" + layout_asserts(f"{namespace}::{struct['name']}", struct["members"], struct["size"])


# ---- an @inline block ----------------------------------------------------------------------------------------------


def emit_inline(package: str, memory: dict[str, int], namespace: str, file: SglFile, binding: dict) -> str:
    """An `@inline binding`: the block byte for byte as the shader reads it, padding included, and `to_block` a copy of it."""
    name = binding["name"]
    out = [f"\nnamespace {namespace}\n{{\n"]
    out.append(f"/// `@inline binding {name}` of {file.path}: the inline constants, byte for byte as the shader reads them. Generated; do not edit.\n")
    out.append("///\n")
    out.append(f"///     pass.set_inline_constants({namespace}::{name}{{...}}.to_block());\n")
    out.append(f"struct {name}\n{{\n")
    out.append(padded_fields(package, memory, f"'{file.path}' `@inline binding {name}`", binding["members"], "    "))
    out.append("\n")
    out.append("    /// The block's size in bytes, as the shader lays it out.\n")
    out.append(f"    static constexpr cc::isize block_size = {binding['block_size']};\n")
    out.append("\n")
    out.append("    /// This value as the shader reads it, which is its own bytes.\n")
    out.append("    [[nodiscard]] cc::fixed_array<cc::byte, block_size> to_block() const;\n")
    out.append("\n")
    out.append("    /// The block as a pipeline layout takes it, `inline_constants`: every backend places it by a fixed rule.\n")
    out.append("    [[nodiscard]] static sg::binding inline_binding();\n")
    out.append("};\n")
    out.append("\n")
    out.append(f"static_assert(sg::declared_inline_constants<{name}>);\n")
    out.append(f"}} // namespace {namespace}\n")
    return "".join(out)


def emit_inline_impl(package: str, memory: dict[str, int], namespace: str, file: SglFile, binding: dict) -> str:
    name = binding["name"]
    qualified = f"{namespace}::{name}"
    for member in binding["members"]:
        host_type(package, memory, f"'{file.path}' `@inline binding {name}` member '{member['name']}'", member["type"])
    out = ["\n" + layout_asserts(qualified, binding["members"], binding["block_size"])]
    out.append(f"\ncc::fixed_array<cc::byte, {qualified}::block_size> {qualified}::to_block() const\n{{\n")
    out.append("    auto block = cc::fixed_array<cc::byte, block_size>{};\n")
    out.append("    cc::memcpy(block.data(), this, block_size);\n")
    out.append("    return block;\n}\n")
    # A block is read in 4-byte words, which is what every backend's inline constants are counted in.
    words = (binding["block_size"] + 3) // 4 * 4
    out.append(f"\nsg::binding {qualified}::inline_binding()\n{{\n")
    out.append("    // dx12 reads it at b0 in slib's reserved space; vulkan and WebGPU read only the kind and the size.\n")
    out.append(f'    return {{.name = "{name}",\n')
    out.append("            .space = slib::inline_constants_space,\n")
    out.append("            .index = 0u,\n")
    out.append("            .count = 1u,\n")
    out.append("            .type = sg::binding_type::constants_buffer,\n")
    out.append(f"            .block_size = {words}}};\n}}\n")
    return "".join(out)


# ---- the package's share of the header and the source ---------------------------------------------------------------


def emit_header(package: str, namespace: str, entries: SglEntries) -> str:
    memory = memory_structs_of(entries)
    out = []
    # Ahead of the bindings, whose fields and views name them.
    for file, struct in entries.memory_structs:
        out.append(emit_memory_struct(package, memory, namespace, file, struct))
    for file, binding in entries.bindings:
        if binding["inline"]:
            out.append(emit_inline(package, memory, namespace, file, binding))
        else:
            out.append(emit_group(package, memory, namespace, file, binding))
    for file, struct in entries.vertex_inputs:
        out.append(emit_vertex_input(package, namespace, file, struct))
    for file, struct in entries.render_targets:
        out.append(emit_render_target(package, namespace, file, struct))
    return "".join(out)


def emit_source(package: str, namespace: str, entries: SglEntries) -> str:
    memory = memory_structs_of(entries)
    out = []
    for _, struct in entries.memory_structs:
        out.append(emit_memory_struct_impl(namespace, struct))
    for file, binding in entries.bindings:
        if binding["inline"]:
            out.append(emit_inline_impl(package, memory, namespace, file, binding))
        else:
            out.append(emit_group_impl(package, memory, namespace, file, binding))
    for file, struct in entries.vertex_inputs:
        out.append(emit_vertex_input_impl(package, namespace, file, struct))
    for file, struct in entries.render_targets:
        out.append(emit_render_target_impl(namespace, struct))
    return "".join(out)


# ---- the entry points --------------------------------------------------------------------------------------------------


def entry_wrappers(entries: SglEntries, stems: dict[str, str]) -> dict[tuple[str, str], str]:
    """The wrapper type each described entry point gets, by (path, entry point).

    One is written only where every binding the entry point lists has a generated type, since its layout is spelled
    with them; any other entry point stays a plain handle.
    """
    generated = {b["name"] for _, b in entries.bindings}
    out = {}
    for (path, name), described in entries.described_entry_points.items():
        if all(b in generated for b in described["bindings"]):
            out[(path, name)] = f"{stems[path]}_{name}_t"
    return out


def emit_entry_wrappers(entries: SglEntries, stems: dict[str, str]) -> str:
    """One struct per wrapped entry point: its asset, and the pipeline layout its binding list states."""
    wrappers = entry_wrappers(entries, stems)
    out = []
    for (path, name), type_name in wrappers.items():
        described = entries.described_entry_points[(path, name)]
        listed = described["bindings"]
        types = ", ".join(listed)
        out.append(f"/// `{name}` of {path}: a {described['stage']} entry point listing {{{', '.join(listed)}}}.\n")
        out.append(f"struct {type_name}\n{{\n")
        out.append("    slib::shader_asset_handle asset;\n")
        out.append("\n")
        out.append("    /// The asset, as a plain handle has it: `->acquire(ctx)`.\n")
        out.append("    [[nodiscard]] slib::shader_asset* operator->() const { return asset.get(); }\n")
        out.append("    [[nodiscard]] bool operator==(std::nullptr_t) const { return asset == nullptr; }\n")
        out.append("\n")
        out.append(f"    /// The pipeline layout this entry point's binding list states, with no reflected binding in it.\n")
        out.append("    /// It carries only the file samplers this entry point reaches, so a file used as a library never fills the sampler slots.\n")
        out.append("    /// A raster pipeline whose stages list different groups needs their union, spelled `ctx.cached.acquire_pipeline_layout<...>()`.\n")
        out.append("    /// One whose stages reach file samplers is built from the file's SGL `pipeline`, whose layout carries every stage's samplers.\n")
        out.append("    [[nodiscard]] sg::pipeline_layout_handle acquire_layout(sg::context& ctx) const\n    {\n")
        samplers = described.get("samplers", [])
        if samplers:
            # The file-scope samplers its code reaches, which no group holds.
            rows = bound_samplers(f"'{path}' entry point '{name}'", entries.file_samplers.get(path, []), samplers,
                                  "            ")
            out.append(f"        static sg::bound_sampler const samplers[] = {{\n{rows}        }};\n")
            out.append(f"        return ctx.cached.acquire_pipeline_layout<{types}>(samplers);\n    }}\n")
        else:
            out.append(f"        return ctx.cached.acquire_pipeline_layout<{types}>();\n    }}\n")
        if described["stage"] == "compute":
            out.append("\n")
            out.append("    /// The compute pipeline of this entry point over that layout, which is all a compute pipeline needs.\n")
            out.append("    [[nodiscard]] sg::async_compute_pipeline acquire_pipeline(sg::context& ctx) const\n    {\n")
            out.append("        return slib::acquire_compute_pipeline(&ctx, asset, acquire_layout(ctx));\n    }\n")
        out.append("};\n\n")
    return "".join(out)


# ---- a vertex input ----------------------------------------------------------------------------------------------------


def streams_of(struct: dict) -> list[tuple[str, bool, list[dict]]]:
    """(stream, steps per instance, its members), in the order the streams first appear, which is their slot order."""
    out: dict[str, tuple[bool, list[dict]]] = {}
    for member in struct["members"]:
        entry = out.setdefault(member["stream"], (member["per_instance"], []))
        entry[1].append(member)
    return [(name, per_instance, members) for name, (per_instance, members) in out.items()]


# A member whose `@format` packs it: the bytes the host writes, whatever type the shader reads them as.
PACKED_HOST_TYPES = {"rgba8_unorm": "cc::u32", "rgba8_uint": "cc::u32"}


def vertex_member_host_type(package: str, where: str, member: dict) -> str:
    fmt = member.get("format", "")
    if fmt in PACKED_HOST_TYPES:
        return PACKED_HOST_TYPES[fmt]
    return host_type(package, {}, where, member["type"])


def vertex_format(package: str, file: SglFile, struct: str, member: dict) -> str:
    if member.get("format"):
        return member["format"]
    # A vertex attribute is a builtin, never a struct in GPU memory.
    host_type(package, {}, f"'{file.path}' `@vertex struct {struct}` member '{member['name']}'", member["type"])
    fmt = HOST_TYPES[member["type"]][2]
    if fmt is None:
        raise HostCodeError(f"shader package '{package}': '{file.path}' `@vertex struct {struct}` member '{member['name']}' "
                            f"has type '{member['type']}', which no vertex format reads")
    return fmt


def emit_vertex_input(package: str, namespace: str, file: SglFile, struct: dict) -> str:
    """A `@vertex struct`: what a vertex buffer holds, and the layout a pipeline reads it with.

    One stream is the struct itself.
    Several are one nested struct each, in slot order, with `buffers` holding one typed buffer per stream, so a draw names
    which buffer feeds which members and no slot index or byte offset is written by hand.
    """
    name = struct["name"]
    streams = streams_of(struct)

    def fields(members: list[dict], indent: str) -> str:
        out = []
        for m in members:
            cpp = vertex_member_host_type(package, f"'{file.path}' `@vertex struct {name}` member '{m['name']}'", m)
            out.append(f"{indent}{cpp} {m['name']}; ///< location {m['location']}, `{dx12_semantic(m)}` on dx12\n")
        return "".join(out)

    out = [f"\nnamespace {namespace}\n{{\n"]
    if len(streams) == 1:
        out.append(f"/// `@vertex struct {name}` of {file.path}, as a vertex buffer holds it. Generated; do not edit.\n")
        out.append(f"struct {name}\n{{\n")
        out.append(fields(streams[0][2], "    "))
        out.append("\n")
        out.append("    /// The layout a pipeline reads this with: every member at its offset, in the shader's order.\n")
        out.append("    [[nodiscard]] static sg::vertex_input_layout layout();\n")
        out.append("};\n")
    else:
        names = ", ".join(f"`{stream}`" for stream, _, _ in streams)
        out.append(f"/// `@vertex struct {name}` of {file.path}, read from {len(streams)} buffers: {names}. Generated; do not edit.\n")
        out.append(f"struct {name}\n{{\n")
        for slot, (stream, per_instance, members) in enumerate(streams):
            out.append(f"    /// Slot {slot}, one element per {'instance' if per_instance else 'vertex'}.\n")
            out.append(f"    struct {stream}\n    {{\n")
            out.append(fields(members, "        "))
            out.append("    };\n\n")
        out.append("    /// One buffer per stream, in slot order; only a buffer of that stream's element fits its field.\n")
        out.append("    struct buffers\n    {\n")
        for stream, _, _ in streams:
            out.append(f"        sg::buffer<{namespace}::{name}::{stream}> {stream};\n")
        out.append("\n")
        out.append("        /// What `bind_vertex_buffers` takes, in slot order.\n")
        out.append(f"        [[nodiscard]] cc::fixed_array<sg::vertex_buffer_view, {len(streams)}> views() const;\n")
        out.append("    };\n\n")
        out.append("    /// The layout a pipeline reads these with: one slot per stream, and the attributes in the shader's order,\n")
        out.append("    /// since vulkan and WGSL take attribute i as location i whatever buffer it comes from.\n")
        out.append("    [[nodiscard]] static sg::vertex_input_layout layout();\n")
        out.append("};\n")
    out.append(f"}} // namespace {namespace}\n")
    return "".join(out)


def emit_vertex_input_impl(package: str, namespace: str, file: SglFile, struct: dict) -> str:
    name = struct["name"]
    qualified = f"{namespace}::{name}"
    streams = streams_of(struct)
    single = len(streams) == 1
    slot_of = {stream: slot for slot, (stream, _, _) in enumerate(streams)}

    def owner(stream: str) -> str:
        return qualified if single else f"{qualified}::{stream}"

    out = [f"\nsg::vertex_input_layout {qualified}::layout()\n{{\n"]
    out.append("    return {.slots = {")
    out.append(", ".join(f"{{.stride = cc::isize(sizeof({owner(stream)})){', .per_instance = true' if per_instance else ''}}}"
                         for stream, per_instance, _ in streams))
    out.append("},\n")
    out.append("            .attributes = {\n")
    for member in struct["members"]:
        fmt = vertex_format(package, file, name, member)
        out.append(f'                {{.semantic = "{dx12_semantic(member)}", '
                   f".format = sg::vertex_attribute_format::{fmt}, "
                   f".offset = cc::isize(offsetof({owner(member['stream'])}, {member['name']})), "
                   f".slot = {slot_of[member['stream']]}}},\n")
    out.append("            }};\n}\n")

    if not single:
        out.append(f"\ncc::fixed_array<sg::vertex_buffer_view, {len(streams)}> {qualified}::buffers::views() const\n{{\n")
        out.append("    return {" + ", ".join(f"{stream}.as_vertex_buffer()" for stream, _, _ in streams) + "};\n}\n")
    return "".join(out)


# ---- a render target ---------------------------------------------------------------------------------------------------

# What a generated render target declares beside its members, so no member may take one of these names.
RENDER_TARGET_RESERVED = ("name", "depth_stencil", "states")


def emit_render_target(package: str, namespace: str, file: SglFile, struct: dict) -> str:
    """A `@pixel struct`: the color targets a rendering binds, by member name rather than by index.

    It carries its qualified name, which a pipeline built from its `states` carries too, so sg refuses a draw that
    mixes two target sets even where their formats agree.
    """
    name = struct["name"]
    # the depth and the sample mask a pixel stage writes are outputs, and no color target a rendering binds
    members = [m for m in struct["members"] if not m.get("output")]
    for m in members:
        if m["name"] in RENDER_TARGET_RESERVED:
            raise HostCodeError(
                f"shader package '{package}': '{file.path}' `@pixel struct {name}` member '{m['name']}' takes a name "
                f"the generated target declares itself ({', '.join(RENDER_TARGET_RESERVED)})")

    out = [f"\nnamespace {namespace}\n{{\n"]
    out.append(f"/// `@pixel struct {name}` of {file.path}: the targets a rendering draws into, one per member. "
               "Generated; do not edit.\n")
    out.append(f"struct {name}\n{{\n")
    out.append("    /// What a pipeline built from `states` and a rendering opened with this both carry, and sg compares.\n")
    out.append(f'    static constexpr cc::string_view name = "{namespace}::{name}";\n\n')
    for m in members:
        out.append(f"    sg::color_target {m['name']}; ///< location {m['location']}\n")
    out.append("    /// A `@pixel struct` says nothing about depth, so the depth target stands beside the colors.\n")
    out.append("    cc::optional<sg::depth_stencil_target> depth_stencil;\n\n")
    out.append("    /// Every target in location order, and the name; viewport and scissor stay unset.\n")
    out.append("    [[nodiscard]] operator sg::rendering_info() const;\n\n")
    out.append("    /// The pipeline's half: one state per target, which converts to `color_targets` in location order.\n")
    out.append("    /// A pipeline that names no `target_set` takes the one its fragment shader writes, which is `name`.\n")
    out.append("    struct states\n    {\n")
    for m in members:
        out.append(f"        sg::color_target_state {m['name']};\n")
    out.append("\n")
    out.append("        [[nodiscard]] operator cc::fixed_vector<sg::color_target_state, sg::max_color_targets>() const;\n")
    out.append("    };\n")
    out.append("};\n")
    out.append(f"}} // namespace {namespace}\n")
    return "".join(out)


def emit_render_target_impl(namespace: str, struct: dict) -> str:
    qualified = f"{namespace}::{struct['name']}"
    names = [m["name"] for m in struct["members"] if not m.get("output")]
    out = [f"\n{qualified}::operator sg::rendering_info() const\n{{\n"]
    out.append("    return {.color_targets = {" + ", ".join(names) + "}, .depth_stencil_target = depth_stencil, "
               ".target_set = name};\n}\n")
    out.append(f"\n{qualified}::states::operator cc::fixed_vector<sg::color_target_state, sg::max_color_targets>() const\n{{\n")
    out.append("    return {" + ", ".join(names) + "};\n}\n")
    return "".join(out)


# ---- a pipeline -------------------------------------------------------------------------------------------------------


def pipeline_type(stem: str, name: str) -> str:
    return f"{stem}_{name}_t"


def open_fields(pipeline: dict) -> list[tuple[str, str, str]]:
    """The open struct's fields: (C++ type and default, field name, the path it states), in the order first left open."""
    out = []
    for path in pipeline["open"]:
        if path == "sample_count":
            out.append(("int", "sample_count", path))
        elif path == "depth_stencil_format":
            out.append(("sg::pixel_format", "depth_stencil_format", path))
        else:
            # color_targets.<target>.format: the field is the target's own name.
            out.append(("sg::pixel_format", path.split(".")[1], path))
    return out


# A pipeline's stages in the order a vertex passes through them, each a field of `describe`'s pipeline.
PIPELINE_STAGES = ("vertex", "tessellation_control", "tessellation_evaluation", "geometry", "pixel")


def pipeline_includes(entries: SglEntries) -> list[str]:
    out = []
    if entries.pipelines:
        out += ["<shaped-shader-library/pipeline.hh>", "<shaped-graphics/fwd.hh>", "<clean-core/thread/async.hh>"]
    if entries.raytracing_pipelines:
        out += ["<shaped-shader-library/raytracing_pipeline.hh>", "<shaped-graphics/fwd.hh>",
                "<clean-core/thread/async.hh>", "<clean-core/container/vector.hh>"]
    return out


def emit_pipelines(entries: SglEntries, stems: dict[str, str]) -> str:
    """One type per `pipeline` declaration: an `sg::raster_pipeline_source`, so `ctx.cached` acquires it."""
    out = []
    for file, p in entries.pipelines:
        stem = stems[file.path]
        type_name = pipeline_type(stem, p["name"])
        fields = open_fields(p)
        named = [p[s] for s in PIPELINE_STAGES if p[s]]
        stages = ", ".join(named[:-1]) + " and " + named[-1] if len(named) > 1 else named[0]
        writes = f", writing `{p['target_set']}`" if p["target_set"] else ", writing depth alone"
        out.append(f"/// `pipeline {p['name']}` of {file.path}: {stages}{writes}. Generated; do not edit.\n")
        out.append(f"/// Acquired as `ctx.cached.acquire_raster_pipeline({stem}.{p['name']}, …)`.\n")
        # Outside the pipeline's type: a nested struct with member initializers cannot be a default argument within it.
        out.append(f"/// What `pipeline {p['name']}` leaves to the host with `.host`, stated when it is acquired; every field must be.\n")
        out.append(f"struct {type_name}_open\n{{\n")
        for cpp, name, path in fields:
            default = "0" if cpp == "int" else "sg::pixel_format::undefined"
            out.append(f"    {cpp} {name} = {default}; ///< `{path}`\n")
        out.append("};\n")
        out.append(f"struct {type_name}\n{{\n")
        out.append(f"    using open = {type_name}_open;\n\n")
        out.append("    /// The description the build states, with `parts` stated and `customize` applied last.\n")
        out.append("    [[nodiscard]] cc::shared_async<sg::raster_pipeline_description> description("
                   "sg::context& ctx, open const& parts = {}, sg::raster_pipeline_customize customize = {}) const;\n")
        out.append("    /// The source's newest stages and settings, even where they moved what this code was built against.\n")
        out.append("    [[nodiscard]] cc::shared_async<sg::raster_pipeline_description> description_latest("
                   "sg::context& ctx, open const& parts = {}, sg::raster_pipeline_customize customize = {}) const;\n")
        out.append("    /// What slib describes it from: the stages, the layout, the settings as code, and the frozen part.\n")
        out.append("    [[nodiscard]] static slib::pipeline_definition const& definition();\n")
        out.append("};\n\n")
    return "".join(out)


def setter_call(s: dict, targets: list[str]) -> str:
    """One setting as a call of its generated setter in `slib::impl::fields`."""
    path = s["path"].split(".")
    target = ""
    if path[0] == "color_targets":
        target = f", {targets.index(path[1])}"
        path = ["color_targets"] + path[2:]
    name = "_".join(path)
    kind = s["kind"]
    if kind == "none":
        return f"f::{name}_none(d{target});"
    if kind == "bool":
        value = "true" if s["value"] else "false"
    elif kind == "int":
        value = str(s["value"])
    elif kind == "float":
        value = f"float({float(s['value'])!r})"
    else:
        value = f"sg::{s['enum']}::{s['value']}"
    return f"f::{name}(d{target}, {value});"


def open_call(path: str, index: int, targets: list[str]) -> str:
    """The host's value for one open part, as a call of its setter."""
    if path == "sample_count":
        return f"f::sample_count(d, int(open[{index}].value));"
    if path == "depth_stencil_format":
        return f"f::depth_stencil_format(d, sg::pixel_format(open[{index}].value));"
    target = targets.index(path.split(".")[1])
    return f"f::color_targets_format(d, {target}, sg::pixel_format(open[{index}].value));"


def emit_pipelines_impl(package: str, namespace: str, entries: SglEntries, stems: dict[str, str],
                        wrappers: dict[tuple[str, str], str]) -> str:
    out = []
    generated = {b["name"] for _, b in entries.bindings}
    vertex_inputs = {v["name"] for _, v in entries.vertex_inputs}
    for file, p in entries.pipelines:
        stem = stems[file.path]
        type_name = pipeline_type(stem, p["name"])
        qualified = f"{namespace}::{type_name}"
        key = f"{stem}_{p['name']}"
        groups = p["layout"] + ([p["inline"]] if p["inline"] else [])
        missing = [g for g in groups if g not in generated]
        # empty for a vertex stage that draws from no vertex buffer
        if p["vertex_input"] and p["vertex_input"] not in vertex_inputs:
            missing.append(p["vertex_input"])
        if missing:
            raise HostCodeError(
                f"shader package '{package}': `pipeline {p['name']}` of '{file.path}' is built from generated types, and "
                f"{', '.join(missing)} has none; declare the file as '{file.path}:*'")

        def handle(entry: str) -> str:
            return f"&{namespace}::{stem}.{entry}" + (".asset" if (file.path, entry) in wrappers else "")

        out.append("\nnamespace\n{\n")
        if p["targets"]:
            names = ", ".join(f'"{t}"' for t in p["targets"])
            out.append(f"constexpr cc::string_view k_{key}_targets[] = {{{names}}};\n")
        frozen = ",\n".join(f'    "{line}"' for line in p["frozen"])
        out.append(f"constexpr cc::string_view k_{key}_frozen[] = {{\n{frozen},\n}};\n")
        group_types = ", ".join(f"{namespace}::{g}" for g in groups)
        samplers = p.get("samplers", [])
        if samplers:
            where = f"shader package '{package}': `pipeline {p['name']}` of '{file.path}'"
            rows = bound_samplers(where, file.samplers, samplers, "    ")
            out.append(f"sg::bound_sampler const k_{key}_samplers[] = {{\n{rows}}};\n")
        out.append(f"sg::pipeline_layout_handle {key}_layout(sg::context& ctx)\n{{\n")
        passed = f"k_{key}_samplers" if samplers else ""
        out.append(f"    return ctx.cached.acquire_pipeline_layout<{group_types}>({passed});\n}}\n")

        # The build's settings as code, in the order they apply, and then what the host stated.
        out.append(f"void {key}_apply(sg::raster_pipeline_description& d, cc::span<slib::open_part const> open)\n{{\n")
        out.append("    namespace f = slib::impl::fields;\n")
        if not p["open"]:
            out.append("    (void)open;\n")
        for s in p["settings"]:
            if s["kind"] != "host":
                out.append(f"    {setter_call(s, p['targets'])}\n")
        for index, path in enumerate(p["open"]):
            out.append(f"    {open_call(path, index, p['targets'])}\n")
        out.append("}\n")
        out.append("} // namespace\n")

        out.append(f"\nslib::pipeline_definition const& {qualified}::definition()\n{{\n")
        out.append("    static slib::pipeline_definition const d = {\n")
        out.append(f'        .file = "{file.path}",\n')
        out.append(f'        .name = "{p["name"]}",\n')
        # in pipeline_definition's field order, which a designated initializer has to follow
        for s in ("vertex", "pixel", "geometry", "tessellation_control", "tessellation_evaluation"):
            if p[s]:
                out.append(f"        .{s} = {handle(p[s])},\n")
        out.append(f"        .acquire_layout = &{key}_layout,\n")
        if p["vertex_input"]:
            out.append(f"        .vertex_input = &{namespace}::{p['vertex_input']}::layout,\n")
        if p["target_set"]:
            out.append(f"        .target_set = {namespace}::{p['target_set']}::name,\n")
        if p["targets"]:
            out.append(f"        .targets = k_{key}_targets,\n")
        out.append(f"        .apply = &{key}_apply,\n")
        out.append(f"        .frozen = k_{key}_frozen,\n")
        out.append("    };\n    return d;\n}\n")

        fields = open_fields(p)
        stated = "{" + ", ".join(f'{{.path = "{path}", .value = cc::i64(parts.{name})}}' for _, name, path in fields) + "}"
        for verb, latest in (("description", "false"), ("description_latest", "true")):
            out.append(f"\ncc::shared_async<sg::raster_pipeline_description> {qualified}::{verb}("
                       "sg::context& ctx, open const& parts, sg::raster_pipeline_customize customize) const\n{\n")
            if not fields:
                out.append("    (void)parts;\n")
            out.append(f"    return slib::describe_raster_pipeline(&ctx, &definition(), cc::vector<slib::open_part>{stated}, "
                       f"cc::move(customize), {latest});\n}}\n")
    return "".join(out)


# ---- a ray-tracing pipeline --------------------------------------------------------------------------------------------


def emit_raytracing_pipelines(entries: SglEntries, stems: dict[str, str]) -> str:
    """One type per `@raytracing pipeline`: its description, its table, and its listed hit groups by position."""
    out = []
    for file, p in entries.raytracing_pipelines:
        type_name = pipeline_type(stems[file.path], p["name"])
        rays = file.ray_set(p["rays"])["rays"]
        out.append(f"/// `@raytracing pipeline {p['name']}` of {file.path}, over the ray set `{p['rays']}`. Generated; do not edit.\n")
        out.append(f"struct {type_name}\n{{\n")
        out.append(f"    /// The ray types of `{p['rays']}`, in table order: a trace of one takes its position as its contribution and its miss.\n")
        out.append(f"    static constexpr int ray_count = {len(rays)};\n")
        out.append("    /// The listed hit groups, by their position among the table's groups.\n")
        out.append("    struct hit_groups_t\n    {\n")
        for index, group in enumerate(p["hit_groups"]):
            out.append(f"        static constexpr int {group} = {index};\n")
        out.append("    };\n")
        if p["host_hit_groups"]:
            out.append(f"    /// The first position a host's hit group takes: they follow the listed ones, in the order handed over.\n")
            out.append(f"    static constexpr int first_host_hit_group = {len(p['hit_groups'])};\n")
        if p["host_callables"]:
            out.append(f"    /// The index a shader calls the host's first callable by: they follow every one the module lists.\n")
            out.append(f"    static constexpr int first_host_callable = {len(p['callables'])};\n")
        out.append("\n    /// The description the build states, with what the declaration leaves to the host in `host`.\n")
        out.append("    [[nodiscard]] cc::shared_async<sg::raytracing_pipeline_description> description("
                   "sg::context& ctx, slib::raytracing_host_parts host = {}) const;\n")
        out.append("    /// A table over `pipeline` with its raygen, a miss per ray type and every callable; rows follow with `add_row`.\n")
        out.append("    [[nodiscard]] static sg::raytracing_shader_table_description table_description("
                   "sg::raytracing_pipeline_handle pipeline, int host_callables = 0);\n")
        out.append("    /// Appends hit group `group`'s row: a record per ray type, whose offset an instance tracing through it takes.\n")
        out.append("    [[nodiscard]] static sg::hit_row add_row(sg::raytracing_shader_table_description& table, int group);\n")
        out.append("    /// What slib describes it from.\n")
        out.append("    [[nodiscard]] static slib::raytracing_pipeline_definition const& definition();\n")
        out.append("};\n\n")
    return "".join(out)


def emit_raytracing_pipelines_impl(package: str, namespace: str, entries: SglEntries, stems: dict[str, str],
                                   wrappers: dict[tuple[str, str], str]) -> str:
    out = []
    generated = {b["name"] for _, b in entries.bindings}
    for file, p in entries.raytracing_pipelines:
        stem = stems[file.path]
        type_name = pipeline_type(stem, p["name"])
        qualified = f"{namespace}::{type_name}"
        key = f"{stem}_{p['name']}"
        where = f"shader package '{package}': `@raytracing pipeline {p['name']}` of '{file.path}'"
        groups = p["layout"] + ([p["inline"]] if p["inline"] else [])
        missing = [g for g in groups if g not in generated]
        if missing:
            raise HostCodeError(f"{where} is built from generated types, and {', '.join(missing)} has none; "
                                f"declare the file as '{file.path}:*'")
        if any(not m for m in p["misses"]):
            # TODO: a ray type without a miss wants an empty miss record, which sg's table has no spelling for yet
            raise HostCodeError(f"{where} leaves a ray type without a miss, which slib cannot table yet")

        def handle(entry: str) -> str:
            if not entry:
                return "nullptr"
            return f"&{namespace}::{stem}.{entry}" + (".asset" if (file.path, entry) in wrappers else "")

        out.append("\nnamespace\n{\n")
        payloads = ", ".join(f'"{payload}"' for payload in file.ray_set(p["rays"])["payloads"])
        out.append(f"constexpr cc::string_view k_{key}_payloads[] = {{{payloads}}};\n")
        misses = ", ".join(handle(m) for m in p["misses"])
        out.append(f"slib::shader_asset_handle const* const k_{key}_misses[] = {{{misses}}};\n")
        for group_name in p["hit_groups"]:
            group = file.hit_group(group_name)
            closest = ", ".join(handle(e) for e in group["closest_hits"])
            any_hits = ", ".join(handle(e) for e in group["any_hits"])
            out.append(f"slib::shader_asset_handle const* const k_{key}_{group_name}_closest[] = {{{closest}}};\n")
            out.append(f"slib::shader_asset_handle const* const k_{key}_{group_name}_any[] = {{{any_hits}}};\n")
        if p["hit_groups"]:
            out.append(f"slib::hit_group_definition const k_{key}_groups[] = {{\n")
            for group_name in p["hit_groups"]:
                group = file.hit_group(group_name)
                out.append(f'    {{.name = "{group_name}", .intersection = {handle(group["intersection"])}, '
                           f".closest_hits = k_{key}_{group_name}_closest, .any_hits = k_{key}_{group_name}_any}},\n")
            out.append("};\n")
        group_types = ", ".join(f"{namespace}::{g}" for g in groups)
        out.append(f"sg::pipeline_layout_handle {key}_layout(sg::context& ctx)\n{{\n")
        out.append(f"    return ctx.cached.acquire_pipeline_layout<{group_types}>();\n}}\n")
        if p["callables"]:
            callables = ", ".join(handle(e) for e in p["callables"])
            out.append(f"slib::shader_asset_handle const* const k_{key}_callables[] = {{{callables}}};\n")
        out.append("} // namespace\n")

        out.append(f"\nslib::raytracing_pipeline_definition const& {qualified}::definition()\n{{\n")
        out.append("    static slib::raytracing_pipeline_definition const d = {\n")
        out.append(f'        .file = "{file.path}",\n')
        out.append(f'        .name = "{p["name"]}",\n')
        out.append("        .ray_count = ray_count,\n")
        out.append(f'        .ray_set = "{p["rays"]}",\n')
        out.append(f"        .payloads = k_{key}_payloads,\n")
        out.append(f"        .raygen = {handle(p['raygen'])},\n")
        out.append(f"        .misses = k_{key}_misses,\n")
        if p["hit_groups"]:
            out.append(f"        .hit_groups = k_{key}_groups,\n")
        if p["host_hit_groups"]:
            out.append("        .has_host_hit_groups = true,\n")
        if p["callables"]:
            out.append(f"        .callables = k_{key}_callables,\n")
        if p["host_callables"]:
            out.append("        .has_host_callables = true,\n")
        out.append(f"        .max_recursion_depth = {p['max_recursion_depth']},\n")
        out.append(f"        .max_payload_size = {p['max_payload_size']},\n")
        out.append(f"        .max_attribute_size = {p['max_attribute_size']},\n")
        out.append(f"        .acquire_layout = &{key}_layout,\n")
        out.append("    };\n    return d;\n}\n")

        out.append(f"\ncc::shared_async<sg::raytracing_pipeline_description> {qualified}::description("
                   "sg::context& ctx, slib::raytracing_host_parts host) const\n{\n")
        out.append("    return slib::describe_raytracing_pipeline(&ctx, &definition(), cc::move(host));\n}\n")
        out.append(f"\nsg::raytracing_shader_table_description {qualified}::table_description("
                   "sg::raytracing_pipeline_handle pipeline, int host_callables)\n{\n")
        out.append("    return slib::table_description(definition(), cc::move(pipeline), host_callables);\n}\n")
        out.append(f"\nsg::hit_row {qualified}::add_row(sg::raytracing_shader_table_description& table, int group)\n{{\n")
        out.append("    return slib::add_hit_group_row(table, group);\n}\n")
    return "".join(out)
