#pragma once

#include <clean-core/container/variant.hh>
#include <shaped-graphics-language/ast/parts.hh>

/// Declarations are kept in source order, in an ordered scope and in an unordered one alike.
/// A member of a `struct`, an `enum` or a `binding` is a declaration too, so a body is one list.

/// `module name`
struct sgl::ast::module_decl
{
    /// A `name`, or a `member` chain for a dotted path.
    expr_id path = expr_id::none;

    constexpr bool operator==(module_decl const&) const = default;
};

/// `use module`, `use module as name`
struct sgl::ast::use_decl
{
    expr_id path = expr_id::none;
    /// Empty without `as`.
    source_span alias;

    constexpr bool operator==(use_decl const&) const = default;
};

enum class sgl::ast::receiver_kind : sgl::u8
{
    /// No `self`: a free function, or a static method when it is a member.
    none,
    /// The first parameter is `self`.
    self,
    /// The first parameter is `mut self`.
    mut_self,
};

/// `fun name[type parameters](parameters){bindings} -> return_type`, then `=> value` or a block.
struct sgl::ast::fun_decl
{
    source_span name;
    range_of<field> type_parameters;
    /// `self`, when written, is the first entry here as well as in `receiver`.
    range_of<field> parameters;
    /// A binding entry is a whole list element, so `name as other` and `name = other` can be given a meaning later.
    range_of<argument> bindings;
    expr_id return_type = expr_id::none;
    /// `body_kind::none` is a signature-only declaration, which is valid here and judged later.
    sgl::ast::body body;
    receiver_kind receiver = receiver_kind::none;

    constexpr bool operator==(fun_decl const&) const = default;
};

struct sgl::ast::struct_decl
{
    source_span name;
    range_of<decl_id> members;

    constexpr bool operator==(struct_decl const&) const = default;
};

struct sgl::ast::enum_decl
{
    source_span name;
    range_of<decl_id> members;

    constexpr bool operator==(enum_decl const&) const = default;
};

/// `type name = value`
struct sgl::ast::type_decl
{
    source_span name;
    /// A type position, like the right side of `:`, `->` and `as`.
    expr_id value = expr_id::none;

    constexpr bool operator==(type_decl const&) const = default;
};

/// `const name = value`, `const name : type = value`
struct sgl::ast::const_decl
{
    source_span name;
    expr_id type = expr_id::none;
    expr_id value = expr_id::none;

    constexpr bool operator==(const_decl const&) const = default;
};

/// `binding name:` with members, or the composition `binding name = other`, `binding name = (a, b)`.
struct sgl::ast::binding_decl
{
    source_span name;
    range_of<decl_id> members;
    /// `none` for the block form.
    expr_id composition = expr_id::none;

    constexpr bool operator==(binding_decl const&) const = default;
};

/// `sampler name:` with one `setting = value` per line.
struct sgl::ast::sampler_decl
{
    source_span name;
    range_of<argument> settings;

    constexpr bool operator==(sampler_decl const&) const = default;
};

/// `notation pattern => replacement`
struct sgl::ast::notation_decl
{
    expr_id pattern = expr_id::none;
    expr_id replacement = expr_id::none;

    constexpr bool operator==(notation_decl const&) const = default;
};

/// A `field` as a member line; its attributes are on the field.
struct sgl::ast::field_decl
{
    field_id field = field_id::none;

    constexpr bool operator==(field_decl const&) const = default;
};

/// `name => value`: a computed, read-only member without a parameter list.
struct sgl::ast::property_decl
{
    source_span name;
    sgl::ast::body body;

    constexpr bool operator==(property_decl const&) const = default;
};

/// A bare name in an `enum` body, or `name = value`.
struct sgl::ast::enum_case_decl
{
    source_span name;
    /// `none` for a bare name.
    expr_id value = expr_id::none;

    constexpr bool operator==(enum_case_decl const&) const = default;
};

struct sgl::ast::invalid_decl
{
    constexpr bool operator==(invalid_decl const&) const = default;
};

struct sgl::ast::decl
{
    form_id form = form_id::none;
    range_of<attribute> attributes;

    cc::variant<invalid_decl,
                module_decl,
                use_decl,
                fun_decl,
                struct_decl,
                enum_decl,
                type_decl,
                const_decl,
                binding_decl,
                sampler_decl,
                notation_decl,
                field_decl,
                property_decl,
                enum_case_decl>
        node;

    bool operator==(decl const&) const = default;
};
