"""The binding pass's grammar, in Python.

The same pass exists twice: this one, for the build-time generator, and the C++ one under
src/shaped-shader-library/binding/, for the runtime rewriter.
They are two readings of one grammar, and tests/data/binding-corpus.txt is what keeps them agreeing.
Both sides run it, so a case is added once.

Neither the corpus nor the per-package self-check proves the two are the same function.
They prove the two agree on what we thought of and on what we ship.
The reflection cross-check is the only leg that asks DXC.

The design is libs/graphics/shaped-shader-library/docs/binding-preprocessor.md.
Keep this file and hlsl_tokens.cc / binding_groups.cc in step, error messages included.
"""

from __future__ import annotations

from dataclasses import dataclass, field

# The attribute names the grammar knows.
# A name outside this set is an error rather than a directive nobody reads -- which is exactly what DXC makes
# of it, since it ignores a pragma it does not know.
ATTRIBUTE_NAMES = ("group", "static", "push_constants", "payload", "vertex_input", "attribute")

# HLSL constructs the pass cannot number, so they may not appear inside a group.
REJECTED_KEYWORDS = ("namespace", "struct", "cbuffer", "tbuffer", "class", "typedef", "interface")

# What a source must carry before any of this can apply.
# Only `#pragma`, since the flatten reprints a directive's tokens and promises nothing about the spacing.
PRAGMA_MARKER = "#pragma"


class BindingError(Exception):
    """A source the pass will not accept.

    The message is the one the C++ side reports, verbatim.
    """


@dataclass(frozen=True)
class Location:
    """Where in the shader something is, as the author would recognise it -- see hlsl_location."""

    file: str = ""
    line: int = 1

    def __str__(self) -> str:
        return f"line {self.line}" if not self.file else f"{self.file}:{self.line}"


@dataclass
class Token:
    kind: str  # identifier | number | punctuation | annotation
    text: str
    offset: int
    length: int
    location: Location


@dataclass
class Annotation:
    name: str
    arguments: list[tuple[str, list[str]]]  # (key, values); key is "" for a positional argument
    location: Location


@dataclass
class Binding:
    """One binding, plus where in the source its address has to be written."""

    name: str
    index: int
    count: int
    type: str  # an sg::binding_type enumerator
    dimension: str | None  # an sg::texture_view_dimension enumerator, for textures only
    register_class: str
    type_offset: int
    semicolon_offset: int
    template_argument: str = ""


@dataclass
class DeclaredSampler:
    """A sampler the shader marked `static`, and the state it declared.

    The keys are sg::sampler's own field names and the values its own enumerator names, spelled exactly.
    Everything omitted takes sg::sampler's default, which is a trilinear repeating sampler.
    Kept as C++ spellings rather than as values, because emitting them is all the generator does with them.
    """

    name: str
    fields: dict[str, str] = field(default_factory=dict)


@dataclass
class Group:
    name: str
    group: int
    bindings: list[Binding] = field(default_factory=list)
    static_samplers: list[DeclaredSampler] = field(default_factory=list)


@dataclass
class InlineConstants:
    """The inline-constants block a shader declares.

    The register is always b0, since a pipeline layout carries at most one such binding, so the only number to
    state is the space.
    The block's layout is here too, because the generator emits a C++ mirror of it -- see the spike's Q14 for
    where each rule came from.
    """

    name: str
    space: int
    type_offset: int
    semicolon_offset: int
    type: str = ""
    members: list[StructMember] = field(default_factory=list)
    size: int = 0


@dataclass
class StructMember:
    """One member of an annotated struct, as the shader declares it."""

    name: str
    type: str
    semantic: str
    semantic_index: int
    type_offset: int
    offset: int = 0  # the byte offset the layout puts it at; a constant block's is DXC's, not C++'s

    # The sg::vertex_attribute_format enumerator a `#pragma sc attribute format=<name>` stated, or empty when the
    # format follows from the member's type.
    #
    # It exists because two formats cannot be reached any other way: HLSL has no spelling that tells a `float4`
    # fed by four floats from one fed by four normalized bytes, so `rgba8_unorm` and `rgba8_uint` have to be
    # stated rather than derived.
    format_override: str = ""


@dataclass
class VertexInput:
    """A vertex input struct: what feeds one bound vertex buffer.

    Members are numbered by declaration order, and that number is what the SPIR-V arm writes as a location.
    The buffer's byte layout is the generated C++ mirror's rather than this struct's.
    """

    name: str
    slot: int = 0
    per_instance: bool = False
    members: list[StructMember] = field(default_factory=list)


@dataclass
class Payload:
    """A ray payload: the struct that travels through TraceRay to the hit and miss shaders and back.

    A payload is registers rather than a buffer, so its members pack at natural alignment and its size is
    their plain sum -- see the spike's Q13, which measured that rather than assuming it.
    """

    name: str
    members: list[StructMember] = field(default_factory=list)
    size: int = 0


@dataclass
class Bindings:
    """Everything the pass reads out of one translation unit."""

    groups: list[Group] = field(default_factory=list)
    inline_constants: InlineConstants | None = None
    vertex_inputs: list[VertexInput] = field(default_factory=list)
    payloads: list[Payload] = field(default_factory=list)


# ---------------------------------------------------------------------------------------------------
# the type table
# ---------------------------------------------------------------------------------------------------

# HLSL type -> (register class, sg::binding_type, sg::texture_view_dimension or None).
# The single most important piece of shared state in the design: the rewriter and the generator must agree
# on it exactly, because a divergence binds a resource to the wrong descriptor with nothing to catch it.
# Keep in step with impl/hlsl_binding_types.cc.
BINDING_TYPES: dict[str, tuple[str, str, str | None]] = {
    "Texture1D": ("t", "readonly_texture", "tex_1d"),
    "Texture1DArray": ("t", "readonly_texture", "tex_1d_array"),
    "Texture2D": ("t", "readonly_texture", "tex_2d"),
    "Texture2DArray": ("t", "readonly_texture", "tex_2d_array"),
    "Texture2DMS": ("t", "readonly_texture", "tex_2d_ms"),
    "Texture2DMSArray": ("t", "readonly_texture", "tex_2d_ms_array"),
    "Texture3D": ("t", "readonly_texture", "tex_3d"),
    "TextureCube": ("t", "readonly_texture", "cube"),
    "TextureCubeArray": ("t", "readonly_texture", "cube_array"),
    # A storage view has no cube and no multisampling, which is why this half of the table is shorter.
    "RWTexture1D": ("u", "readwrite_texture", "tex_1d"),
    "RWTexture1DArray": ("u", "readwrite_texture", "tex_1d_array"),
    "RWTexture2D": ("u", "readwrite_texture", "tex_2d"),
    "RWTexture2DArray": ("u", "readwrite_texture", "tex_2d_array"),
    "RWTexture3D": ("u", "readwrite_texture", "tex_3d"),
    "Buffer": ("t", "readonly_structured_buffer", None),
    "RWBuffer": ("u", "readwrite_structured_buffer", None),
    "StructuredBuffer": ("t", "readonly_structured_buffer", None),
    "RWStructuredBuffer": ("u", "readwrite_structured_buffer", None),
    "ByteAddressBuffer": ("t", "readonly_raw_buffer", None),
    "RWByteAddressBuffer": ("u", "readwrite_raw_buffer", None),
    "ConstantBuffer": ("b", "uniform_buffer", None),
    "SamplerState": ("s", "sampler", None),
    "SamplerComparisonState": ("s", "sampler", None),
    "RaytracingAccelerationStructure": ("t", "acceleration_structure", None),
}


# ---------------------------------------------------------------------------------------------------
# sampler state
# ---------------------------------------------------------------------------------------------------

SAMPLER_FILTERS = ("nearest", "linear")
SAMPLER_ADDRESS_MODES = ("repeat", "mirror_repeat", "clamp_edge", "clamp_border", "mirror_clamp_edge")
SAMPLER_BORDER_COLORS = ("transparent_black", "opaque_black", "opaque_white")
COMPARE_OPS = ("never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always")

# key -> (the sg::sampler fields it sets, the enumerator set its values come from).
# A shorthand sets three fields at once, and its tuple form addresses them in the order sg::sampler declares.
SAMPLER_ENUM_KEYS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    "filter": (("min_filter", "mag_filter", "mip_filter"), SAMPLER_FILTERS),
    "address": (("address_u", "address_v", "address_w"), SAMPLER_ADDRESS_MODES),
    "min_filter": (("min_filter",), SAMPLER_FILTERS),
    "mag_filter": (("mag_filter",), SAMPLER_FILTERS),
    "mip_filter": (("mip_filter",), SAMPLER_FILTERS),
    "address_u": (("address_u",), SAMPLER_ADDRESS_MODES),
    "address_v": (("address_v",), SAMPLER_ADDRESS_MODES),
    "address_w": (("address_w",), SAMPLER_ADDRESS_MODES),
    "border_color": (("border_color",), SAMPLER_BORDER_COLORS),
    "compare": (("compare",), COMPARE_OPS),
}

SAMPLER_FLOAT_KEYS = ("mip_lod_bias", "min_lod", "max_lod")

# The register space an inline-constants block occupies, reserved for it across every package.
#
# A pipeline layout carries at most one such block and its register is always `b0`, so the space was the only
# number left to choose -- and every block in the tree chose 9, by hand and by convention.
# Reserving it deletes the argument, the collision it could name, and the class of mistake at once.
# Keep in step with slib::inline_constants_space in binding/binding_groups.hh.
INLINE_CONSTANTS_SPACE = 9

# HLSL value type -> (what the mirror declares, size in bytes, constant-block alignment, C++ alignment,
# sg::vertex_attribute_format or None).
#
# The mirror spells its members as plain `float` / `int` / `unsigned` and friends, because generated package code
# sits below anything that could define a vector type.
#
# The two alignments are different numbers for a reason: a constant block starts a `double[2]` on a whole 16-byte
# row where C++ aligns it to 8, so a payload (natural packing) and a constant block read different columns.
# Keep in step with impl/hlsl_value_types.cc, the rejection reasons below included.
VALUE_TYPES: dict[str, tuple[str, int, int, int, str | None]] = {
    "float": ("float", 4, 4, 4, "f32"),
    "float2": ("float[2]", 8, 4, 4, "vec2f"),
    "float3": ("float[3]", 12, 4, 4, "vec3f"),
    "float4": ("float[4]", 16, 4, 4, "vec4f"),
    "int": ("int", 4, 4, 4, "i32"),
    "int2": ("int[2]", 8, 4, 4, "vec2i"),
    "int3": ("int[3]", 12, 4, 4, "vec3i"),
    "int4": ("int[4]", 16, 4, 4, "vec4i"),
    "uint": ("unsigned", 4, 4, 4, "u32"),
    "uint2": ("unsigned[2]", 8, 4, 4, "vec2u"),
    "uint3": ("unsigned[3]", 12, 4, 4, "vec3u"),
    "uint4": ("unsigned[4]", 16, 4, 4, "vec4u"),
    # Four bytes in a constant block, and no vertex attribute format at all -- the reason sr::gpu_boolean exists.
    "bool": ("unsigned", 4, 4, 4, None),
    # The one matrix, and Q14g is why it is the only one: a float4x4 is four vectors of four whichever
    # orientation is in force, where a float3x4 has different member offsets row-major and column-major at the
    # same 64-byte total.
    # The pass cannot see the orientation, so it admits only the matrix that does not have one.
    "float4x4": ("float[16]", 64, 16, 4, None),
    # `half` and `min16float` are the same 32 bits as `float` unless `-enable-16bit-types` is passed, and nothing
    # in ssc passes it.
    # Q14h pins that, so adding the flag is a failing test rather than a wrong number.
    "half": ("float", 4, 4, 4, None),
    "half2": ("float[2]", 8, 4, 4, None),
    "half3": ("float[3]", 12, 4, 4, None),
    "half4": ("float[4]", 16, 4, 4, None),
    "min16float": ("float", 4, 4, 4, None),
    "min16float2": ("float[2]", 8, 4, 4, None),
    "min16float3": ("float[3]", 12, 4, 4, None),
    "min16float4": ("float[4]", 16, 4, 4, None),
    # The 64-bit types obey rules of their own, which Q14i measured: a scalar aligns to 8, a vector starts a whole
    # row, and neither is kept off a row boundary -- a double3 is 24 bytes and crosses one outright.
    "double": ("double", 8, 8, 8, None),
    "double2": ("double[2]", 16, 16, 8, None),
    "double3": ("double[3]", 24, 16, 8, None),
    "double4": ("double[4]", 32, 16, 8, None),
    "int64_t": ("long long", 8, 8, 8, None),
    "int64_t2": ("long long[2]", 16, 16, 8, None),
    "int64_t3": ("long long[3]", 24, 16, 8, None),
    "int64_t4": ("long long[4]", 32, 16, 8, None),
    "uint64_t": ("unsigned long long", 8, 8, 8, None),
    "uint64_t2": ("unsigned long long[2]", 16, 16, 8, None),
    "uint64_t3": ("unsigned long long[3]", 24, 16, 8, None),
    "uint64_t4": ("unsigned long long[4]", 32, 16, 8, None),
}

# Why a type outside the table is outside it, appended to the refusal.
# A reader wants opposite reactions to a table gap and to a portability rule, and the message is the only thing
# that tells them which they have.
REJECTION_REASONS: tuple[tuple[str, str], ...] = (
    ("float1x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"),
    ("float2x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"),
    ("float3x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"),
    ("float4x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"),
    ("matrix", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"),
)


# Every sg::vertex_attribute_format enumerator, so a `format=` override can be checked against the real set.
# The two a member's type can never reach are the last: `rgba8_unorm` and `rgba8_uint` are what the override
# exists for.
# Keep in step with k_formats in impl/hlsl_value_types.cc.
VERTEX_ATTRIBUTE_FORMATS = (
    "f32", "vec2f", "vec3f", "vec4f",
    "i32", "vec2i", "vec3i", "vec4i",
    "u32", "vec2u", "vec3u", "vec4u",
    "rgba8_unorm", "rgba8_uint",
)


def rejection_reason_for(hlsl_type: str) -> str:
    """The sentence to append to a refusal, or empty when the pass has nothing more specific to say."""
    for prefix, reason in REJECTION_REASONS:
        if hlsl_type.startswith(prefix):
            return reason
    return ""


def parse_sampler_state(attribute: Annotation) -> dict[str, str]:
    """The sg::sampler fields a `static` attribute sets, as C++ spellings.

    Keep in step with impl/hlsl_sampler_state.cc, error messages included.
    """
    fields: dict[str, str] = {}

    for key, values in attribute.arguments:
        if not key:
            raise BindingError(
                f"{attribute.location}: 'static' takes key=value arguments, not '{values[0]}'")

        if key in SAMPLER_ENUM_KEYS:
            targets, allowed = SAMPLER_ENUM_KEYS[key]
            if len(targets) == 3:
                if len(values) not in (1, 3):
                    raise BindingError(f"{attribute.location}: '{key}' takes one value or a tuple of three")
            elif len(values) != 1:
                raise BindingError(f"{attribute.location}: '{key}' takes exactly one value")

            for value in values:
                if value not in allowed:
                    raise BindingError(f"{attribute.location}: '{value}' is not a value of '{key}'")

            spread = values * 3 if len(targets) == 3 and len(values) == 1 else values
            for target, value in zip(targets, spread):
                fields[target] = value
            continue

        # The key first, then its arity -- the order hlsl_sampler_state.cc dispatches in.
        # Checking arity first reported "'bogus' takes exactly one value" for a key that is not a field at all,
        # which is a different sentence from the C++ half's for the same input.
        if key not in SAMPLER_FLOAT_KEYS and key != "max_anisotropy":
            raise BindingError(f"{attribute.location}: '{key}' is not a field of sg::sampler")

        if len(values) != 1:
            raise BindingError(f"{attribute.location}: '{key}' takes exactly one value")

        if key in SAMPLER_FLOAT_KEYS:
            try:
                float(values[0])
            except ValueError:
                raise BindingError(f"{attribute.location}: '{values[0]}' is not a number") from None
            fields[key] = values[0]
            continue

        if not values[0].isdigit() or int(values[0]) == 0:
            raise BindingError(f"{attribute.location}: '{values[0]}' is not an anisotropy")
        fields[key] = values[0]

    return fields


# ---------------------------------------------------------------------------------------------------
# the tokenizer
# ---------------------------------------------------------------------------------------------------


def _is_identifier_start(c: str) -> bool:
    return c.isascii() and (c.isalpha() or c == "_")


def _is_identifier_char(c: str) -> bool:
    return c.isascii() and (c.isalnum() or c == "_")


def _is_inline_space(c: str) -> bool:
    """Whitespace WITHIN a line -- the newline is what ends a directive here, so it is not one."""
    return c in " \t\r"


def _trim(text: str) -> str:
    return text.strip(" \t\r")


def _eat_word(text: str, word: str) -> str | None:
    """`text` past `word` and the whitespace after it, or None when it does not start with it.

    The word must end at a boundary, so `#pragma scope` is not a `#pragma sc` line.
    """
    if not text.startswith(word):
        return None
    if len(text) > len(word) and _is_identifier_char(text[len(word)]):
        return None
    return text[len(word):].lstrip(" \t\r")


def lex(hlsl: str) -> list[Token]:
    """HLSL as tokens, dropping whitespace, comments and the insides of literals.

    A `#pragma sc` line becomes an `annotation`, and a `#line` directive moves the location the tokens after
    it report without becoming a token itself.
    """
    tokens: list[Token] = []
    size = len(hlsl)
    i = 0
    line = 1
    file = ""
    line_has_token = False

    def rest_of_line(start: int) -> int:
        end = hlsl.find("\n", start)
        return size if end < 0 else end

    def emit(kind: str, text: str, offset: int, length: int) -> None:
        nonlocal line_has_token
        tokens.append(Token(kind, text, offset, length, Location(file, line)))
        line_has_token = True

    while i < size:
        c = hlsl[i]

        if c == "\n":
            line += 1
            line_has_token = False
            i += 1
            continue

        if _is_inline_space(c):
            i += 1
            continue

        # A directive only counts as one when it opens its line, which is what the language requires of it too.
        if c == "#" and not line_has_token:
            end = rest_of_line(i)
            tail = _trim(hlsl[i + 1:end])

            after_pragma = _eat_word(tail, "pragma")
            if after_pragma is not None:
                after_sc = _eat_word(after_pragma, "sc")
                if after_sc is not None:
                    emit("annotation", _trim(after_sc), i, end - i)
                    i = end
                    continue

            # `#line <n> ["file"]` is how the flattened source says where its text came from.
            after_line = _eat_word(tail, "line")
            if after_line is not None:
                digits = 0
                while digits < len(after_line) and after_line[digits].isdigit():
                    digits += 1
                if digits > 0:
                    # The directive names the line AFTER it, and the newline below is what arrives there.
                    line = int(after_line[:digits]) - 1
                    quoted = _trim(after_line[digits:])
                    if quoted.startswith('"'):
                        closing = quoted.find('"', 1)
                        if closing > 0:
                            file = quoted[1:closing]
                    i = end
                    continue

        if c == "/" and i + 1 < size and hlsl[i + 1] == "/":
            i = rest_of_line(i)
            continue

        if c == "/" and i + 1 < size and hlsl[i + 1] == "*":
            opened_at = Location(file, line)
            end = i + 2
            while end + 1 < size and not (hlsl[end] == "*" and hlsl[end + 1] == "/"):
                if hlsl[end] == "\n":
                    line += 1
                end += 1
            if end + 1 >= size:
                raise BindingError(f"{opened_at}: unterminated block comment")
            i = end + 2
            continue

        # A literal is skipped whole, so a `//` or a brace inside one never reaches the parser.
        if c in "\"'":
            opened_at = Location(file, line)
            end = i + 1
            while end < size and hlsl[end] != c:
                if hlsl[end] == "\n":
                    line += 1
                end += 2 if hlsl[end] == "\\" else 1
            if end >= size:
                raise BindingError(f"{opened_at}: unterminated literal")
            i = end + 1
            continue

        if _is_identifier_start(c):
            end = i
            while end < size and _is_identifier_char(hlsl[end]):
                end += 1
            emit("identifier", hlsl[i:end], i, end - i)
            i = end
            continue

        if c.isdigit() or (c == "." and i + 1 < size and hlsl[i + 1].isdigit()):
            end = i
            while end < size and (_is_identifier_char(hlsl[end]) or hlsl[end] == "."):
                prev = hlsl[end]
                end += 1
                if prev in "eE" and end < size and hlsl[end] in "+-":
                    end += 1
            emit("number", hlsl[i:end], i, end - i)
            i = end
            continue

        emit("punctuation", hlsl[i], i, 1)
        i += 1

    return tokens


def parse_annotation(text: str, location: Location) -> Annotation:
    """One annotation token's text as `<name> [key=value]...`."""
    size = len(text)
    i = 0

    def skip_spaces() -> None:
        nonlocal i
        while i < size and _is_inline_space(text[i]):
            i += 1

    def read_word() -> str:
        nonlocal i
        start = i
        while i < size and not _is_inline_space(text[i]) and text[i] not in "=(),":
            i += 1
        return text[start:i]

    skip_spaces()
    name = read_word()
    if not name:
        raise BindingError(f"{location}: an attribute must name what it is")

    arguments: list[tuple[str, list[str]]] = []
    while True:
        skip_spaces()
        if i >= size:
            break

        word = read_word()
        if not word:
            raise BindingError(f"{location}: unexpected '{text[i]}' in attribute '{name}'")

        # Whitespace around `=` is tolerated: the flatten reproduces a pragma's tokens, and nothing guarantees
        # it reproduces the spacing between them.
        skip_spaces()

        if i < size and text[i] == "=":
            key = word
            i += 1
            skip_spaces()

            values: list[str] = []
            if i < size and text[i] == "(":
                i += 1
                while True:
                    skip_spaces()
                    value = read_word()
                    if not value:
                        raise BindingError(f"{location}: '{key}' has an empty value in its tuple")
                    values.append(value)

                    skip_spaces()
                    if i >= size:
                        raise BindingError(f"{location}: '{key}' opens a tuple it never closes")
                    if text[i] == ")":
                        i += 1
                        break
                    if text[i] != ",":
                        raise BindingError(f"{location}: expected ',' or ')' in '{key}'")
                    i += 1
            else:
                value = read_word()
                if not value:
                    raise BindingError(f"{location}: '{key}' is missing its value")
                values.append(value)

            arguments.append((key, values))
        else:
            arguments.append(("", [word]))

    return Annotation(name, arguments, location)


# ---------------------------------------------------------------------------------------------------
# the parser
# ---------------------------------------------------------------------------------------------------


class _Parser:
    """Cursor state, because an attribute stands on the line before the declaration it applies to."""

    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.at = 0
        self.annotations: list[tuple[int, int]] = []  # (offset, length) of every directive consumed
        self.inline_constants: InlineConstants | None = None
        self.vertex_inputs: list[VertexInput] = []
        self.payloads: list[Payload] = []
        # Where one file-scope `struct <name> { ... }` body sits, as token indices -- recorded rather than
        # parsed, because reading one is only necessary when a `push_constants` block names it.
        self.struct_bodies: list[tuple[str, int, int]] = []
        self.group_names: set[str] = set()
        self.group_numbers: set[int] = set()
        self.binding_names: set[str] = set()

    def at_end(self) -> bool:
        return self.at >= len(self.tokens)

    def current(self) -> Token:
        return self.tokens[self.at]

    def is_punctuation(self, c: str) -> bool:
        return not self.at_end() and self.current().kind == "punctuation" and self.current().text == c

    def is_identifier(self, text: str) -> bool:
        return not self.at_end() and self.current().kind == "identifier" and self.current().text == text

    def location_here(self) -> Location:
        if not self.at_end():
            return self.current().location
        return self.tokens[-1].location if self.tokens else Location()

    def read_annotation(self) -> Annotation:
        token = self.current()
        parsed = parse_annotation(token.text, token.location)
        self.annotations.append((token.offset, token.length))
        self.at += 1

        if parsed.name not in ATTRIBUTE_NAMES:
            raise BindingError(f"{token.location}: '{parsed.name}' is not an attribute this pass knows")
        return parsed

    def run(self) -> list[Group]:
        groups: list[Group] = []
        pending: Annotation | None = None

        while not self.at_end():
            if self.current().kind == "annotation":
                location = self.current().location
                parsed = self.read_annotation()
                if pending is not None:
                    raise BindingError(f"{location}: two attributes stand before one declaration")
                pending = parsed
                continue

            if self.is_identifier("namespace"):
                group = self.parse_namespace(pending)
                pending = None
                if group is not None:
                    groups.append(group)
                continue

            # A `push_constants` attribute attaches to an ordinary file-scope declaration rather than to a
            # namespace, so it is the one attribute that reads a declaration out here.
            if pending is not None and pending.name == "push_constants" and self.current().kind == "identifier":
                self.parse_inline_constants(pending)
                pending = None
                continue

            if pending is not None and pending.name == "vertex_input" and self.is_identifier("struct"):
                self.parse_vertex_input(pending)
                pending = None
                continue

            if pending is not None and pending.name == "payload" and self.is_identifier("struct"):
                self.parse_payload(pending)
                pending = None
                continue

            self.reject_unclaimed(pending)

            if self.is_identifier("struct") and self.record_struct_body():
                continue

            self.at += 1

        self.reject_unclaimed(pending)
        return groups

    def record_struct_body(self) -> bool:
        """Notes where an unannotated `struct <name> { ... }` body is and skips it.

        Reports False and leaves the cursor alone when this was not one.
        """
        start = self.at
        self.at += 1  # `struct`

        if self.at_end() or self.current().kind != "identifier":
            self.at = start
            return False

        name = self.current().text
        self.at += 1

        if not self.is_punctuation("{"):
            self.at = start
            return False
        self.at += 1

        first = self.at
        depth = 1
        while not self.at_end() and depth > 0:
            if self.is_punctuation("{"):
                depth += 1
            elif self.is_punctuation("}"):
                depth -= 1
            self.at += 1

        if depth != 0:
            self.at = start
            return False

        self.struct_bodies.append((name, first, self.at - 1))
        return True

    def layout_inline_constants(self) -> None:
        """Reads the struct an inline-constants block names, and lays it out the way a constant buffer does.

        Run after the whole file is walked, so the struct may be declared on either side of the block.
        The rules are the spike's Q14, measured against DXC on both targets rather than restated.
        """
        if self.inline_constants is None:
            return

        constants = self.inline_constants
        body = next((b for b in self.struct_bodies if b[0] == constants.type), None)
        if body is None:
            raise BindingError(
                f"the inline-constants block '{constants.name}' names a struct '{constants.type}' "
                f"this file does not declare")

        saved = self.at
        self.at = body[1]

        offset = 0
        while self.at < body[2]:
            member = self.parse_constant_member()
            _, size, cb_align, _, _ = VALUE_TYPES[member.type]

            # Where the type may start at all, which for a 64-bit scalar is 8 and for a 64-bit vector or a matrix
            # is a whole row -- Q14i and Q14g measured both, and neither follows from the 32-bit rules.
            offset += (cb_align - offset % cb_align) % cb_align

            # And then the 32-bit rule: a value of 16 bytes or less may not straddle a row, and a row is filled
            # before it is left.
            # A wider one straddles freely, which is what a `double3` crossing a row boundary showed.
            if size <= 16 and offset % 16 + size > 16:
                offset += 16 - offset % 16

            member.offset = offset
            offset += size
            constants.members.append(member)

        self.at = saved

        if not constants.members:
            raise BindingError(f"the inline-constants block '{constants.name}' declares no members")

        # The block's own total rounds up to a whole row.
        constants.size = offset if offset % 16 == 0 else offset + (16 - offset % 16)

    def parse_constant_member(self) -> StructMember:
        """One `<type> <name>;` of a constant block.

        The subset is scalars, vectors and `bool`. An array or a matrix is refused rather than mirrored, and
        Q14 is why: the member after one packs into its last row's tail, which C++ cannot express.
        """
        token = self.current()
        if token.kind != "identifier":
            raise BindingError(f"{token.location}: expected a member declaration, found '{token.text}'")

        type_name = token.text
        location = token.location
        self.at += 1

        if self.at_end() or self.current().kind != "identifier":
            raise BindingError(f"{location}: expected a name after '{type_name}'")

        name = self.current().text
        self.at += 1

        if self.is_punctuation("["):
            raise BindingError(f"{location}: an array is not supported in a constant block, because the member "
                               f"after it packs into its last row")

        if not self.is_punctuation(";"):
            raise BindingError(f"{location}: expected ';' after '{name}'")
        self.at += 1

        if type_name not in VALUE_TYPES:
            raise BindingError(f"{location}: '{type_name}' is not a constant block type this pass knows"
                               f"{rejection_reason_for(type_name)}")

        return StructMember(name, type_name, "", 0, token.offset)

    @staticmethod
    def reject_unclaimed_static(pending: Annotation | None) -> None:
        if pending is not None:
            raise BindingError(f"{pending.location}: a 'static' attribute must stand before a sampler declaration")

    @staticmethod
    def reject_unclaimed(pending: Annotation | None) -> None:
        if pending is None:
            return
        if pending.name == "push_constants":
            raise BindingError(
                f"{pending.location}: a 'push_constants' attribute must stand before a ConstantBuffer declaration")
        if pending.name in ("vertex_input", "payload"):
            raise BindingError(
                f"{pending.location}: a '{pending.name}' attribute must stand before a struct declaration")

        # `static` and `attribute` each stand before a declaration rather than before a namespace, so the
        # sentence below is not true of either -- and neither one points at the fix.
        # reject_unclaimed_static already says the right thing; it was reachable only from a namespace's closing
        # brace, so a `static` at file scope fell through to the wrong message.
        if pending.name == "static":
            Parser.reject_unclaimed_static(pending)

        if pending.name == "attribute":
            raise BindingError(
                f"{pending.location}: an 'attribute' attribute must stand before a struct member")
        raise BindingError(
            f"{pending.location}: a '{pending.name}' attribute must stand before a namespace declaration")

    def parse_inline_constants(self, attribute: Annotation) -> None:
        """The one `ConstantBuffer<T> name;` a `push_constants` attribute stands before.

        The register is always `b0` and the space is reserved, so the attribute carries no number at all.
        """
        if attribute.arguments:
            raise BindingError(
                f"{attribute.location}: 'push_constants' takes no arguments, and its space is reserved")

        if self.inline_constants is not None:
            raise BindingError(
                f"{attribute.location}: a second 'push_constants' block, and a pipeline layout carries at most one")

        location = self.current().location
        binding = self.parse_binding(0)

        if binding.type != "uniform_buffer":
            raise BindingError(
                f"{location}: 'push_constants' describes a ConstantBuffer, and '{binding.name}' is not one")
        if binding.count != 1:
            raise BindingError(f"{location}: an inline-constants block is one buffer, not an array")
        if not binding.template_argument:
            raise BindingError(f"{location}: an inline-constants block must name the struct it holds")

        self.inline_constants = InlineConstants(binding.name, INLINE_CONSTANTS_SPACE, binding.type_offset,
                                                binding.semicolon_offset, binding.template_argument)

    def parse_vertex_input(self, attribute: Annotation) -> None:
        """The `struct <name> { <type> <member> : <SEMANTIC>; ... };` a `vertex_input` attribute stands before."""
        vertex_input = VertexInput("")

        # Declaration order, the same order the members are already numbered by.
        # The pass trusted it for the members and asked the author for the struct's own number, which was one
        # rule too many -- `slot=` stays for the case that needs it, and that case is two shaders sharing a
        # vertex-input header while declaring their structs in a different order.
        vertex_input.slot = len(self.vertex_inputs)

        stated = self.read_vertex_input_arguments(attribute, vertex_input)
        if stated is not None:
            vertex_input.slot = stated

        keyword_location = self.current().location
        self.at += 1  # `struct`

        if self.at_end() or self.current().kind != "identifier":
            raise BindingError(f"{keyword_location}: a 'vertex_input' struct must be named")

        vertex_input.name = self.current().text
        self.at += 1

        if any(other.name == vertex_input.name for other in self.vertex_inputs):
            raise BindingError(f"{keyword_location}: struct '{vertex_input.name}' is declared twice")

        # The same collision a group already refuses by number, and it can only come from an explicit `slot=`
        # now that the default is the declaration index.
        for other in self.vertex_inputs:
            if other.slot == vertex_input.slot:
                raise BindingError(f"{keyword_location}: slot {vertex_input.slot} is claimed twice, by struct "
                                   f"'{other.name}' and struct '{vertex_input.name}'")

        if not self.is_punctuation("{"):
            raise BindingError(f"{keyword_location}: struct '{vertex_input.name}' must open its block right away")
        self.at += 1

        # A vertex buffer is a byte stream the input assembler decodes per attribute offset, so the mirror
        # *defines* the layout: every member is naturally packed, which for the 32-bit attribute types is a
        # plain sum.
        offset = 0

        # A member may carry its own attribute, which today means one thing: the format it is fed in.
        pending_member_attribute: Annotation | None = None
        while not self.is_punctuation("}"):
            if self.at_end():
                raise BindingError(f"{self.location_here()}: struct '{vertex_input.name}' is never closed")

            if self.current().kind == "annotation":
                location = self.current().location
                parsed = self.read_annotation()
                if parsed.name != "attribute":
                    raise BindingError(f"{location}: '{parsed.name}' does not stand before a struct member")
                if pending_member_attribute is not None:
                    raise BindingError(f"{location}: two attributes stand before one member")
                pending_member_attribute = parsed
                continue

            member = self.parse_struct_member()
            if pending_member_attribute is not None:
                member.format_override = self.format_override_of(pending_member_attribute)
                pending_member_attribute = None

            member.offset = offset
            offset += VALUE_TYPES[member.type][1]
            vertex_input.members.append(member)
        self.at += 1  # the '}'

        if pending_member_attribute is not None:
            raise BindingError(
                f"{pending_member_attribute.location}: an 'attribute' attribute must stand before a struct member")

        # The declaration's own `;`, which HLSL requires and the pass does not otherwise care about.
        if self.is_punctuation(";"):
            self.at += 1

        self.vertex_inputs.append(vertex_input)

    def parse_payload(self, attribute: Annotation) -> None:
        """The `struct <name> { <type> <member>; ... };` a `payload` attribute stands before."""
        if attribute.arguments:
            raise BindingError(f"{attribute.location}: 'payload' takes no arguments")

        keyword_location = self.current().location
        self.at += 1  # `struct`

        if self.at_end() or self.current().kind != "identifier":
            raise BindingError(f"{keyword_location}: a 'payload' struct must be named")

        payload = Payload(self.current().text)
        self.at += 1

        if any(other.name == payload.name for other in self.payloads):
            raise BindingError(f"{keyword_location}: struct '{payload.name}' is declared twice")

        if not self.is_punctuation("{"):
            raise BindingError(f"{keyword_location}: struct '{payload.name}' must open its block right away")
        self.at += 1

        while not self.is_punctuation("}"):
            if self.at_end():
                raise BindingError(f"{self.location_here()}: struct '{payload.name}' is never closed")
            member = self.parse_struct_member(requires_semantic=False)

            # Natural alignment is the mirror's own, so a member sits where C++ would put it -- a plain sum while
            # every type in the table was 4-aligned, and an alignment step now that the 64-bit ones are not.
            _, size, _, cpp_align, _ = VALUE_TYPES[member.type]
            payload.size += (cpp_align - payload.size % cpp_align) % cpp_align
            member.offset = payload.size
            payload.size += size

            payload.members.append(member)
        self.at += 1  # the '}'

        if self.is_punctuation(";"):
            self.at += 1

        if not payload.members:
            raise BindingError(f"{keyword_location}: payload '{payload.name}' declares no members")

        # C++ pads a struct out to its own alignment, so the mirror's `sizeof` is the sum rounded up to the
        # widest member's -- and max_payload_size has to be that number rather than the sum, or the generated
        # static_assert compares two different things.
        # A no-op until a payload holds a 64-bit member, since everything else in the table aligns to 4.
        alignment = max(VALUE_TYPES[m.type][3] for m in payload.members)
        payload.size += (alignment - payload.size % alignment) % alignment

        self.payloads.append(payload)

    @staticmethod
    def format_override_of(attribute: Annotation) -> str:
        """The one argument `attribute` takes: `format=<sg::vertex_attribute_format enumerator>`."""
        if (len(attribute.arguments) != 1 or attribute.arguments[0][0] != "format"
                or len(attribute.arguments[0][1]) != 1):
            raise BindingError(f"{attribute.location}: 'attribute' takes exactly one format=<name>")

        name = attribute.arguments[0][1][0]
        if name not in VERTEX_ATTRIBUTE_FORMATS:
            raise BindingError(f"{attribute.location}: '{name}' is not an sg::vertex_attribute_format")
        return name

    @staticmethod
    def read_vertex_input_arguments(attribute: Annotation, vertex_input: VertexInput) -> int | None:
        """`slot=<n>` and the bare `per_instance` flag, both optional.

        A stated slot is returned rather than set, so the caller can tell it from the default.
        """
        stated: int | None = None
        for key, values in attribute.arguments:
            if not key and len(values) == 1 and values[0] == "per_instance":
                vertex_input.per_instance = True
                continue

            if key == "slot" and len(values) == 1:
                if not values[0].isdigit():
                    raise BindingError(f"{attribute.location}: '{values[0]}' is not a slot")
                stated = int(values[0])
                continue

            named = key if key else values[0]
            raise BindingError(
                f"{attribute.location}: 'vertex_input' takes slot=<n> and per_instance, not '{named}'")
        return stated

    def parse_struct_member(self, requires_semantic: bool = True) -> StructMember:
        """One `<type> <name>[ : <SEMANTIC>];`.

        A vertex input requires the semantic, since that is what HLSL matches an input by; a payload has none.
        """
        token = self.current()
        if token.kind != "identifier":
            raise BindingError(f"{token.location}: expected a member declaration, found '{token.text}'")

        type_name = token.text
        location = token.location
        type_offset = token.offset
        self.at += 1

        if type_name not in VALUE_TYPES:
            kind = "vertex attribute" if requires_semantic else "payload"
            raise BindingError(f"{location}: '{type_name}' is not a {kind} type this pass knows"
                               f"{rejection_reason_for(type_name)}")

        # `bool` is the case: four bytes in a constant block, and no vertex attribute format on any API.
        # Refused here rather than generated, since the generator would otherwise emit a format that is not one.
        if requires_semantic and VALUE_TYPES[type_name][4] is None:
            raise BindingError(
                f"{location}: '{type_name}' has no vertex attribute format, so it cannot feed a vertex input")

        if self.at_end() or self.current().kind != "identifier":
            raise BindingError(f"{location}: expected a name after '{type_name}'")

        name = self.current().text
        self.at += 1

        # The semantic, which is what HLSL matches a vertex input by, and what the mirror carries into sg.
        if requires_semantic and not self.is_punctuation(":"):
            raise BindingError(f"{location}: vertex input member '{name}' must carry a semantic")

        semantic = ""
        digits = 0
        semantic_index = 0
        if self.is_punctuation(":"):
            self.at += 1
            if self.at_end() or self.current().kind != "identifier":
                raise BindingError(f"{location}: expected a semantic after '{name}'")

            # HLSL splits a trailing integer off the semantic, so TEXCOORD0 is TEXCOORD index 0.
            semantic = self.current().text
            digits = len(semantic)
            while digits > 0 and semantic[digits - 1].isdigit():
                digits -= 1
            semantic_index = int(semantic[digits:]) if digits < len(semantic) else 0
            self.at += 1

        if not self.is_punctuation(";"):
            raise BindingError(f"{location}: expected ';' after '{name}'")
        self.at += 1

        return StructMember(name, type_name, semantic[:digits], semantic_index, type_offset)

    def parse_namespace(self, pending: Annotation | None) -> Group | None:
        keyword_location = self.current().location
        self.at += 1  # `namespace`

        if self.at_end() or self.current().kind != "identifier":
            self.reject_unclaimed(pending)
            return None

        name = self.current().text
        name_location = self.current().location
        self.at += 1

        if pending is None:
            return None

        if pending.name != "group":
            raise BindingError(f"{pending.location}: '{pending.name}' is not an attribute of a namespace")

        number = self.group_number_of(pending)

        # One annotated namespace is declared exactly once, in one block, and owns its number alone.
        if name in self.group_names:
            raise BindingError(f"{name_location}: namespace '{name}' is declared twice")
        self.group_names.add(name)
        if number in self.group_numbers:
            raise BindingError(f"{name_location}: group {number} is declared twice, by namespace '{name}'")
        self.group_numbers.add(number)

        # Group n occupies space n, so this is the one way a group and an inline-constants block could still land
        # in the same space now that the block's own space is reserved rather than stated.
        if number == INLINE_CONSTANTS_SPACE:
            raise BindingError(f"{name_location}: group {number} would share its space with the inline constants, "
                               f"which reserve it")

        if not self.is_punctuation("{"):
            raise BindingError(f"{keyword_location}: namespace '{name}' must open its block right away")
        self.at += 1

        bindings, statics = self.parse_bindings(name)
        return Group(name, number, bindings, statics)

    @staticmethod
    def group_number_of(attribute: Annotation) -> int:
        if len(attribute.arguments) != 1 or attribute.arguments[0][0] != "" or len(attribute.arguments[0][1]) != 1:
            raise BindingError(f"{attribute.location}: 'group' takes exactly one number")

        value = attribute.arguments[0][1][0]
        if not value.isdigit():
            raise BindingError(f"{attribute.location}: '{value}' is not a group number")
        return int(value)

    def parse_bindings(self, group_name: str) -> tuple[list[Binding], list[DeclaredSampler]]:
        bindings: list[Binding] = []
        statics: list[DeclaredSampler] = []
        next_index = 0

        # A `static` attribute stands on the line before the sampler it describes.
        pending: Annotation | None = None

        while True:
            if self.at_end():
                raise BindingError(f"{self.location_here()}: namespace '{group_name}' is never closed")

            if self.is_punctuation("}"):
                self.reject_unclaimed_static(pending)
                self.at += 1
                return bindings, statics

            token = self.current()

            if token.kind == "annotation":
                parsed = self.read_annotation()
                if parsed.name != "static":
                    raise BindingError(f"{token.location}: '{parsed.name}' is not an attribute of a binding")
                if pending is not None:
                    raise BindingError(f"{token.location}: two attributes stand before one declaration")
                pending = parsed
                continue

            if token.kind == "punctuation" and token.text == "#":
                raise BindingError(
                    f"{token.location}: a preprocessor directive is not supported inside an annotated namespace")

            if token.kind != "identifier":
                raise BindingError(f"{token.location}: expected a binding declaration, found '{token.text}'")

            if token.text in REJECTED_KEYWORDS:
                raise BindingError(
                    f"{token.location}: '{token.text}' is not supported inside an annotated namespace")

            binding = self.parse_binding(next_index)

            if pending is not None:
                if binding.type != "sampler":
                    raise BindingError(
                        f"{pending.location}: 'static' describes a sampler, and '{binding.name}' is not one")
                statics.append(DeclaredSampler(binding.name, parse_sampler_state(pending)))
                pending = None

            # An array consumes one index per element, because DXIL numbers every element while SPIR-V numbers
            # the array once -- advancing by one would put the next binding at an address the two disagree on.
            next_index += binding.count
            bindings.append(binding)

    def parse_binding(self, index: int) -> Binding:
        type_name = self.current().text
        type_offset = self.current().offset
        location = self.current().location
        self.at += 1

        # The template arguments say what the resource holds, never where it is bound -- except the one a
        # `push_constants` block needs, which is the struct it mirrors.
        template_argument = ""
        if self.is_punctuation("<"):
            self.at += 1
            if not self.at_end() and self.current().kind == "identifier":
                template_argument = self.current().text
            while not self.is_punctuation(">"):
                if self.at_end() or self.is_punctuation(";") or self.is_punctuation("{"):
                    raise BindingError(f"{location}: '{type_name}' opens an argument list it never closes")
                if self.is_punctuation("<"):
                    raise BindingError(
                        f"{self.current().location}: a nested template argument list is not supported")
                self.at += 1
            self.at += 1  # the '>'

        if self.at_end() or self.current().kind != "identifier":
            raise BindingError(f"{location}: expected a name after '{type_name}'")

        name = self.current().text
        self.at += 1

        count = 1
        if self.is_punctuation("["):
            self.at += 1
            if self.at_end() or self.current().kind != "number":
                raise BindingError(f"{location}: the length of '{name}' must be a decimal literal")

            text = self.current().text
            if not text.isdigit() or int(text) == 0:
                raise BindingError(f"{location}: '{text}' is not an array length")
            count = int(text)
            self.at += 1

            if not self.is_punctuation("]"):
                raise BindingError(f"{location}: '{name}' never closes its array length")
            self.at += 1

        if self.is_punctuation("("):
            raise BindingError(
                f"{location}: a function definition is not supported inside an annotated namespace")
        if self.is_punctuation(":"):
            raise BindingError(f"{location}: '{name}' must not write its own register — the pass owns the address")
        if not self.is_punctuation(";"):
            raise BindingError(f"{location}: expected ';' after '{name}'")

        semicolon_offset = self.current().offset
        self.at += 1

        entry = BINDING_TYPES.get(type_name)
        if entry is None:
            raise BindingError(f"{location}: '{type_name}' is not a resource type this pass knows")

        # Reflection reports the bare name, so one name declared in two groups would reach sg as one binding at
        # two addresses -- which a namespace does nothing to prevent.
        if name in self.binding_names:
            raise BindingError(f"{location}: '{name}' is declared twice")
        self.binding_names.add(name)

        register_class, binding_type, dimension = entry
        return Binding(name, index, count, binding_type, dimension, register_class, type_offset, semicolon_offset,
                       template_argument)


def parse_binding_groups(hlsl: str) -> Bindings:
    """Everything `hlsl` declares, in declaration order.

    Raises BindingError on anything else.
    """
    if PRAGMA_MARKER not in hlsl:
        return Bindings()

    parser = _Parser(lex(hlsl))
    groups = parser.run()

    # After the walk, so the struct an inline-constants block names may be declared on either side of it.
    parser.layout_inline_constants()

    return Bindings(groups, parser.inline_constants, parser.vertex_inputs, parser.payloads)
