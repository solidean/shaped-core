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
    "float3": ("tg::vec3f", 12, "vec3f"),
    "vec3": ("tg::vec3f", 12, "vec3f"),
    "pos3": ("tg::pos3f", 12, "vec3f"),
    "float4": ("tg::vec4f", 16, "vec4f"),
    "int3": ("tg::vec3i", 12, "vec3i"),
    "mat4": ("tg::mat4f", 64, None),
}


class HostCodeError(Exception):
    """A declaration the compiler accepts and the host has no type for yet."""


def host_type(package: str, where: str, sgl_type: str) -> str:
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
    for file, binding in entries.bindings:
        name = binding["name"]
        if name in taken:
            raise HostCodeError(
                f"shader package '{package}': '{file.path}' declares `binding {name}`, and the generated C++ name "
                f"'{name}' is already {taken[name]}")
        taken[name] = f"`binding {name}` of '{file.path}'"


def includes(entries: SglEntries) -> list[str]:
    """The headers the generated SGL types need, beyond what every package header includes."""
    if not entries.bindings:
        return []
    out = ["<clean-core/container/span.hh>", "<clean-core/container/vector.hh>",
           "<shaped-graphics/binding/binding.hh>", "<shaped-graphics/binding/binding_group.hh>"]
    if any(not b["inline"] for _, b in entries.bindings):
        out.append("<shaped-graphics/resource/views.hh>")
    if any(b["inline"] for _, b in entries.bindings):
        out += ["<clean-core/container/fixed_array.hh>", "<clean-core/fwd.hh>"]

    # Every type a field names: a constant's own, and a buffer's element.
    types = {m["type"] for _, b in entries.bindings for m in b["members"]}
    if "int" in types:
        out.append("<clean-core/fwd.hh>")
    if types & {"float3", "vec3", "float4", "int3"}:
        out.append("<typed-geometry/linalg/vec.hh>")
    if "pos3" in types:
        out.append("<typed-geometry/linalg/pos.hh>")
    if "mat4" in types:
        out.append("<typed-geometry/linalg/mat.hh>")
    return list(dict.fromkeys(out))


# ---- a resource group ----------------------------------------------------------------------------------------------


def emit_group(package: str, namespace: str, file: SglFile, binding: dict) -> str:
    name = binding["name"]
    out = [f"\nnamespace {namespace}\n{{\n"]
    out.append(f"/// `binding {name}` of {file.path}, as the host fills it. Generated; do not edit.\n")
    out.append("///\n")
    out.append("/// It fixes no group index: SGL numbers a group by its position in each entry point's list, so bind it\n")
    out.append("/// with `bind_group(index, group)` at the index the pipeline has it at.\n")
    out.append("///\n")
    out.append(f"///     auto const layout = ctx.cached.acquire_binding_group_layout<{namespace}::{name}>();\n")
    out.append(f"///     auto const g = ctx.transient.create_binding_group(layout, {namespace}::{name}{{...}});\n")
    out.append(f"struct {name}\n{{\n")
    # One field per member, in the shader's order.
    # A buffer's field is the view its access takes, of the element the shader reads, so a read-only view of a `mut`
    # buffer, or a view of another element type, does not compile.
    # A plain member is a plain field: the group owns the constant buffer it lands in, and sg fills it at creation.
    for member in binding["members"]:
        where = f"'{file.path}' `binding {name}` member '{member['name']}'"
        if member["kind"] == "constant":
            out.append(f"    {host_type(package, where, member['type'])} {member['name']}; ///< `{member['type']}`, "
                       f"at byte {member['offset']} of the group's constant buffer\n")
            continue
        element = host_type(package, where, member["type"])
        access = "readwrite" if member["mut"] else "readonly"
        sgl_type = f"mut buffer[{member['type']}]" if member["mut"] else f"buffer[{member['type']}]"
        out.append(f"    sg::{access}_buffer_view<{element}> {member['name']}; ///< `{sgl_type}`\n")
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
    out.append("    /// Empty: an SGL group declares no static sampler yet.\n")
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


def emit_group_impl(package: str, namespace: str, file: SglFile, binding: dict) -> str:
    name = binding["name"]
    qualified = f"{namespace}::{name}"
    buffers = [m for m in binding["members"] if m["kind"] == "buffer"]
    out = [f"\n// `binding {name}` of {file.path}: the table the shader's resources were numbered from.\n"]
    out.append(f"namespace\n{{\nsg::binding const k_sgl_bindings_{name}[] = {{\n")
    if has_block(binding):
        # A uniform block is read in rows of 16 bytes, so that is what the shader reflects its size as.
        size = (binding["block_size"] + 15) // 16 * 16
        out.append(f'    {{.name = "{binding["block_reflected_name"]}", .index = {binding["block_slot"]}u, .count = 1u, '
                   f".type = sg::binding_type::uniform_buffer, .block_size = {size}}},\n")
    for member in buffers:
        kind = "readwrite_structured_buffer" if member["mut"] else "readonly_structured_buffer"
        out.append(f'    {{.name = "{member["reflected_name"]}", .index = {member["slot"]}u, .count = 1u, '
                   f".type = sg::binding_type::{kind}}},\n")
    out.append("};\n} // namespace\n")

    out.append(f"\ncc::span<sg::binding const> {qualified}::declared_bindings()\n{{\n")
    out.append(f"    return k_sgl_bindings_{name};\n}}\n")
    out.append(f"\ncc::span<sg::named_sampler const> {qualified}::declared_samplers()\n{{\n    return {{}};\n}}\n")

    out.append(f"\nvoid {qualified}::gather(cc::vector<sg::slotted_view>& views,\n")
    out.append(" " * (len("void ") + len(qualified) + len("::gather(")))
    out.append("cc::vector<sg::named_sampler>& samplers) const\n{\n")
    out.append("    (void)samplers;\n")
    out.append(f"    views.reserve({len(buffers)});\n")
    for member in buffers:
        out.append(f"    views.push_back({{.slot = sg::binding_slot({member['slot']}), .view = {member['name']}}});\n")
    out.append("}\n")

    if has_block(binding):
        out.append(f"\nvoid {qualified}::write_constants(cc::span<cc::byte> block) const\n{{\n")
        out.append("    CC_ASSERT(block.size() >= constants_size, \"the block is smaller than the group's constant buffer\");\n")
        out.append(pack_members(package, file, name, binding))
        out.append("}\n")
    return "".join(out)


def pack_members(package: str, file: SglFile, name: str, binding: dict) -> str:
    """Each plain member copied to the offset the compiler placed it at, its size checked against the shader's."""
    out = []
    for member in binding["members"]:
        if member["kind"] != "constant":
            continue
        cpp = host_type(package, f"'{file.path}' `binding {name}` member '{member['name']}'", member["type"])
        size = HOST_TYPES[member["type"]][1]
        if size != member["size"]:
            raise HostCodeError(
                f"shader package '{package}': '{file.path}' `binding {name}` member '{member['name']}' is "
                f"{member['size']} bytes in the shader and {size} as {cpp}")
        out.append(f"    static_assert(sizeof({cpp}) == {size});\n")
        out.append(f"    cc::memcpy(block.data() + {member['offset']}, &{member['name']}, {size});\n")
    return "".join(out)


# ---- an @inline block ----------------------------------------------------------------------------------------------


def emit_inline(package: str, namespace: str, file: SglFile, binding: dict) -> str:
    """An `@inline binding`: a plain struct in C++'s own layout, and the block the shader reads, packed from it.

    The struct does not mirror the shader's padding, because it is never handed over as it is.
    `to_block` writes each member where the compiler placed it, which is the one layout the shader reads.
    """
    name = binding["name"]
    out = [f"\nnamespace {namespace}\n{{\n"]
    out.append(f"/// `@inline binding {name}` of {file.path}: the inline constants, in C++'s own layout. Generated; do not edit.\n")
    out.append("///\n")
    out.append("/// Hand the shader `to_block()`, never the struct: the block is laid out as the shader reads it.\n")
    out.append("///\n")
    out.append(f"///     pass.set_inline_constants({namespace}::{name}{{...}}.to_block());\n")
    out.append(f"struct {name}\n{{\n")
    for member in binding["members"]:
        cpp = host_type(package, f"'{file.path}' `@inline binding {name}` member '{member['name']}'", member["type"])
        out.append(f"    {cpp} {member['name']}; ///< `{member['type']}`, at byte {member['offset']} of the block\n")
    out.append("\n")
    out.append("    /// The block's size in bytes, as the shader lays it out.\n")
    out.append(f"    static constexpr cc::isize block_size = {binding['block_size']};\n")
    out.append("\n")
    out.append("    /// This value as the shader reads it: every member at the offset the compiler placed it, the rest zero.\n")
    out.append("    [[nodiscard]] cc::fixed_array<cc::byte, block_size> to_block() const;\n")
    out.append("};\n")
    out.append(f"}} // namespace {namespace}\n")
    return "".join(out)


def emit_inline_impl(package: str, namespace: str, file: SglFile, binding: dict) -> str:
    name = binding["name"]
    qualified = f"{namespace}::{name}"
    out = [f"\ncc::fixed_array<cc::byte, {qualified}::block_size> {qualified}::to_block() const\n{{\n"]
    out.append("    auto block = cc::fixed_array<cc::byte, block_size>{};\n")
    out.append(pack_members(package, file, name, binding))
    out.append("    return block;\n}\n")
    return "".join(out)


# ---- the package's share of the header and the source ---------------------------------------------------------------


def emit_header(package: str, namespace: str, entries: SglEntries) -> str:
    out = []
    for file, binding in entries.bindings:
        if binding["inline"]:
            out.append(emit_inline(package, namespace, file, binding))
        else:
            out.append(emit_group(package, namespace, file, binding))
    return "".join(out)


def emit_source(package: str, namespace: str, entries: SglEntries) -> str:
    out = []
    for file, binding in entries.bindings:
        if binding["inline"]:
            out.append(emit_inline_impl(package, namespace, file, binding))
        else:
            out.append(emit_group_impl(package, namespace, file, binding))
    return "".join(out)
