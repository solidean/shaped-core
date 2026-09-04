#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Generate a shader package's C++ from a manifest, as sc_add_shader_package's build step.

    uv run generate_shader_package.py --manifest <path> --out-dir <dir>

Only those two paths come in; everything else is read from the manifest, which sc_add_shader_package writes.

Emits, into --out-dir:
    <name>.hh  the typed symbols call sites use, the generated binding groups, and package()
    <name>.cc  the globals, the definition table, every source file embedded, and the self-check
    <name>.d   a depfile naming every file read, so editing an .hlsli regenerates

This runs at BUILD time, not configure time.
The .cc embeds shader source, so editing a shader must regenerate it; doing this at configure time would
silently ship a stale copy from an incremental build.

Output is LF-terminated whatever the platform, so a package generated on Windows and one generated on Linux
are the same bytes.

Python rather than a C++ host tool: a host tool complicates building and CI, and cross-compiling would mean
building it for the host while the library builds for the target.
The cost is that the binding grammar exists twice -- see binding_grammar.py for what keeps the two in step.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from binding_grammar import (BindingError, DeclaredSampler, Group, InlineConstants, Payload,  # noqa: E402
                             StructMember, VALUE_TYPES, VertexInput, parse_binding_groups)

# How each sg::sampler field is spelled in C++, and the order sg::sampler declares them in -- a designated
# initializer has to follow the declaration order, so the order here is load-bearing.
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
    ("border_color", "sg::sampler_border_color::{}"),
)


def emit_sampler(sampler: DeclaredSampler) -> str:
    """One sg::sampler as a designated initializer, carrying only the fields the shader named."""
    parts = [pattern.format(sampler.fields[key]) for key, pattern in SAMPLER_FIELDS if key in sampler.fields]
    named = [f".{key} = {value}"
             for (key, _), value in zip([f for f in SAMPLER_FIELDS if f[0] in sampler.fields], parts)]
    return "{" + ", ".join(named) + "}"

# Kept in step with sg::shader_stage (shaped-graphics/binding/compiled_shader.hh). The DSL spells stages
# exactly as the enum does, so the generator emits the enumerator rather than a string for C++ to parse back.
VALID_STAGES = (
    "vertex", "tessellation_control", "tessellation_evaluation", "geometry", "fragment", "compute",
    "raygen", "closest_hit", "any_hit", "miss", "intersection", "callable",
)

VALID_LANGUAGES = ("hlsl",)

# The stage words that declare something other than an entry point.
BINDING_STAGE = "binding"
VERTEX_INPUT_STAGE = "vertex_input"
PAYLOAD_STAGE = "payload"
CONSTANTS_STAGE = "constants"

# Sources are embedded as raw string literals so a shipped binary carries its own shaders and needs no
# source tree.
# A delimiter keeps a shader containing )" from ending the literal early.
#
# MSVC caps ONE string literal at 16380 bytes and truncates past it with C2026, which a path tracer's worth
# of HLSL passes.
# Adjacent literals are concatenated only *after* that per-literal limit applies, so every source goes out as
# a run of chunks.
# Clang and gcc have no such cap, and do not care either way.
# What survives chunking is MSVC's 65535-byte ceiling on the concatenated result, the real per-shader budget.
#
# Splitting on byte offsets rather than lines is safe, and deliberately so.
# `)slibsrc"` may not appear in the source at all, so no boundary can grow one.
# Nothing requires a chunk to be a whole line either -- concatenation restores the exact byte sequence.
EMBED_CHUNK_BYTES = 8000


class GeneratorError(Exception):
    """A declaration the generator will not accept.

    Printed and turned into a non-zero exit.
    """


@dataclass
class Manifest:
    name: str = ""
    namespace: str = ""
    source_dir: Path = Path()
    language: str = "hlsl"
    shaders: list[str] = field(default_factory=list)


@dataclass
class ShaderFile:
    """One source file and the entry points declared on it, grouped for emission."""

    stem: str  # the C++ identifier the path folds into
    path: str  # as declared, verbatim
    stages: dict[str, list[str]] = field(default_factory=dict)  # stage -> entry points, in declaration order


@dataclass
class BindingEntry:
    """One `path:binding:namespace` entry: a group to generate a struct for."""

    path: str
    namespace: str
    group: Group


@dataclass
class VertexInputEntry:
    """One `path:vertex_input:struct` entry: a vertex input struct to mirror in C++."""

    path: str
    input: VertexInput


@dataclass
class PayloadEntry:
    """One `path:payload:struct` entry: a ray payload to mirror in C++, and to size."""

    path: str
    payload: Payload


@dataclass
class ConstantsEntry:
    """One `path:constants:name` entry: an inline-constants block to mirror in C++."""

    path: str
    constants: InlineConstants


def read_manifest(path: Path) -> Manifest:
    """The manifest is `key=value` lines; SHADER= repeats, one per declared entry."""
    manifest = Manifest()
    for line in path.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if not sep or not key.isupper():
            continue
        if key == "NAME":
            manifest.name = value
        elif key == "NAMESPACE":
            manifest.namespace = value
        elif key == "SOURCE_DIR":
            manifest.source_dir = Path(value)
        elif key == "LANGUAGE":
            manifest.language = value
        elif key == "SHADER":
            manifest.shaders.append(value)
    return manifest


def identifier_of(path: str) -> str:
    """The path as one C++ identifier, directory included.

    post-process/manga_render.hlsl becomes post_process_manga_render, since '/' and '-' both map to '_'.
    """
    stem = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
    directory = path.rsplit("/", 1)[0] if "/" in path else ""
    ident = stem.replace("-", "_")
    if directory:
        ident = directory.replace("-", "_").replace("/", "_") + "_" + ident
    return ident


@dataclass
class Entries:
    """Everything a package's manifest declares, grouped by what it generates."""

    files: list[ShaderFile] = field(default_factory=list)
    bindings: list[BindingEntry] = field(default_factory=list)
    vertex_inputs: list[VertexInputEntry] = field(default_factory=list)
    payloads: list[PayloadEntry] = field(default_factory=list)
    constants: list[ConstantsEntry] = field(default_factory=list)

    @property
    def paths(self) -> list[str]:
        """Every file an entry names, which is what the embed closure starts from."""
        return ([f.path for f in self.files] + [b.path for b in self.bindings]
                + [v.path for v in self.vertex_inputs] + [p.path for p in self.payloads]
                + [c.path for c in self.constants])


def parse_entries(manifest: Manifest) -> Entries:
    entries = Entries()
    files = entries.files
    by_stem: dict[str, ShaderFile] = {}
    bindings = entries.bindings
    vertex_inputs = entries.vertex_inputs
    payloads = entries.payloads
    seen: set[str] = set()
    name = manifest.name

    for entry in manifest.shaders:
        parts = entry.split(":")
        if len(parts) != 3:
            raise GeneratorError(
                f"shader package '{name}': entry '{entry}' must be path:stage:entry_point, "
                f"path:{BINDING_STAGE}:namespace, path:{VERTEX_INPUT_STAGE}:struct, "
                f"path:{PAYLOAD_STAGE}:struct or path:{CONSTANTS_STAGE}:name")

        path, stage, tail = parts
        if (stage not in (BINDING_STAGE, VERTEX_INPUT_STAGE, PAYLOAD_STAGE, CONSTANTS_STAGE)
                and stage not in VALID_STAGES):
            raise GeneratorError(
                f"shader package '{name}': entry '{entry}' has unknown stage '{stage}'. "
                f"Stages are spelled as sg::shader_stage: {' '.join(VALID_STAGES)}")

        source = manifest.source_dir / path
        if not source.is_file():
            raise GeneratorError(f"shader package '{name}': '{path}' does not exist under {manifest.source_dir}")

        if entry in seen:
            raise GeneratorError(f"shader package '{name}': entry '{entry}' is declared twice")
        seen.add(entry)

        if stage == BINDING_STAGE:
            bindings.append(read_binding_entry(manifest, path, tail))
            continue

        if stage == VERTEX_INPUT_STAGE:
            vertex_inputs.append(read_vertex_input_entry(manifest, path, tail))
            continue

        if stage == PAYLOAD_STAGE:
            payloads.append(read_payload_entry(manifest, path, tail))
            continue

        if stage == CONSTANTS_STAGE:
            entries.constants.append(read_constants_entry(manifest, path, tail))
            continue

        stem = identifier_of(path)
        existing = by_stem.get(stem)
        if existing is None:
            existing = ShaderFile(stem, path)
            by_stem[stem] = existing
            files.append(existing)
        elif existing.path != path:
            # Two files that collapse to one identifier would emit the same struct twice; say so here rather
            # than in a wall of C++ redefinition errors.
            raise GeneratorError(
                f"shader package '{name}': '{path}' and '{existing.path}' both map to the C++ identifier '{stem}'")

        existing.stages.setdefault(stage, []).append(tail)

    return entries


def read_binding_entry(manifest: Manifest, path: str, namespace: str) -> BindingEntry:
    """The one group `namespace` names, parsed out of `path` alone.

    A binding entry generates from the named file and never from its includes: an .hlsli that declares a
    group is registered on its own, in whichever package owns it, or every shader including it would generate
    the same struct again.
    That works because a namespace's numbering is local to its single block in its single file, which is the
    invariant the pass enforces.
    """
    text = (manifest.source_dir / path).read_text(encoding="utf-8")
    try:
        groups = parse_binding_groups(text).groups
    except BindingError as e:
        raise GeneratorError(f"shader package '{manifest.name}': {path}: {e}") from e

    for group in groups:
        if group.name == namespace:
            return BindingEntry(path, namespace, group)

    declared = ", ".join(g.name for g in groups) or "none"
    raise GeneratorError(
        f"shader package '{manifest.name}': '{path}' declares no binding group named '{namespace}' "
        f"(it declares: {declared})")


def read_vertex_input_entry(manifest: Manifest, path: str, struct: str) -> VertexInputEntry:
    """The one vertex input struct `struct` names, parsed out of `path` alone.

    Per file and never through its includes, for the same reason a binding entry is.
    """
    text = (manifest.source_dir / path).read_text(encoding="utf-8")
    try:
        inputs = parse_binding_groups(text).vertex_inputs
    except BindingError as e:
        raise GeneratorError(f"shader package '{manifest.name}': {path}: {e}") from e

    for candidate in inputs:
        if candidate.name == struct:
            return VertexInputEntry(path, candidate)

    declared = ", ".join(i.name for i in inputs) or "none"
    raise GeneratorError(
        f"shader package '{manifest.name}': '{path}' declares no vertex input struct named '{struct}' "
        f"(it declares: {declared})")


def read_payload_entry(manifest: Manifest, path: str, struct: str) -> PayloadEntry:
    """The one ray payload `struct` names, parsed out of `path` alone."""
    text = (manifest.source_dir / path).read_text(encoding="utf-8")
    try:
        payloads = parse_binding_groups(text).payloads
    except BindingError as e:
        raise GeneratorError(f"shader package '{manifest.name}': {path}: {e}") from e

    for candidate in payloads:
        if candidate.name == struct:
            return PayloadEntry(path, candidate)

    declared = ", ".join(p.name for p in payloads) or "none"
    raise GeneratorError(
        f"shader package '{manifest.name}': '{path}' declares no payload struct named '{struct}' "
        f"(it declares: {declared})")


def read_constants_entry(manifest: Manifest, path: str, name: str) -> ConstantsEntry:
    """The inline-constants block `name` names, parsed out of `path` alone."""
    text = (manifest.source_dir / path).read_text(encoding="utf-8")
    try:
        constants = parse_binding_groups(text).inline_constants
    except BindingError as e:
        raise GeneratorError(f"shader package '{manifest.name}': {path}: {e}") from e

    if constants is None:
        raise GeneratorError(
            f"shader package '{manifest.name}': '{path}' declares no inline-constants block")
    if constants.name != name:
        raise GeneratorError(
            f"shader package '{manifest.name}': '{path}' declares an inline-constants block named "
            f"'{constants.name}', not '{name}'")

    return ConstantsEntry(path, constants)


def include_closure(manifest: Manifest, roots: list[str]) -> list[str]:
    """Every file the package has to embed: the declared shaders plus what they `#include`.

    A shipped binary reads its shaders from the embedded copy, and ssc::dxc has no filesystem fallback for
    #include -- an unresolved one is a hard error.
    So every file a shader pulls in has to be embedded too.

    Scanning for `#include "..."` is a deliberate approximation: it does not know about #if, so it
    over-approximates and embeds a file an #ifdef would have skipped.
    Over-approximating costs binary size; under-approximating would break the shipped build, so the bias is
    the right way up.
    """
    files = list(dict.fromkeys(roots))
    queue = list(files)

    while queue:
        current = queue.pop(0)
        text = (manifest.source_dir / current).read_text(encoding="utf-8")

        for line in text.splitlines():
            stripped = line.lstrip()
            if not stripped.startswith("#"):
                continue
            after_hash = stripped[1:].lstrip()
            if not after_hash.startswith("include"):
                continue
            rest = after_hash[len("include"):].lstrip()
            if not rest.startswith('"'):
                continue
            closing = rest.find('"', 1)
            if closing < 0:
                continue
            include_path = rest[1:closing]

            # Next to the including file, then the package root.
            # A path that resolves to neither lives in another mount (a shared library) and is not ours to embed.
            #
            # Deliberately wider than slib::shader_library, which resolves every include from the entry
            # shader's own directory at any depth.
            # Embedding a file the runtime would not have found costs binary size; the reverse would break
            # the shipped build.
            directory = current.rsplit("/", 1)[0] if "/" in current else ""
            sibling = f"{directory}/{include_path}" if directory else include_path

            resolved = None
            if (manifest.source_dir / sibling).is_file():
                resolved = sibling
            elif (manifest.source_dir / include_path).is_file():
                resolved = include_path

            if resolved is not None and resolved not in files:
                files.append(resolved)
                queue.append(resolved)

    return files


def embed_literal(text: str) -> str:
    """One shader's source as a run of adjacent raw string literals -- see EMBED_CHUNK_BYTES."""
    raw = text.encode("utf-8")
    if not raw:
        # An empty shader file still has to yield a literal rather than nothing.
        return ' R"slibsrc()slibsrc"'

    chunks = [raw[i:i + EMBED_CHUNK_BYTES] for i in range(0, len(raw), EMBED_CHUNK_BYTES)]
    return "".join(f'\n    R"slibsrc({chunk.decode("utf-8", "surrogateescape")})slibsrc"' for chunk in chunks)


def emit_mirror_members(members: list[StructMember]) -> str:
    """The members of a mirror struct, naturally packed the way both HLSL and C++ pack them."""
    out = []
    for member in members:
        cpp_type, _, _ = VALUE_TYPES[member.type]
        if cpp_type.endswith("]"):
            base, _, extent = cpp_type.partition("[")
            out.append(f"    {base} {member.name}[{extent[:-1]}];\n")
        else:
            out.append(f"    {cpp_type} {member.name};\n")
    return "".join(out)


def emit_mirror_asserts(qualified: str, name: str, members: list[StructMember], what: str) -> str:
    """Size and every member's offset, against what the generator computed.

    Size alone would pass a mirror whose fields are in the wrong places and whose padding happens to add up,
    which is exactly the failure a hand-written mirror has today.
    """
    out = []
    total = sum(VALUE_TYPES[m.type][1] for m in members)
    out.append(f"\nstatic_assert(sizeof({qualified}) == {total},\n")
    out.append(f'              "{name} is not the size its {what} states");\n')

    offset = 0
    for member in members:
        out.append(f"static_assert(offsetof({qualified}, {member.name}) == {offset},\n")
        out.append(f'              "{name}::{member.name} is not where its {what} states");\n')
        offset += VALUE_TYPES[member.type][1]
    return "".join(out)


def emit_constants(manifest: Manifest, entry: ConstantsEntry) -> str:
    """The C++ mirror of an inline-constants block, with the padding its HLSL layout needs.

    A constant block's layout is DXC's rather than C++'s, so unlike a vertex input or a payload this mirror
    reproduces a layout instead of defining one -- the padding is where a hand-written mirror goes quietly
    wrong, and the spike's Q14 is where each rule came from.
    """
    constants = entry.constants
    qualified = f"{manifest.namespace}::{constants.type}"

    out = [f"\nnamespace {manifest.namespace}\n{{\n"]
    out.append(f"/// The inline constants {constants.name} carries. Generated from {entry.path}; do not edit.\n")
    out.append("///\n")
    out.append("/// The padding is HLSL's constant-buffer layout, not C++'s: an element may not straddle a\n")
    out.append("/// 16-byte row, and a row is filled before it is left.\n")
    out.append(f"struct {constants.type}\n{{\n")

    offset = 0
    pad_index = 0
    for member in constants.members:
        if member.offset > offset:
            words = (member.offset - offset) // 4
            out.append(f"    unsigned _padding{pad_index}[{words}];\n")
            pad_index += 1
            offset = member.offset

        cpp_type, size, _ = VALUE_TYPES[member.type]
        if cpp_type.endswith("]"):
            base, _, extent = cpp_type.partition("[")
            out.append(f"    {base} {member.name}[{extent[:-1]}];\n")
        else:
            out.append(f"    {cpp_type} {member.name};\n")
        offset += size

    # The block's own total rounds up to a whole row, and the mirror has to carry that tail: a CBV is sized in
    # whole rows, so a shorter struct would upload fewer bytes than the shader reads.
    if constants.size > offset:
        out.append(f"    unsigned _padding{pad_index}[{(constants.size - offset) // 4}];\n")

    out.append("};\n")
    out.append(f"}} // namespace {manifest.namespace}\n")

    out.append(f"\nstatic_assert(sizeof({qualified}) == {constants.size},\n")
    out.append(f'              "{constants.type} is not the size its constant block states");\n')
    for member in constants.members:
        out.append(f"static_assert(offsetof({qualified}, {member.name}) == {member.offset},\n")
        out.append(f'              "{constants.type}::{member.name} is not where its constant block states");\n')

    return "".join(out)


def emit_payload(manifest: Manifest, entry: PayloadEntry) -> str:
    """The C++ struct a ray payload mirrors, and the max_payload_size a pipeline must declare for it.

    A payload packs at natural alignment rather than in a constant buffer's 16-byte rows -- the spike's Q13
    measured that rather than assuming it -- and natural packing is what C++ does, so the mirror needs no
    padding members at all.
    The static_asserts below are what says so rather than hoping it.
    """
    payload = entry.payload
    qualified = f"{manifest.namespace}::{payload.name}"

    out = [f"\nnamespace {manifest.namespace}\n{{\n"]
    out.append(f"/// The ray payload {payload.name} carries. Generated from {entry.path}; do not edit.\n")
    out.append(f"struct {payload.name}\n{{\n")
    out.append(emit_mirror_members(payload.members))
    out.append("\n")
    out.append("    /// What raytracing_pipeline_description::max_payload_size must be for this payload.\n")
    out.append("    /// The driver refuses a smaller one, which is what the spike's Q13 measured.\n")
    out.append(f"    static constexpr cc::isize max_payload_size = {payload.size};\n")
    out.append("};\n")
    out.append(f"}} // namespace {manifest.namespace}\n")
    out.append(emit_mirror_asserts(qualified, payload.name, payload.members, "payload size"))
    return "".join(out)


def emit_vertex_input(manifest: Manifest, entry: VertexInputEntry) -> str:
    """The C++ struct a vertex input mirrors, plus the sg::vertex_layout_of that describes it.

    The mirror is what defines the buffer's byte layout, so nothing here models HLSL's own packing: a vertex
    buffer is a byte stream the input assembler decodes per attribute offset, and this struct is what a caller
    fills.
    Everything the specialization states is already in the shader -- the semantic from the member, the format
    from its type, the offset from the mirror, the stride from its total.
    """
    vertex_input = entry.input
    qualified = f"{manifest.namespace}::{vertex_input.name}"

    out = [f"\nnamespace {manifest.namespace}\n{{\n"]
    out.append(f"/// The vertex buffer {vertex_input.name} reads. Generated from {entry.path}; do not edit.\n")
    out.append(f"struct {vertex_input.name}\n{{\n")
    out.append(emit_mirror_members(vertex_input.members))
    out.append("};\n")
    out.append(f"}} // namespace {manifest.namespace}\n")

    # The mirror is naturally packed, so its stride is its size -- but say so rather than assume it, since a
    # wrong stride draws geometry rather than failing.
    out.append(emit_mirror_asserts(qualified, vertex_input.name, vertex_input.members, "vertex layout"))

    out.append(f"\n/// What sg::vertex_input_layout::create<{qualified}>() reads.\n")
    out.append("template <>\n")
    out.append(f"struct sg::vertex_layout_of<{qualified}>\n{{\n")
    out.append("    [[nodiscard]] static sg::vertex_type_layout get()\n    {\n")
    out.append(f"        return {{.stride = sizeof({qualified}),\n")
    out.append(f"                .per_instance = {'true' if vertex_input.per_instance else 'false'},\n")
    out.append("                .attributes = {\n")
    for member in vertex_input.members:
        _, _, fmt = VALUE_TYPES[member.type]
        out.append(f'                    {{.semantic = "{member.semantic}",\n')
        out.append(f"                     .semantic_index = {member.semantic_index},\n")
        out.append(f"                     .format = sg::vertex_attribute_format::{fmt},\n")
        out.append(f"                     .offset = offsetof({qualified}, {member.name})}},\n")
    out.append("                }};\n")
    out.append("    }\n")
    out.append("};\n")
    return "".join(out)


def emit_header(manifest: Manifest, entries: Entries) -> str:
    files = entries.files
    bindings = entries.bindings
    vertex_inputs = entries.vertex_inputs
    payloads = entries.payloads
    constants = entries.constants
    out = ["// This file is auto-generated by sc_add_shader_package. Do not edit.\n"]
    out.append("#pragma once\n\n")
    out.append("#include <shaped-shader-library/fwd.hh>\n")
    out.append("#include <shaped-shader-library/shader_asset.hh>\n")
    out.append("#include <shaped-shader-library/shader_package.hh>\n")
    if vertex_inputs:
        out.append("\n#include <shaped-graphics/raster/vertex_input.hh>\n")
    if payloads:
        out.append("\n#include <clean-core/fwd.hh> // cc::isize\n")
    if vertex_inputs or payloads or constants:
        out.append("\n#include <cstddef> // offsetof\n")
    if bindings:
        out.append("\n#include <clean-core/container/span.hh>\n")
        out.append("#include <clean-core/error/result.hh>\n")
        out.append("#include <clean-core/string/string.hh>\n")
        out.append("#include <shaped-graphics/binding/binding.hh>\n")
        out.append("#include <shaped-graphics/binding/binding_group.hh>\n")
        out.append("#include <shaped-graphics/binding/binding_group_layout.hh>\n")
        out.append("#include <shaped-graphics/context/context.hh>\n")
        out.append("#include <shaped-graphics/resource/views.hh>\n")
    out.append(f"\nnamespace {manifest.namespace}\n{{\n")

    for file in files:
        out.append(f"/// {file.path}\n")
        out.append(f"struct {file.stem}_t\n{{\n")
        for stage, entry_points in file.stages.items():
            out.append("    struct\n    {\n")
            for entry_point in entry_points:
                out.append(f"        slib::shader_asset_handle {entry_point};\n")
            out.append(f"    }} {stage};\n")
        out.append("};\n")
        out.append(f"extern {file.stem}_t {file.stem};\n\n")

    out.append("/// Pass to slib::shader_library::add_package. The handles above are null until you do.\n")
    out.append("slib::shader_package const& package();\n")
    out.append(f"}} // namespace {manifest.namespace}\n")

    for entry in bindings:
        out.append(emit_binding_group(manifest, entry))
    for entry in vertex_inputs:
        out.append(emit_vertex_input(manifest, entry))
    for entry in payloads:
        out.append(emit_payload(manifest, entry))
    for entry in constants:
        out.append(emit_constants(manifest, entry))

    return "".join(out)


def emit_binding_group(manifest: Manifest, entry: BindingEntry) -> str:
    """The struct one annotated namespace becomes: one named member per binding, plus its layout and group."""
    group = entry.group
    out = [f"\nnamespace {manifest.namespace}::{group.name}\n{{\n"]
    out.append(f"/// The bindings {group.name} declares, in slot order. Generated from {entry.path}; do not edit.\n")
    out.append("///\n")
    out.append("/// Every address here is a constant rather than something reflected, because the same parse that\n")
    out.append("/// wrote the addresses into the shader produced this table.\n")
    out.append("struct group\n{\n")

    static_names = {s.name for s in group.static_samplers}
    for binding in group.bindings:
        if binding.type == "sampler":
            # A sampler the shader marked `static` is baked into the layout, so the group has no field for it.
            if binding.name not in static_names:
                out.append(f"    sg::sampler {binding.name};\n")
        else:
            out.append(f"    sg::bound_view {binding.name};\n")

    out.append("\n")
    out.append("    /// The group index the attribute gave, so no call site writes the number.\n")
    out.append(f"    static constexpr sg::u32 group_index = {group.group};\n")
    out.append("\n")
    out.append("    /// The declared bindings, in slot order — the whole table, not a stage's reflected subset.\n")
    out.append("    [[nodiscard]] static cc::span<sg::binding const> declared_bindings();\n")
    out.append("\n")
    out.append("    /// The samplers the shader declared `static`, in declaration order.\n")
    out.append("    /// Baked into the pipeline layout's root signature, so they cost no per-group descriptor.\n")
    out.append("    [[nodiscard]] static cc::span<sg::named_sampler const> declared_samplers();\n")
    out.append("\n")
    out.append("    /// The layout these declarations define — constant, so no reflection is consulted.\n")
    out.append("    [[nodiscard]] static sg::binding_group_layout_handle acquire_layout(sg::context& ctx);\n")
    out.append("\n")
    out.append("    /// The same, plus static samplers for the ones the shader left undeclared.\n")
    out.append("    /// A declared sampler wins: supplying one the shader already declared is a mistake, not an\n")
    out.append("    /// override, and it is dropped with an assertion rather than quietly taking effect.\n")
    out.append("    [[nodiscard]] static sg::binding_group_layout_handle acquire_layout(\n")
    out.append("        sg::context& ctx, cc::span<sg::named_sampler const> samplers);\n")
    out.append("\n")
    out.append("    /// Builds a group from the fields above.\n")
    out.append("    [[nodiscard]] cc::result<sg::binding_group_handle> create(sg::context& ctx) const;\n")
    out.append("\n")
    out.append("    /// Empty while the table above still describes the shader it was generated from.\n")
    out.append("    ///\n")
    out.append("    /// Parses the embedded source with the runtime pass and compares. The two halves of the pass are\n")
    out.append("    /// two readings of one grammar, and this is the leg that covers what we actually ship — the\n")
    out.append("    /// shared corpus covers what we thought of.\n")
    out.append("    [[nodiscard]] static cc::string self_check();\n")
    out.append("};\n")
    out.append(f"}} // namespace {manifest.namespace}::{group.name}\n")
    return "".join(out)


def emit_source(manifest: Manifest, files: list[ShaderFile], bindings: list[BindingEntry],
                embedded: list[str]) -> str:
    out = ["// This file is auto-generated by sc_add_shader_package. Do not edit.\n\n"]
    out.append(f'#include "{manifest.name}.hh"\n\n')
    out.append("#include <shaped-graphics/binding/compiled_shader.hh>\n")
    if bindings:
        out.append("#include <clean-core/common/assert.hh>\n")
        out.append("#include <clean-core/container/vector.hh>\n")
        out.append("#include <clean-core/string/format.hh>\n")
        out.append("#include <shaped-shader-library/binding/binding_groups.hh>\n")
    out.append("\n")

    for file in files:
        out.append(f"{manifest.namespace}::{file.stem}_t {manifest.namespace}::{file.stem};\n")
    out.append("\nnamespace\n{\n")

    entries = []
    for index, path in enumerate(embedded):
        text = (manifest.source_dir / path).read_text(encoding="utf-8")
        out.append(f"constexpr char const* k_source_{index} ={embed_literal(text)};\n")
        entries.append(f'{{.path = "{path}", .text = k_source_{index}}}')

    out.append("\nconstexpr slib::embedded_file k_embedded_files[] = {\n")
    for entry in entries:
        out.append(f"    {entry},\n")
    out.append("};\n")

    # The definition table carries the declared path through verbatim rather than rebuilding it from the
    # folder and stem, and emits the sg::shader_stage enumerator rather than a string to parse back.
    out.append("\nslib::shader_definition const k_definitions[] = {\n")
    for file in files:
        for stage, points in file.stages.items():
            for point in points:
                out.append(f'    {{.path = "{file.path}",\n')
                out.append(f"     .stage = sg::shader_stage::{stage},\n")
                out.append(f'     .entry_point = "{point}",\n')
                out.append(f"     .asset = &{manifest.namespace}::{file.stem}.{stage}.{point}}},\n")
    out.append("};\n")

    for entry in bindings:
        out.append(emit_binding_table(entry, embedded))

    out.append("} // namespace\n\n")

    # The absolute source dir is baked in.
    # A dev build finds it and hot-reloads; a shipped build does not and falls back to the embedded copy above.
    # No mode flag, no probing for "am I installed".
    out.append(f"slib::shader_package const& {manifest.namespace}::package()\n{{\n")
    out.append("    static slib::shader_package const pkg = {\n")
    out.append(f'        .name = "{manifest.name}",\n')
    out.append(f"        .language = slib::shader_language::{manifest.language},\n")
    out.append(f'        .source_dir = "{manifest.source_dir.as_posix()}",\n')
    out.append("        .embedded_files = k_embedded_files,\n")
    out.append("        .definitions = k_definitions,\n")
    out.append("    };\n")
    out.append("    return pkg;\n")
    out.append("}\n")

    for entry in bindings:
        out.append(emit_binding_group_impl(manifest, entry, embedded))

    return "".join(out)


def emit_binding_table(entry: BindingEntry, embedded: list[str]) -> str:
    """The constant binding table one group defines, plus the self-check that it still describes the source."""
    group = entry.group
    ident = f"{group.name}"
    out = [f"\n// {entry.path}, namespace {group.name} — the table the shader's own addresses were written from.\n"]
    out.append(f"sg::binding const k_bindings_{ident}[] = {{\n")
    for binding in group.bindings:
        out.append("    {" + f'.name = "{binding.name}",\n')
        out.append(f"     .group_index = {group.group}u,\n")
        out.append(f"     .space = {group.group}u,\n")
        out.append(f"     .index = {binding.index}u,\n")
        out.append(f"     .count = {binding.count}u,\n")
        out.append(f"     .type = sg::binding_type::{binding.type},\n")
        if binding.dimension is not None:
            out.append(f"     .texture_dimension = sg::texture_view_dimension::{binding.dimension},\n")
        out.append("    },\n")
    out.append("};\n")

    if group.static_samplers:
        out.append(f"\nsg::named_sampler const k_static_samplers_{ident}[] = {{\n")
        for sampler in group.static_samplers:
            out.append(f'    {{.name = "{sampler.name}", .sampler = {emit_sampler(sampler)}}},\n')
        out.append("};\n")

    return "".join(out)


def emit_self_check(manifest: Manifest, entry: BindingEntry, embedded: list[str]) -> str:
    """The check that the C++ parse of the embedded source agrees with the table Python produced from it.

    Two parsers exist -- this generator's and the runtime rewriter's -- and the package already carries the
    exact bytes both read.
    So its corpus is every shader anyone declares, and it grows without anyone remembering to extend it.
    """
    group = entry.group
    source_index = embedded.index(entry.path)
    qualified = f"{manifest.namespace}::{group.name}::group"
    out = [f"\ncc::string {qualified}::self_check()\n{{\n"]
    out.append(f"    auto const groups = slib::parse_binding_groups(k_source_{source_index});\n")
    out.append("    if (groups.has_error())\n")
    out.append(f'        return cc::format("{entry.path}: {{}}", groups.error().to_string());\n')
    out.append("\n")
    out.append("    for (auto const& parsed : groups.value().groups)\n")
    out.append("    {\n")
    out.append(f'        if (parsed.name != "{group.name}")\n')
    out.append("            continue;\n")
    out.append("\n")
    out.append(f"        if (parsed.group != {group.group}u)\n")
    out.append(f'            return cc::format("{group.name}: group {{}}, the table says {group.group}", parsed.group);\n')
    out.append(f"        if (parsed.bindings.size() != {len(group.bindings)})\n")
    out.append(f'            return cc::format("{group.name}: {{}} binding(s), the table says {len(group.bindings)}",\n')
    out.append("                              parsed.bindings.size());\n")
    out.append("\n")
    out.append(f"        auto const table = cc::span<sg::binding const>(k_bindings_{group.name});\n")
    out.append("        for (cc::isize i = 0; i < parsed.bindings.size(); ++i)\n")
    out.append("        {\n")
    out.append("            auto const& a = parsed.bindings[i];\n")
    out.append("            auto const& b = table[i];\n")
    out.append("            if (a.name != b.name || a.index != b.index || a.count != b.count || a.type != b.type\n")
    out.append("                || a.texture_dimension != b.texture_dimension || a.group_index != b.group_index\n")
    out.append("                || a.space != b.space)\n")
    out.append(f'                return cc::format("{group.name}: binding {{}} reads as \'{{}}\', the table says \'{{}}\'",\n')
    out.append("                                  i, a.name, b.name);\n")
    out.append("        }\n")
    out.append("\n")
    out.append("        return cc::string();\n")
    out.append("    }\n")
    out.append("\n")
    out.append(f'    return cc::string("{entry.path} declares no binding group named {group.name}");\n')
    out.append("}\n")
    return "".join(out)


def emit_binding_group_impl(manifest: Manifest, entry: BindingEntry, embedded: list[str]) -> str:
    group = entry.group
    qualified = f"{manifest.namespace}::{group.name}::group"
    static_names = {s.name for s in group.static_samplers}
    views = [b for b in group.bindings if b.type != "sampler"]
    samplers = [b for b in group.bindings if b.type == "sampler" and b.name not in static_names]

    out = [f"\ncc::span<sg::binding const> {qualified}::declared_bindings()\n{{\n"]
    out.append(f"    return k_bindings_{group.name};\n")
    out.append("}\n")

    out.append(f"\ncc::span<sg::named_sampler const> {qualified}::declared_samplers()\n{{\n")
    if group.static_samplers:
        out.append(f"    return k_static_samplers_{group.name};\n")
    else:
        out.append("    return {};\n")
    out.append("}\n")

    out.append(f"\nsg::binding_group_layout_handle {qualified}::acquire_layout(sg::context& ctx)\n{{\n")
    out.append("    // At the first acquire the table and the shader came from the same build, so a difference\n")
    out.append("    // between them means this generator is wrong rather than that a shader moved on.\n")
    out.append("    CC_ASSERT(self_check().empty(), \"the generated binding table does not describe its own shader\");\n")
    out.append(f"    return ctx.cached.acquire_binding_group_layout(k_bindings_{group.name}, declared_samplers());\n")
    out.append("}\n")

    out.append(f"\nsg::binding_group_layout_handle {qualified}::acquire_layout(\n")
    out.append("    sg::context& ctx, cc::span<sg::named_sampler const> samplers)\n{\n")
    out.append("    CC_ASSERT(self_check().empty(), \"the generated binding table does not describe its own shader\");\n")
    out.append("\n")
    out.append("    cc::vector<sg::named_sampler> merged;\n")
    out.append("    merged.reserve(declared_samplers().size() + samplers.size());\n")
    out.append("    for (auto const& declared : declared_samplers())\n")
    out.append("        merged.push_back(declared);\n")
    out.append("\n")
    out.append("    // Declared first, then only what the shader did not declare, so the shader wins in every build\n")
    out.append("    // and the assertion is what names the mistake in a checked one.\n")
    out.append("    for (auto const& supplied : samplers)\n")
    out.append("    {\n")
    out.append("        bool already_declared = false;\n")
    out.append("        for (auto const& declared : declared_samplers())\n")
    out.append("            already_declared = already_declared || declared.name == supplied.name;\n")
    out.append("\n")
    out.append("        CC_ASSERT(!already_declared, \"the shader already declared this sampler static\");\n")
    out.append("        if (!already_declared)\n")
    out.append("            merged.push_back(supplied);\n")
    out.append("    }\n")
    out.append("\n")
    out.append(f"    return ctx.cached.acquire_binding_group_layout(k_bindings_{group.name}, merged);\n")
    out.append("}\n")

    out.append(f"\ncc::result<sg::binding_group_handle> {qualified}::create(sg::context& ctx) const\n{{\n")
    out.append("    auto const layout = acquire_layout(ctx);\n")
    out.append("\n")
    out.append("    // A slot is a position in THIS layout's bindings(), and one from another layout would be\n")
    out.append("    // in range, wrong and silent — where a wrong name is an error message.\n")
    out.append("    // So the layout is checked to be the one these slots were generated against.\n")
    out.append(f"    CC_ASSERT(layout->structural_hash()\n")
    out.append(f"                  == ctx.cached.acquire_binding_group_layout(k_bindings_{group.name},\n")
    out.append("                                                             declared_samplers())\n")
    out.append("                         ->structural_hash(),\n")
    out.append("              \"the layout is not the one this group was generated against\");\n")
    out.append("\n")
    if views:
        out.append("    cc::vector<sg::slotted_view> views;\n")
        out.append(f"    views.reserve({len(views)});\n")
        for binding in views:
            slot = group.bindings.index(binding)
            out.append(f"    views.push_back({{.slot = sg::binding_slot({slot}), .view = {binding.name}}});\n")
    else:
        out.append("    cc::vector<sg::slotted_view> const views;\n")
    out.append("\n")
    if samplers:
        out.append("    // A sampler the shader did not mark `static` is per-group rather than baked into the layout.\n")
        out.append(f"    cc::vector<sg::named_sampler> samplers;\n")
        out.append(f"    samplers.reserve({len(samplers)});\n")
        for binding in samplers:
            out.append(f'    samplers.push_back({{.name = "{binding.name}", .sampler = {binding.name}}});\n')
    else:
        out.append("    cc::vector<sg::named_sampler> const samplers;\n")
    out.append("\n")
    out.append("    return ctx.persistent.try_create_binding_group(layout, views, samplers);\n")
    out.append("}\n")

    out.append(emit_self_check(manifest, entry, embedded))
    return "".join(out)


def write_if_different(path: Path, content: str) -> None:
    """Copy-if-different: an unchanged package must not retrigger the compile that consumes these."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_text(encoding="utf-8", newline="") == content:
        return
    path.write_text(content, encoding="utf-8", newline="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()

    manifest = read_manifest(args.manifest)
    if manifest.language not in VALID_LANGUAGES:
        print(f"shader package '{manifest.name}': unknown LANGUAGE '{manifest.language}'. "
              f"One of: {' '.join(VALID_LANGUAGES)}", file=sys.stderr)
        return 1

    try:
        entries = parse_entries(manifest)
        embedded = include_closure(manifest, entries.paths)
    except GeneratorError as e:
        print(str(e), file=sys.stderr)
        return 1

    write_if_different(args.out_dir / f"{manifest.name}.hh", emit_header(manifest, entries))
    write_if_different(args.out_dir / f"{manifest.name}.cc",
                       emit_source(manifest, entries.files, entries.bindings, embedded))

    # Depfile: every file we read.
    # This is what makes editing an .hlsli regenerate the package -- DEPENDS alone only covers the entry
    # points named in the manifest, and the include closure is discovered here.
    depfile = f"{(args.out_dir / (manifest.name + '.hh')).as_posix()}:"
    for path in embedded:
        depfile += f" \\\n  {(manifest.source_dir / path).as_posix()}"
    (args.out_dir / f"{manifest.name}.d").write_text(depfile + "\n", encoding="utf-8", newline="")

    return 0


if __name__ == "__main__":
    sys.exit(main())
