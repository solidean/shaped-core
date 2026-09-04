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
ATTRIBUTE_NAMES = ("group", "static", "push_constants", "payload", "vertex_input")

# The attributes whose meaning has not landed yet.
UNIMPLEMENTED_ATTRIBUTES = ("push_constants", "payload", "vertex_input")

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
    first_on_line: bool


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

        if len(values) != 1:
            raise BindingError(f"{attribute.location}: '{key}' takes exactly one value")

        if key in SAMPLER_FLOAT_KEYS:
            try:
                float(values[0])
            except ValueError:
                raise BindingError(f"{attribute.location}: '{values[0]}' is not a number") from None
            fields[key] = values[0]
            continue

        if key == "max_anisotropy":
            if not values[0].isdigit() or int(values[0]) == 0:
                raise BindingError(f"{attribute.location}: '{values[0]}' is not an anisotropy")
            fields[key] = values[0]
            continue

        raise BindingError(f"{attribute.location}: '{key}' is not a field of sg::sampler")

    return fields


# ---------------------------------------------------------------------------------------------------
# the tokenizer
# ---------------------------------------------------------------------------------------------------


def _is_identifier_start(c: str) -> bool:
    return c.isascii() and (c.isalpha() or c == "_")


def _is_identifier_char(c: str) -> bool:
    return c.isascii() and (c.isalnum() or c == "_")


def _is_space(c: str) -> bool:
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
        tokens.append(Token(kind, text, offset, length, Location(file, line), not line_has_token))
        line_has_token = True

    while i < size:
        c = hlsl[i]

        if c == "\n":
            line += 1
            line_has_token = False
            i += 1
            continue

        if _is_space(c):
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
        while i < size and _is_space(text[i]):
            i += 1

    def read_word() -> str:
        nonlocal i
        start = i
        while i < size and not _is_space(text[i]) and text[i] not in "=(),":
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
        if parsed.name in UNIMPLEMENTED_ATTRIBUTES:
            raise BindingError(f"{token.location}: the '{parsed.name}' attribute is not supported yet")
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

            self.reject_unclaimed(pending)
            self.at += 1

        self.reject_unclaimed(pending)
        return groups

    @staticmethod
    def reject_unclaimed_static(pending: Annotation | None) -> None:
        if pending is not None:
            raise BindingError(f"{pending.location}: a 'static' attribute must stand before a sampler declaration")

    @staticmethod
    def reject_unclaimed(pending: Annotation | None) -> None:
        if pending is not None:
            raise BindingError(
                f"{pending.location}: a '{pending.name}' attribute must stand before a namespace declaration")

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

        # The template arguments say what the resource holds, never where it is bound.
        if self.is_punctuation("<"):
            self.at += 1
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
        return Binding(name, index, count, binding_type, dimension, register_class, type_offset, semicolon_offset)


def parse_binding_groups(hlsl: str) -> list[Group]:
    """Every binding group `hlsl` declares, in declaration order.

    Raises BindingError on anything else.
    """
    if PRAGMA_MARKER not in hlsl:
        return []
    return _Parser(lex(hlsl)).run()
