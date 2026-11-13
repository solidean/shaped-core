#include "interpret.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/impl/soft_math.hh>
#include <shaped-graphics-language/builtins/subgroup_calls.hh>
#include <shaped-graphics-language/interpret/impl/textures.hh>
#include <shaped-graphics-language/legalize/impl/walk.hh>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;

namespace
{
cc::span<member_info const> members_of(checked_module const& m, type_id type)
{
    if (!is_valid(type) || index_of(type) >= m.types.size())
        return {};
    auto const range = m.at(type).members;
    if (isize(range.first) + isize(range.count) > m.members.size())
        return {};
    return m.at(range);
}

void append_zero(checked_module const& m, type_id type, cc::vector<scalar>& leaves, int depth)
{
    if (depth > k_max_depth)
        return;
    // A builtin type says what it is made of, whether or not the prelude gives it fields.
    if (auto const* const record = m.builtin_type_of(type))
    {
        for (auto i = 0; i < record->leaf_count; ++i)
            leaves.push_back({.kind = record->leaf_kind, .bits = 0});
        return;
    }
    if (is_valid(type) && m.at(type).kind == type_kind::enumeration)
    {
        leaves.push_back({.kind = value_kind::scalar_int, .bits = 0});
        return;
    }
    if (is_valid(type) && m.at(type).kind == type_kind::atomic)
    {
        append_zero(m, m.at(type).element, leaves, depth + 1);
        return;
    }
    if (is_valid(type) && m.at(type).kind == type_kind::array)
    {
        for (auto i = 0; i < m.at(type).count; ++i)
            append_zero(m, m.at(type).element, leaves, depth + 1);
        return;
    }
    for (auto const& member : members_of(m, type))
        append_zero(m, member.type, leaves, depth + 1);
}

/// One call that exchanges values between lanes, the `occurrence`-th time a lane runs it, and what each lane gave it.
/// A subgroup operation stands only where every lane arrives together (CHK-377), so the n-th run of a call in one
/// lane meets the n-th in every other.
struct exchange
{
    flat_expr_id call = flat_expr_id::none;
    i32 occurrence = 0;
    /// Parallel to the lanes: the scalars of each lane's arguments, and whether it gave any.
    cc::vector<cc::vector<scalar>> offers;
    cc::vector<bool> has;

    [[nodiscard]] bool is_complete() const
    {
        for (auto const h : has)
            if (!h)
                return false;
        return true;
    }
};

/// Every exchange of one round of a run's lanes (EVAL-143).
struct exchange_table
{
    cc::vector<exchange> entries;

    [[nodiscard]] exchange const* find(flat_expr_id call, i32 occurrence) const
    {
        for (auto const& x : entries)
            if (x.call == call && x.occurrence == occurrence)
                return &x;
        return nullptr;
    }

    exchange& at(flat_expr_id call, i32 occurrence, i32 lanes)
    {
        for (auto& x : entries)
            if (x.call == call && x.occurrence == occurrence)
                return x;
        auto& x = entries.emplace_back(exchange{.call = call, .occurrence = occurrence});
        x.offers.resize_to_defaulted(lanes);
        x.has.resize_to_filled(lanes, false);
        return x;
    }

    /// Whether every lane gave every exchange the same arguments in both rounds, which settles the run.
    [[nodiscard]] bool is_same_as(exchange_table const& other) const
    {
        if (entries.size() != other.entries.size())
            return false;
        for (auto const& x : entries)
        {
            auto const* const y = other.find(x.call, x.occurrence);
            if (y == nullptr || x.has.size() != y->has.size())
                return false;
            for (auto i = isize(0); i < x.has.size(); ++i)
                if (x.has[i] != y->has[i] || !ast::impl::is_equal(x.offers[i], y->offers[i]))
                    return false;
        }
        return true;
    }
};

/// What the lanes of a run share, which each lane starts from and leaves to the next (EVAL-142).
struct shared_memory
{
    cc::vector<buffer_contents> buffers;
    cc::vector<bool> is_stored;
    cc::vector<texture_contents> textures;
    cc::vector<bool> is_texture_stored;
    /// Parallel to `textures`: one a test stated rather than its driver bound, which grows (EVAL-127).
    cc::vector<bool> is_stated;
    cc::vector<sampler_binding> samplers;
};

/// `a op b` of one scalar each, as a subgroup's reduction combines them, at the width of their kind.
scalar combined(builtins::subgroup_op op, scalar a, scalar b)
{
    using builtins::subgroup_op;
    auto const kind = a.kind;
    auto const x = a.widened();
    auto const y = b.widened();
    auto result = x;
    if (x.kind == value_kind::scalar_float)
    {
        auto const p = x.as_float();
        auto const q = y.as_float();
        auto const r = op == subgroup_op::min ? (q < p ? q : p)
                     : op == subgroup_op::max ? (q > p ? q : p)
                     : (op == subgroup_op::mul || op == subgroup_op::exclusive_mul || op == subgroup_op::inclusive_mul)
                         ? p * q
                         : p + q;
        result = scalar::of(r);
    }
    else
    {
        auto const is_signed = x.kind == value_kind::scalar_int;
        auto const p = x.bits;
        auto const q = y.bits;
        switch (op)
        {
        case subgroup_op::min:
            result.bits = is_signed ? (i32(q) < i32(p) ? q : p) : (q < p ? q : p);
            break;
        case subgroup_op::max:
            result.bits = is_signed ? (i32(q) > i32(p) ? q : p) : (q > p ? q : p);
            break;
        case subgroup_op::mul:
        case subgroup_op::exclusive_mul:
        case subgroup_op::inclusive_mul:
            result.bits = p * q;
            break;
        case subgroup_op::bit_and:
            result.bits = p & q;
            break;
        case subgroup_op::bit_or:
            result.bits = p | q;
            break;
        case subgroup_op::bit_xor:
            result.bits = p ^ q;
            break;
        default:
            result.bits = p + q;
            break;
        }
    }
    return result.narrowed_to(kind);
}

/// The identity of a scan's operation, for the lanes below the first: 0 of a sum, 1 of a product.
scalar identity_of(builtins::subgroup_op op, value_kind kind)
{
    auto const is_product = op == builtins::subgroup_op::exclusive_mul || op == builtins::subgroup_op::inclusive_mul;
    auto const wide = scalar{.kind = kind}.widened().kind;
    auto one = wide == value_kind::scalar_float ? scalar::of(is_product ? 1.0f : 0.0f)
                                                : scalar{.kind = wide, .bits = is_product ? 1u : 0u};
    return one.narrowed_to(kind);
}

enum class flow_kind : u8
{
    normal,
    leave,
    continue_,
    break_,
    return_,
    failed,
};

/// What a statement or an expression ended in; an expression can exit too, since it may hold a block.
struct flow
{
    flow_kind kind = flow_kind::normal;
    label_id label = label_id::none;

    [[nodiscard]] bool is_normal() const { return kind == flow_kind::normal; }
};

struct machine
{
    checked_module const& m;
    flat_entry_point const& e;
    run_inputs const& inputs;
    run_limits const& limits;
    i64 fuel = 0;
    outcome out;

    cc::vector<value> locals;
    cc::vector<bool> is_set;

    /// One member of a `@workgroup` binding, which the run holds from its start: memory of one invocation's workgroup.
    struct workgroup_cell
    {
        symbol_id binding = symbol_id::none;
        i32 member = -1;
        value memory;
        /// Parallel to `memory.leaves`: nothing is defined before it is stored (CHK-292).
        cc::vector<bool> is_written;
    };
    cc::vector<workgroup_cell> workgroup;
    /// Parallel to `out.buffers`: whether the run stored to that buffer, which is what the outcome keeps.
    cc::vector<bool> is_stored;
    /// Every texture and image the run binds, which an image store changes, and whether one did.
    cc::vector<texture_contents> textures;
    cc::vector<bool> is_texture_stored;
    /// Parallel to 	extures: one the run stated rather than its driver bound, which grows as it is stated (EVAL-127).
    cc::vector<bool> is_stated;
    /// The state of each dynamic sampler the driver set or the run stated (EVAL-128).
    cc::vector<sampler_binding> samplers;

    /// EVAL-140: this lane among the run's, which exchange values at every subgroup operation and derivative.
    i32 lane = 0;
    i32 lane_count = 1;
    /// What every lane gave each exchange in the round before, which this lane's results are made of.
    exchange_table const* previous = nullptr;
    /// What this round's lanes give, this one's included.
    exchange_table* current = nullptr;
    /// How often this lane ran each exchanging call so far.
    struct occurrence
    {
        flat_expr_id call = flat_expr_id::none;
        i32 count = 0;
    };
    cc::vector<occurrence> occurrences;
    /// What a call reads of a texture the run binds nothing to: 0 by 0, with no level (EVAL-121).
    texture_contents unbound_texture;
    /// Each weak compare-exchange of the source that met `expected` so far, and whether that last attempt failed.
    /// One of the source is one wherever it was inlined (EVAL-100).
    struct weak_exchange
    {
        origin from;
        bool failed = false;
    };
    cc::vector<weak_exchange> weak_exchanges;
    /// The value a `leave` or a `return` under way carries.
    value carried;
    int depth = 0;

    flow fail(run_status status, cc::string detail)
    {
        if (out.status == run_status::ok)
        {
            out.status = status;
            out.detail = cc::move(detail);
        }
        return {.kind = flow_kind::failed};
    }

    flow type_error(cc::string detail) { return fail(run_status::type_error, cc::move(detail)); }

    /// One step; false when the run is over.
    bool burn()
    {
        if (--fuel >= 0)
        {
            // a relaxed load every 4096 steps: a stop is seen within a fraction of a millisecond, and costs the run nothing measurable
            if (limits.stop != nullptr && (fuel & 4095) == 0 && limits.stop->load(cc::memory_order_relaxed))
            {
                fail(run_status::stopped, "");
                return false;
            }
            return true;
        }
        fail(run_status::out_of_fuel, "");
        return false;
    }

    // ---- expressions ------------------------------------------------------------------------------------------------

    flow eval_all(ast::range_of<flat_expr_id> range, cc::vector<value>& values)
    {
        if (!is_known(e, range))
            return type_error("an argument list that reaches outside the tree");
        for (auto const id : e.at(range))
        {
            auto v = value();
            if (auto const f = eval(id, v); !f.is_normal())
                return f;
            values.push_back(cc::move(v));
        }
        return {};
    }

    flow slice_member(value const& object, i32 index, value& result)
    {
        auto const members = members_of(m, object.type);
        if (index < 0 || index >= members.size())
            return type_error(cc::format("member {} of '{}'", index, m.name_of(object.type)));
        auto offset = isize(0);
        for (auto i = 0; i < index; ++i)
            offset += leaf_count_of(m, members[i].type);
        auto const count = leaf_count_of(m, members[index].type);
        if (offset + count > object.leaves.size())
            return type_error("a value with fewer scalars than its type");
        result.type = members[index].type;
        result.leaves.clear();
        result.leaves.push_back_range(cc::span<scalar const>(object.leaves).subspan({.offset = offset, .size = count}));
        return {};
    }

    /// An index is one `int` or one `uint` (CHK-287).
    [[nodiscard]] static bool is_index(value const& index)
    {
        return index.leaves.size() == 1
            && (index.leaves[0].kind == value_kind::scalar_int || index.leaves[0].kind == value_kind::scalar_uint);
    }

    /// The element an index names, which a `uint` past the `int`s names as well.
    [[nodiscard]] static isize index_value(value const& index)
    {
        auto const& leaf = index.leaves[0];
        return leaf.kind == value_kind::scalar_uint ? isize(leaf.as_uint()) : isize(leaf.as_int());
    }

    /// Where element `index` of an array of `type` starts among its scalars; outside it is a program error (EVAL-90).
    flow element_offset(type_id type, value const& index, isize& offset, isize& count)
    {
        if (!is_index(index))
            return type_error("an array index that is no int and no uint");
        // a lane of a vector, one scalar per field, past whose last lane is the same program error (CHK-395)
        if (is_valid(type) && m.at(type).kind == type_kind::structure && m.at(type).has_swizzles)
        {
            auto const lanes = m.at(m.at(type).members);
            auto const at = index_value(index);
            if (at < 0 || at >= lanes.size())
                return fail(run_status::program_error,
                            cc::format("the index {} is out of bounds of {}", at, m.name_of(type)));
            count = leaf_count_of(m, lanes[0].type);
            offset = at * count;
            return {};
        }
        if (!is_valid(type) || m.at(type).kind != type_kind::array)
            return type_error("an element of what is no array");
        auto const& info = m.at(type);
        auto const at = index_value(index);
        if (at < 0 || at >= info.count)
            return fail(run_status::program_error,
                        cc::format("the index {} is out of bounds of {}", at, m.name_of(type)));
        count = leaf_count_of(m, info.element);
        offset = at * count;
        return {};
    }

    /// A texel's atomic, whose first argument is the image rather than the atomic.
    [[nodiscard]] bool is_texel_atomic(flat_call const& c) const
    {
        auto const arguments = e.at(c.arguments);
        return !arguments.empty() && is_known(e, arguments[0]) && m.at(e.at(arguments[0]).type).kind == type_kind::image;
    }

    /// EVAL-93: the atomic's place, then the other arguments, then the update, in one step.
    flow atomic_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record, value& result)
    {
        auto const arguments = e.at(c.arguments);
        if (arguments.empty() || !is_known(e, arguments[0]))
            return type_error("an atomic call without its atomic");

        // where the atomic is: an element of a buffer's scalars, a cell of workgroup memory, or an image's texel
        auto buffer = isize(-1);
        auto where = place_ref();
        auto texture = isize(-1);
        auto offset = isize(0);
        auto first_value = isize(1);
        auto const atomic = arguments[0];
        if (auto const* const element = e.at(atomic).node.try_as<flat_buffer_element>())
        {
            if (auto const f = locate(*element, e.at(atomic).type, buffer, offset); !f.is_normal())
                return f;
        }
        else if (is_in_workgroup(atomic))
        {
            auto type = type_id::none;
            if (auto const f = locate_place(atomic, where, offset, type); !f.is_normal())
                return f;
        }
        else if (is_texel_atomic(c))
        {
            // EVAL-100: the texel the image's coordinate and layer name, which the run holds as it holds any texel
            auto binding = symbol_id::none;
            auto member = i32(-1);
            auto element = i32(0);
            if (auto const f = resource_of(atomic, binding, member, element); !f.is_normal())
                return f;
            auto const axes = builtins::texture::axes_of(m.at(e.at(atomic).type).shape);
            auto coord = value();
            if (arguments.size() < 2)
                return type_error("a texel atomic without its coordinate");
            if (auto const f = eval(arguments[1], coord); !f.is_normal())
                return f;
            auto layer = value();
            first_value = 2;
            if (axes.is_array)
            {
                if (arguments.size() < 3)
                    return type_error("a texel atomic of an array without its layer");
                if (auto const f = eval(arguments[2], layer); !f.is_normal())
                    return f;
                first_value = 3;
            }
            for (auto i = isize(0); i < textures.size(); ++i)
                if (textures[i].binding == binding && textures[i].member == member && textures[i].element == element)
                    texture = i;
            if (texture < 0)
                return fail(run_status::program_error,
                            cc::format("an atomic {} of an image the run binds no texels to", record.name));
            if (auto const failure = texel_offset(textures[texture], axes, coord.leaves,
                                                  layer.leaves.empty() ? 0 : layer.leaves[0].as_int(), offset);
                !failure.is_ok())
                return fail(failure.status, cc::format("{}, in an atomic {}", failure.detail, record.name));
        }
        else
            return type_error("an atomic that is no buffer element, no workgroup memory and no texel");

        auto in = cc::vector<scalar>();
        in.push_back({});
        for (auto k = first_value; k < arguments.size(); ++k)
        {
            auto v = value();
            if (auto const f = eval(arguments[k], v); !f.is_normal())
                return f;
            in.push_back_range(v.leaves);
        }

        auto const is_store = !is_valid(record.result) && !record.gives_prelude_struct;
        if (where.cell >= 0)
        {
            auto const& cell = workgroup[where.cell];
            if (!is_store && !cell.is_written[offset])
                return fail(
                    run_status::program_error,
                    cc::format("an atomic {} of {}.{} where nothing was stored", record.name, m.at(cell.binding).name,
                               m.at(m.bindings[m.at(cell.binding).info].members)[cell.member].name));
            in[0] = cell.memory.leaves[offset];
        }
        else if (texture >= 0)
            in[0] = textures[texture].texels[offset];
        else
            in[0] = out.buffers[buffer].leaves[offset];

        auto after = cc::vector<scalar>();
        record.evaluate(in, after);
        if (after.size() != 1)
            return type_error(cc::format("an atomic '{}' whose evaluator gave no one value", record.name));
        // EVAL-100: a weak compare-exchange that meets `expected` fails every other time, starting with the first
        auto is_spurious = false;
        if (record.gives_prelude_struct && in.size() == 3 && in[0].bits == in[1].bits && limits.fails_weak_exchanges)
        {
            auto* site = static_cast<weak_exchange*>(nullptr);
            for (auto& w : weak_exchanges)
                if (w.from == x.from)
                    site = &w;
            if (site == nullptr)
                site = &weak_exchanges.emplace_back(weak_exchange{.from = x.from});
            is_spurious = !site->failed;
            site->failed = is_spurious;
            if (is_spurious)
                after[0] = in[0];
        }
        if (where.cell >= 0)
        {
            workgroup[where.cell].memory.leaves[offset] = after[0];
            workgroup[where.cell].is_written[offset] = true;
        }
        else if (texture >= 0)
        {
            textures[texture].texels[offset] = after[0];
            is_texture_stored[texture] = true;
        }
        else
        {
            out.buffers[buffer].leaves[offset] = after[0];
            is_stored[buffer] = true;
        }

        result.type = x.type;
        result.leaves.clear();
        if (!is_store)
            result.leaves.push_back(in[0]);
        if (record.gives_prelude_struct)
            result.leaves.push_back({.kind = value_kind::boolean, .bits = in[0].bits == in[1].bits && !is_spurious});
        out.trace.push_back(result);
        return {};
    }

    /// EVAL-97: a load or store of `bytes`, a word at a time at a byte offset, against the memory the inputs hold.
    flow bytes_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record, value& result)
    {
        auto const arguments = e.at(c.arguments);
        auto const count = record.is_bytes_store ? 3 : 2;
        if (arguments.size() != count || !is_known(e, arguments[0]))
            return type_error(cc::format("a call of '{}' without its bytes", record.name));
        auto buffer = isize(-1);
        if (auto const f = buffer_named(arguments[0], buffer); !f.is_normal())
            return f;

        auto offset = value();
        if (auto const f = eval(arguments[1], offset); !f.is_normal())
            return f;
        if (offset.leaves.size() != 1 || offset.leaves[0].kind != value_kind::scalar_uint)
            return type_error("a byte offset that is no uint");
        auto stored = value();
        if (record.is_bytes_store)
            if (auto const f = eval(arguments[2], stored); !f.is_normal())
                return f;

        // no target agrees on a word that straddles two, or on one past the end, so a correct program has neither
        auto const at = isize(offset.leaves[0].as_uint());
        auto const words = isize(record.bytes_words);
        auto& memory = out.buffers[buffer].leaves;
        if (at % 4 != 0)
            return fail(run_status::program_error, cc::format("the byte offset {} is no multiple of 4", at));
        if (at / 4 + words > memory.size())
            return fail(run_status::program_error,
                        cc::format("{} words from byte {} are out of bounds of {} bytes", words, at, memory.size() * 4));

        result.type = x.type;
        result.leaves.clear();
        if (record.is_bytes_store)
        {
            if (stored.leaves.size() != words)
                return type_error(cc::format("a call of '{}' storing a value of the wrong width", record.name));
            for (auto k = isize(0); k < words; ++k)
                memory[at / 4 + k] = stored.leaves[k];
            is_stored[buffer] = true;
        }
        else
            for (auto k = isize(0); k < words; ++k)
                result.leaves.push_back(memory[at / 4 + k]);
        out.trace.push_back(result);
        return {};
    }

    /// The resource `id` names: a binding member, or an element of a binding array of them, whose index runs here.
    flow resource_of(flat_expr_id id, symbol_id& binding, i32& member, i32& element)
    {
        if (!is_known(e, id))
            return type_error("a resource that names nothing");
        element = 0;
        auto named = id;
        if (auto const* const array = e.at(id).node.try_as<flat_element>())
        {
            auto index = value();
            if (auto const f = eval(array->index, index); !f.is_normal())
                return f;
            if (index.leaves.size() != 1)
                return type_error("an index into a binding array that is no int");
            element = index.leaves[0].as_int();
            named = array->object;
        }
        auto const* const b = is_known(e, named) ? e.at(named).node.try_as<flat_binding_member>() : nullptr;
        if (b == nullptr)
            return type_error("a resource that no binding member names");
        binding = b->binding;
        member = b->member;
        return {};
    }

    /// The position in `out.buffers` of the memory `id` names; its index, if it is a binding array's, runs here.
    flow buffer_named(flat_expr_id id, isize& buffer)
    {
        auto binding = symbol_id::none;
        auto member = i32(-1);
        auto element = i32(0);
        if (auto const f = resource_of(id, binding, member, element); !f.is_normal())
            return f;
        buffer = -1;
        for (auto i = isize(0); i < out.buffers.size(); ++i)
            if (out.buffers[i].binding == binding && out.buffers[i].member == member && out.buffers[i].element == element)
                buffer = i;
        // EVAL-90: past a binding array's last element, as past an array's
        if (buffer < 0 && e.at(id).node.is<flat_element>())
            return fail(run_status::program_error,
                        cc::format("the index {} is out of bounds of a binding array", element));
        if (buffer < 0)
            return type_error("a buffer the inputs do not hold");
        return {};
    }

    /// EVAL-123: the sampler `id` names: a file-scope one or a static member, whose state the source sets, or a
    /// dynamic member, whose state the driver sets and otherwise `sampler_state`'s default.
    flow sampler_of(flat_expr_id id, sampler_state& state)
    {
        if (!is_known(e, id))
            return type_error("a sampler that names nothing");
        if (auto const* const file = e.at(id).node.try_as<flat_file_sampler>())
        {
            auto const info = m.at(file->sampler).info;
            if (info < 0 || info >= m.samplers.size())
                return type_error("a file-scope sampler with no state");
            state = m.samplers[info];
            return {};
        }
        auto binding = symbol_id::none;
        auto member = i32(-1);
        auto element = i32(0);
        if (auto const f = resource_of(id, binding, member, element); !f.is_normal())
            return f;
        auto const members = m.at(m.bindings[m.at(binding).info].members);
        if (member < 0 || member >= members.size())
            return type_error("a sampler member its binding does not have");
        if (auto const s = members[member].static_sampler; s >= 0 && s < m.samplers.size())
        {
            state = m.samplers[s];
            return {};
        }
        state = sampler_state();
        for (auto const& bound : samplers)
            if (bound.binding == binding && bound.member == member && bound.element == element)
                state = bound.state;
        return {};
    }

    /// EVAL-120: a texture or an image call, on the texels the run binds; its arguments run left to right.
    flow texture_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record, value& result)
    {
        auto const call = builtins::texture::call::unpack(record.write.data);
        auto const layout = builtins::texture::layout_of(call);
        if (!is_known(e, c.arguments) || c.arguments.count == 0)
            return type_error(cc::format("a call of '{}' without its texture", record.name));
        auto const arguments = e.at(c.arguments);
        auto args = cc::vector<value>();
        args.resize_to_defaulted(arguments.size());
        auto binding = symbol_id::none;
        auto member = i32(-1);
        auto element = i32(0);
        auto state = sampler_state();
        auto has_sampler = false;
        for (auto k = isize(0); k < arguments.size(); ++k)
        {
            auto f = flow();
            if (k == 0)
                f = resource_of(arguments[0], binding, member, element);
            else if (k == layout.sampler)
            {
                f = sampler_of(arguments[k], state);
                has_sampler = true;
            }
            else
                f = eval(arguments[k], args[k]);
            if (!f.is_normal())
                return f;
        }

        auto texture = isize(-1);
        for (auto i = isize(0); i < textures.size(); ++i)
            if (textures[i].binding == binding && textures[i].member == member && textures[i].element == element)
                texture = i;
        result.type = x.type;
        result.leaves.clear();

        // EVAL-127: a test states a texel of what it binds, and one its driver bound nothing to grows to hold it
        if (call.what == builtins::texture::op::test_texel)
        {
            if (texture < 0)
            {
                textures.push_back({.binding = binding, .member = member, .element = element});
                is_texture_stored.push_back(false);
                is_stated.push_back(true);
                texture = textures.size() - 1;
            }
            if (auto const failure = state_texel(call, textures[texture], is_stated[texture], args); !failure.is_ok())
                return fail(failure.status, cc::format("{}, in a call of '{}'", failure.detail, record.name));
            // a texel of an image the driver bound comes back to it, as a store's does
            if (call.is_image && !is_stated[texture])
                is_texture_stored[texture] = true;
            out.trace.push_back(result);
            return {};
        }

        unbound_texture = texture_contents{.binding = binding, .member = member, .element = element};
        auto& contents = texture >= 0 ? textures[texture] : unbound_texture;

        // EVAL-144: a sample that picks its own level takes it from its coordinate's derivatives across the quad
        auto gradients = coordinate_gradients();
        auto has_gradients = false;
        if (record.uses_derivatives && lane_count >= 4 && args.size() > 1)
        {
            auto const* const before = exchanged(x, args[1].leaves);
            auto const quad = lane & ~3;
            if (before != nullptr && quad + 3 < lane_count)
            {
                auto const& o = before->offers;
                for (auto k = isize(0); k < o[quad].size(); ++k)
                {
                    gradients.x.push_back(scalar::of(o[quad + 1][k].as_float() - o[quad][k].as_float()));
                    gradients.y.push_back(scalar::of(o[quad + 2][k].as_float() - o[quad][k].as_float()));
                }
                has_gradients = true;
            }
        }

        auto stored = false;
        if (auto const failure = run_texture_call(call, contents, has_sampler ? &state : nullptr, args, result.leaves,
                                                  stored, has_gradients ? &gradients : nullptr);
            !failure.is_ok())
            return fail(failure.status, cc::format("{}, in a call of '{}'", failure.detail, record.name));
        if (stored && texture >= 0)
            is_texture_stored[texture] = true;
        if (result.leaves.size() != leaf_count_of(m, x.type))
            return type_error(cc::format("a call of '{}' with a result of the wrong type", record.name));
        if (!c.is_pure)
            out.trace.push_back(result);
        return {};
    }

    /// EVAL-143: gives `offer` to the exchange this lane's call `x` meets now, and returns what every lane gave it
    /// in the round before; null where some lane gave it nothing then, so this round's result is no result yet.
    exchange const* exchanged(flat_expr const& x, cc::vector<scalar> offer)
    {
        auto const id = flat_expr_id(i32(&x - e.exprs.data()));
        auto n = i32(-1);
        for (auto& o : occurrences)
            if (o.call == id)
                n = o.count++;
        if (n < 0)
        {
            occurrences.push_back({.call = id, .count = 1});
            n = 0;
        }
        if (current == nullptr || previous == nullptr)
            return nullptr;
        auto& now = current->at(id, n, lane_count);
        now.offers[lane] = cc::move(offer);
        now.has[lane] = true;
        auto const* const before = previous->find(id, n);
        return before != nullptr && before->is_complete() ? before : nullptr;
    }

    /// EVAL-143: a subgroup operation over what every lane gave it, in lane order.
    flow subgroup_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record, value& result)
    {
        using builtins::subgroup_op;
        auto args = cc::vector<value>();
        if (auto const f = eval_all(c.arguments, args); !f.is_normal())
            return f;
        auto offer = cc::vector<scalar>();
        for (auto const& a : args)
            offer.push_back_range(a.leaves);
        auto const takes_lane = args.size() == 2;
        auto const width = takes_lane ? offer.size() - 1 : offer.size();
        auto const named = takes_lane && !offer.empty() ? offer[width].as_int() : 0;
        auto const* const before = exchanged(x, offer);

        result = zero_value(m, x.type);
        if (before == nullptr)
        {
            out.trace.push_back(result);
            return {};
        }
        result.leaves.clear();
        auto const& offers = before->offers;
        auto const op = subgroup_op(record.write.data);
        auto const n = lane_count;
        auto const from_lane = [&](i32 source)
        {
            // a lane that names no invocation of the subgroup reads zero (CHK-376)
            if (source < 0 || source >= n)
            {
                result = zero_value(m, x.type);
                return;
            }
            for (auto k = isize(0); k < width; ++k)
                result.leaves.push_back(offers[source][k]);
        };
        switch (op)
        {
        case subgroup_op::all:
        case subgroup_op::any:
        {
            auto is = op == subgroup_op::all;
            for (auto l = 0; l < n; ++l)
                is = op == subgroup_op::all ? is && offers[l][0].as_bool() : is || offers[l][0].as_bool();
            result.leaves.push_back(scalar::of(is));
            break;
        }
        case subgroup_op::ballot:
        {
            u32 words[4] = {0, 0, 0, 0};
            for (auto l = 0; l < n && l < 128; ++l)
                if (offers[l][0].as_bool())
                    words[l / 32] |= 1u << (l % 32);
            for (auto const w : words)
                result.leaves.push_back(scalar::of_uint(w));
            break;
        }
        case subgroup_op::exclusive_add:
        case subgroup_op::exclusive_mul:
        case subgroup_op::inclusive_add:
        case subgroup_op::inclusive_mul:
        {
            auto const is_inclusive = op == subgroup_op::inclusive_add || op == subgroup_op::inclusive_mul;
            for (auto k = isize(0); k < width; ++k)
            {
                auto acc = identity_of(op, offers[lane][k].kind);
                for (auto l = 0; l < lane + (is_inclusive ? 1 : 0); ++l)
                    acc = combined(op, acc, offers[l][k]);
                result.leaves.push_back(acc);
            }
            break;
        }
        case subgroup_op::broadcast:
        case subgroup_op::shuffle:
            from_lane(named);
            break;
        case subgroup_op::broadcast_first:
            from_lane(0);
            break;
        case subgroup_op::shuffle_xor:
            from_lane(lane ^ named);
            break;
        case subgroup_op::shuffle_up:
            from_lane(lane - named);
            break;
        case subgroup_op::shuffle_down:
            from_lane(lane + named);
            break;
        case subgroup_op::quad_swap_x:
            from_lane(lane ^ 1);
            break;
        case subgroup_op::quad_swap_y:
            from_lane(lane ^ 2);
            break;
        case subgroup_op::quad_swap_diagonal:
            from_lane(lane ^ 3);
            break;
        case subgroup_op::quad_broadcast:
            from_lane((lane & ~3) | named);
            break;
        default:
            for (auto k = isize(0); k < width; ++k)
            {
                auto acc = offers[0][k];
                for (auto l = 1; l < n; ++l)
                    acc = combined(op, acc, offers[l][k]);
                result.leaves.push_back(acc);
            }
            break;
        }
        result.type = x.type;
        if (result.leaves.size() != leaf_count_of(m, x.type))
            return type_error(cc::format("a call of '{}' with a result of the wrong type", record.name));
        out.trace.push_back(result);
        return {};
    }

    /// EVAL-144: a derivative across the quad; the coarse one along its top row and down its left column, the fine one
    /// along the lane's own row and down its own column.
    flow derivative_call(flat_expr const& x, flat_call const& c, builtins::function_record const& record, value& result)
    {
        auto args = cc::vector<value>();
        if (auto const f = eval_all(c.arguments, args); !f.is_normal())
            return f;
        if (args.size() != 1)
            return type_error(cc::format("a call of '{}' without its one argument", record.name));
        auto const* const before = exchanged(x, args[0].leaves);
        result = zero_value(m, x.type);
        auto const quad = lane & ~3;
        if (before == nullptr || quad + 3 >= lane_count)
            return {};
        auto const& o = before->offers;
        auto const name = cc::string_view(record.name);
        auto const is_fine = name.ends_with("_fine");
        // the lanes of a quad are (0, 0), (1, 0), (0, 1), (1, 1)
        auto const row = is_fine ? quad + (lane & 2) : quad;
        auto const column = is_fine ? quad + (lane & 1) : quad;
        auto const is_x = name.starts_with("ddx");
        auto const is_y = name.starts_with("ddy");
        result.leaves.clear();
        for (auto k = isize(0); k < o[quad].size(); ++k)
        {
            auto const dx = o[row + 1][k].as_float() - o[row][k].as_float();
            auto const dy = o[column + 2][k].as_float() - o[column][k].as_float();
            auto const fw = (dx < 0.0f ? -dx : dx) + (dy < 0.0f ? -dy : dy);
            result.leaves.push_back(scalar::of(is_x ? dx : is_y ? dy : fw));
        }
        if (result.leaves.size() != leaf_count_of(m, x.type))
            return type_error(cc::format("a call of '{}' with a result of the wrong type", record.name));
        return {};
    }

    /// EVAL-128: `s.test_state(from)` gives a dynamic sampler the state of a sampler whose state the source sets.
    flow state_sampler(flat_expr const& x, flat_call const& c, value& result)
    {
        auto const arguments = e.at(c.arguments);
        if (arguments.size() != 2)
            return type_error("a sampler's statement without its two samplers");
        auto binding = symbol_id::none;
        auto member = i32(-1);
        auto element = i32(0);
        if (auto const f = resource_of(arguments[0], binding, member, element); !f.is_normal())
            return f;
        auto const members = m.at(m.bindings[m.at(binding).info].members);
        if (member < 0 || member >= members.size())
            return type_error("a sampler member its binding does not have");
        if (members[member].static_sampler >= 0)
            return fail(run_status::program_error,
                        cc::format("'{}' is a static sampler, whose state the source sets", members[member].name));
        auto state = sampler_state();
        if (auto const f = sampler_of(arguments[1], state); !f.is_normal())
            return f;
        auto is_found = false;
        for (auto& s : samplers)
            if (s.binding == binding && s.member == member && s.element == element)
            {
                s.state = state;
                is_found = true;
            }
        if (!is_found)
            samplers.push_back({.binding = binding, .member = member, .element = element, .state = state});
        result.type = x.type;
        result.leaves.clear();
        out.trace.push_back(result);
        return {};
    }

    flow call(flat_expr const& x, flat_call const& c, value& result)
    {
        // CHK-427: a body in a target's own text runs where that target does, never here
        if (c.text >= 0)
            return type_error("a body in a target's own text, which only its target runs");
        if (auto const* const record = m.builtin_function(c.intrinsic); record != nullptr && record->is_atomic)
            return atomic_call(x, c, *record, result);
        if (auto const* const record = m.builtin_function(c.intrinsic); record != nullptr && record->bytes_words > 0)
            return bytes_call(x, c, *record, result);
        if (auto const* const record = m.builtin_function(c.intrinsic); record != nullptr && record->is_texture_call)
            return texture_call(x, c, *record, result);
        if (auto const* const record = m.builtin_function(c.intrinsic); record != nullptr)
        {
            // EVAL-140, EVAL-141: what lane the run is in, and what its lanes exchange
            if (record->is_subgroup_operation)
                return subgroup_call(x, c, *record, result);
            if (record->uses_derivatives && lane_count >= 4)
                return derivative_call(x, c, *record, result);
            if (record->is_test_only && record->name == "test_state")
                return state_sampler(x, c, result);
            if (record->is_test_only && (record->name == "test_lane" || record->name == "test_lane_count"))
            {
                result.type = x.type;
                result.leaves.clear();
                result.leaves.push_back(scalar::of(record->name == "test_lane" ? lane : lane_count));
                out.trace.push_back(result);
                return {};
            }
        }
        auto args = cc::vector<value>();
        if (auto const f = eval_all(c.arguments, args); !f.is_normal())
            return f;

        result.type = x.type;
        result.leaves.clear();
        auto const* const record = m.builtin_function(c.intrinsic);
        if (record == nullptr || record->evaluate == nullptr)
            return type_error("a call of something that is no builtin function");
        // EVAL-95: the emulated trace reads the pool and the roots of the run's inputs, which no evaluator holds
        if (record->takes_acceleration_index || record->name == "acceleration_pool_load")
            return acceleration_read(*record, args, result);

        // Every argument is the scalars its parameter type says, which is all an evaluator relies on.
        auto in = cc::vector<scalar>();
        auto is_typed = args.size() == record->parameters.size();
        for (auto k = isize(0); is_typed && k < args.size(); ++k)
        {
            // A resource parameter names no builtin type, and a resource is a value of no scalars.
            auto const type = m.builtins->find_type(record->parameters[k]);
            auto const leaf_count = is_valid(type) ? m.builtins->at(type).leaf_count : 0;
            is_typed = args[k].leaves.size() == leaf_count;
            for (auto const& leaf : args[k].leaves)
                is_typed = is_typed && leaf.kind == m.builtins->at(type).leaf_kind;
            in.push_back_range(args[k].leaves);
        }
        // An evaluator computes at 32 bits, and a 16-bit result is rounded once from what it gives.
        for (auto& leaf : in)
            leaf = leaf.widened();
        if (is_typed && record->undefined_when != nullptr)
            if (auto const why = record->undefined_when(in); !why.empty())
                return fail(run_status::program_error, cc::format("{}, in a call of '{}'", why, record->name));
        if (is_typed)
            record->evaluate(in, result.leaves);

        // a prelude struct's leaves are typed by its fields, which `leaf_count_of` below counts
        auto is_result_typed = is_valid(record->result) || result.leaves.empty() || record->gives_prelude_struct;
        if (is_valid(record->result))
        {
            auto const& returned = m.builtins->at(record->result);
            is_result_typed = result.leaves.size() == returned.leaf_count;
            for (auto& leaf : result.leaves)
            {
                leaf = leaf.narrowed_to(returned.leaf_kind);
                is_result_typed = is_result_typed && leaf.kind == returned.leaf_kind;
            }
        }
        if (!is_typed || !is_result_typed || result.leaves.size() != leaf_count_of(m, x.type))
            return type_error(cc::format("a call of '{}' with arguments or a result of the wrong type", record->name));
        // The one way to see WHEN a call with an effect ran: its value joins the trace where the call happened.
        if (!c.is_pure)
            out.trace.push_back(result);
        return {};
    }

    /// `acceleration_root(world, k)`, whose `k` flatten appended, or `acceleration_pool_load(unit)`.
    flow acceleration_read(builtins::function_record const& record, cc::span<value const> args, value& result)
    {
        auto const is_root = record.takes_acceleration_index;
        auto const* const last = args.empty() ? nullptr : &args[args.size() - 1];
        auto const kind = is_root ? value_kind::scalar_int : value_kind::scalar_uint;
        if (last == nullptr || last->leaves.size() != 1 || last->leaves[0].kind != kind)
            return type_error(cc::format("a call of '{}' with arguments of the wrong type", record.name));
        if (is_root)
        {
            auto const k = isize(last->leaves[0].as_int());
            result.leaves.push_back(
                scalar::of_uint(k >= 0 && k < inputs.acceleration_roots.size() ? inputs.acceleration_roots[k] : 0));
            return {};
        }

        auto const unit = isize(last->leaves[0].as_uint());
        auto const& pool = inputs.acceleration_pool;
        if (!pool.empty() && (unit + 1) * 16 > pool.size())
            return fail(
                run_status::program_error,
                cc::format("the unit {} is past the end of an acceleration pool of {} units", unit, pool.size() / 16));
        for (auto i = 0; i < 4; ++i)
        {
            auto word = u32(0);
            if (!pool.empty())
                for (auto b = 0; b < 4; ++b)
                    word |= u32(pool[unit * 16 + i * 4 + b]) << (8 * b);
            result.leaves.push_back(scalar::of_uint(word));
        }
        return {};
    }

    /// The element `b` names: evaluates the index, then finds the buffer in `out.buffers` and the element's first scalar.
    /// A position rather than a pointer, since whatever runs next may be the value of a store.
    flow locate(flat_buffer_element const& b, type_id element_type, isize& buffer, isize& offset)
    {
        auto index = value();
        if (auto const f = eval(b.index, index); !f.is_normal())
            return f;
        if (!is_index(index))
            return type_error("a buffer index that is no int and no uint");
        if (auto const f = buffer_named(b.buffer, buffer); !f.is_normal())
            return f;

        auto const count = leaf_count_of(m, element_type);
        if (count <= 0 || out.buffers[buffer].leaves.size() % count != 0)
            return type_error(cc::format("a buffer of {} scalars, which holds no whole number of its elements",
                                         out.buffers[buffer].leaves.size()));
        // EVAL-90: no target agrees on an index past the end, so a correct program never has one
        auto const at = index_value(index);
        auto const length = out.buffers[buffer].leaves.size() / count;
        if (at < 0 || at >= length)
            return fail(run_status::program_error,
                        cc::format("the index {} is out of bounds of a buffer of {} elements", at, length));
        offset = at * count;
        return {};
    }

    flow eval_bool(flat_expr_id id, bool& result)
    {
        auto v = value();
        if (auto const f = eval(id, v); !f.is_normal())
            return f;
        if (v.leaves.size() != 1 || v.leaves[0].kind != value_kind::boolean)
            return type_error("a condition or a logical operand that is no bool");
        result = v.leaves[0].as_bool();
        return {};
    }

    flow eval(flat_expr_id id, value& result)
    {
        if (!is_known(e, id))
            return type_error("an expression id that names nothing");
        if (depth > k_max_depth)
            return type_error("an expression nested beyond any program");
        if (!burn())
            return {.kind = flow_kind::failed};
        ++depth;
        auto const f = is_in_workgroup(id) ? read_workgroup(id, result) : eval_node(e.at(id), result);
        --depth;
        return f;
    }

    flow eval_node(flat_expr const& x, value& result)
    {
        result.type = x.type;
        result.leaves.clear();

        if (x.node.is<flat_invalid>())
            return type_error("an unfilled expression");
        // a literal of a 16-bit type is that type's value, whatever text spelled it (CHK-253)
        auto const* const builtin = m.builtin_type_of(x.type);
        auto const kind = builtin != nullptr ? builtin->leaf_kind : value_kind::none;
        if (auto const* const l = x.node.try_as<flat_literal>())
        {
            result.leaves.push_back(kind == value_kind::scalar_half     ? scalar::of_half(l->value)
                                    : kind == value_kind::scalar_double ? scalar::of_double(l->value)
                                                                        : scalar::of(f32(l->value)));
            return {};
        }
        if (auto const* const l = x.node.try_as<flat_wide_literal>())
        {
            result.leaves.push_back(scalar::of_wide(kind, l->bits));
            return {};
        }
        if (auto const* const l = x.node.try_as<flat_int_literal>())
        {
            result.leaves.push_back(
                (l->is_unsigned ? scalar::of_uint(u32(l->value)) : scalar::of(l->value)).narrowed_to(kind));
            return {};
        }
        if (auto const* const l = x.node.try_as<flat_bool_literal>())
        {
            result.leaves.push_back(scalar::of(l->value));
            return {};
        }
        if (auto const* const v = x.node.try_as<flat_enum_value>())
        {
            auto const cases = m.at(m.at(x.type).cases);
            if (v->case_index < 0 || v->case_index >= cases.size())
                return type_error("an enum value whose case its type does not have");
            result.leaves.push_back(scalar::of(cases[v->case_index].value));
            return {};
        }
        if (auto const* const ref = x.node.try_as<flat_local_ref>())
        {
            if (!is_known(e, ref->local))
                return type_error("a local id that names nothing");
            // an undefined() is handed on like any value, and what reads it reads zeroes
            if (!is_set[index_of(ref->local)] && !e.at(ref->local).is_undefined)
                return fail(run_status::uninitialized_read, e.at(ref->local).name);
            result = locals[index_of(ref->local)];
            return {};
        }
        if (auto const* const b = x.node.try_as<flat_binding_member>())
        {
            for (auto k = isize(0); k < e.bindings.size() && k < inputs.bindings.size(); ++k)
            {
                if (e.bindings[k] != b->binding)
                    continue;
                auto const& info = m.bindings[m.at(b->binding).info];
                auto offset = isize(0);
                auto const members = m.at(info.members);
                if (b->member < 0 || b->member >= members.size())
                    break;
                for (auto i = 0; i < b->member; ++i)
                    offset += leaf_count_of(m, members[i].type);
                auto const count = leaf_count_of(m, members[b->member].type);
                if (offset + count > inputs.bindings[k].leaves.size())
                    break;
                result.leaves.push_back_range(
                    cc::span<scalar const>(inputs.bindings[k].leaves).subspan({.offset = offset, .size = count}));
                return {};
            }
            return type_error("a binding member the inputs do not hold");
        }
        // CHK-460: what the inputs bound, in elements, or in bytes of a `bytes`
        if (auto const* const length = x.node.try_as<flat_buffer_length>())
        {
            auto buffer = isize(-1);
            if (auto const f = buffer_named(length->buffer, buffer); !f.is_normal())
                return f;
            auto const& type = m.at(e.at(length->buffer).type);
            auto const& leaves = out.buffers[buffer].leaves;
            auto const n
                = type.kind == type_kind::bytes ? leaves.size() * 4 : leaves.size() / leaf_count_of(m, type.element);
            result.leaves.push_back(scalar::of_uint(u32(n)));
            return {};
        }
        if (auto const* const element = x.node.try_as<flat_buffer_element>())
        {
            auto buffer = isize(-1);
            auto offset = isize(0);
            if (auto const f = locate(*element, x.type, buffer, offset); !f.is_normal())
                return f;
            result.type = x.type;
            result.leaves.clear();
            result.leaves.push_back_range(cc::span<scalar const>(out.buffers[buffer].leaves)
                                              .subspan({.offset = offset, .size = leaf_count_of(m, x.type)}));
            return {};
        }
        if (auto const* const element = x.node.try_as<flat_element>())
        {
            auto object = value();
            if (auto const f = eval(element->object, object); !f.is_normal())
                return f;
            auto index = value();
            if (auto const f = eval(element->index, index); !f.is_normal())
                return f;
            auto offset = isize(0);
            auto count = isize(0);
            if (auto const f = element_offset(e.at(element->object).type, index, offset, count); !f.is_normal())
                return f;
            if (offset + count > object.leaves.size())
                return type_error("a value with fewer scalars than its type");
            result.type = x.type;
            result.leaves.clear();
            result.leaves.push_back_range(
                cc::span<scalar const>(object.leaves).subspan({.offset = offset, .size = count}));
            return {};
        }
        if (auto const* const member = x.node.try_as<flat_member>())
        {
            auto object = value();
            if (auto const f = eval(member->object, object); !f.is_normal())
                return f;
            auto const type = x.type;
            if (!member->is_swizzle())
            {
                if (auto const f = slice_member(object, member->member, result); !f.is_normal())
                    return f;
                result.type = type;
                return {};
            }
            // a swizzle is its fields' values one after another, in the order its letters are written
            auto leaves = cc::vector<scalar>();
            for (auto i = 0; i < member->letters.count; ++i)
            {
                auto field = value();
                if (auto const f = slice_member(object, member->letters.fields[i], field); !f.is_normal())
                    return f;
                leaves.push_back_range(field.leaves);
            }
            result.type = type;
            result.leaves = cc::move(leaves);
            return {};
        }
        if (auto const* const construct = x.node.try_as<flat_construct>())
        {
            auto args = cc::vector<value>();
            if (auto const f = eval_all(construct->arguments, args); !f.is_normal())
                return f;
            for (auto const& a : args)
                result.leaves.push_back_range(a.leaves);
            if (result.leaves.size() != leaf_count_of(m, x.type))
                return type_error(
                    cc::format("a construction of '{}' from the wrong number of scalars", m.name_of(x.type)));
            return {};
        }
        if (auto const* const c = x.node.try_as<flat_call>())
            return call(x, *c, result);
        // the interpreter has no device, so it runs what a device without the construct would: the emulated form
        if (auto const* const both = x.node.try_as<flat_by_target>())
            return eval(both->emulated, result);
        if (auto const* const n = x.node.try_as<flat_not>())
        {
            auto operand = false;
            if (auto const f = eval_bool(n->operand, operand); !f.is_normal())
                return f;
            result.leaves.push_back(scalar::of(!operand));
            return {};
        }
        auto const* const a = x.node.try_as<flat_and>();
        auto const* const o = x.node.try_as<flat_or>();
        if (a != nullptr || o != nullptr)
        {
            auto lhs = false;
            if (auto const f = eval_bool(a != nullptr ? a->lhs : o->lhs, lhs); !f.is_normal())
                return f;
            auto rhs = lhs;
            // the right operand runs only when the left one has not decided the value
            if (lhs == (a != nullptr))
                if (auto const f = eval_bool(a != nullptr ? a->rhs : o->rhs, rhs); !f.is_normal())
                    return f;
            result.leaves.push_back(scalar::of(rhs));
            return {};
        }
        if (auto const* const block = x.node.try_as<flat_block>())
        {
            auto const f = run_body(block->body);
            // A void block has its one value however it ends, which is no scalars at all.
            auto const is_void = leaf_count_of(m, x.type) == 0;
            if (f.kind == flow_kind::leave && f.label == block->label)
            {
                if (carried.leaves.empty() && !is_void)
                    return type_error("a block expression left without a value");
                result.leaves = cc::move(carried.leaves);
                carried = {};
                return {};
            }
            if (f.is_normal())
                return is_void ? flow{} : fail(run_status::fell_off_the_end, "a block expression");
            return f;
        }
        return type_error("an expression of a kind the machine does not know");
    }

    // ---- statements -------------------------------------------------------------------------------------------------

    flow store(local_id local, value v)
    {
        if (!is_known(e, local))
            return type_error("a local id that names nothing");
        v.type = e.at(local).type;
        locals[index_of(local)] = cc::move(v);
        is_set[index_of(local)] = true;
        return {};
    }

    /// Where a place lies in its local: evaluates its array indices, the one nearest the local first (EVAL-14).
    /// The cell of workgroup memory `b` names, which starts as the run's first touch of it.
    isize cell_of(flat_binding_member const& b)
    {
        for (auto i = isize(0); i < workgroup.size(); ++i)
            if (workgroup[i].binding == b.binding && workgroup[i].member == b.member)
                return i;
        auto const type = m.at(m.bindings[m.at(b.binding).info].members)[b.member].type;
        auto cell = workgroup_cell{.binding = b.binding, .member = b.member, .memory = zero_value(m, type)};
        cell.is_written.resize_to_filled(cell.memory.leaves.size(), false);
        workgroup.push_back(cc::move(cell));
        return workgroup.size() - 1;
    }

    /// True for a member or an element of workgroup memory, at any depth.
    [[nodiscard]] bool is_in_workgroup(flat_expr_id id) const
    {
        for (auto i = 0; i < k_max_depth && is_known(e, id); ++i)
        {
            auto const& node = e.at(id).node;
            if (auto const* const b = node.try_as<flat_binding_member>())
                return b->is_workgroup;
            if (auto const* const element = node.try_as<flat_element>())
                id = element->object;
            else if (auto const* const member = node.try_as<flat_member>(); member != nullptr && !member->is_swizzle())
                id = member->object;
            else
                return false;
        }
        return false;
    }

    /// A read of workgroup memory, of what the run stored there and nothing else (EVAL-92).
    flow read_workgroup(flat_expr_id id, value& result)
    {
        auto where = place_ref();
        auto offset = isize(0);
        auto type = type_id::none;
        if (auto const f = locate_place(id, where, offset, type); !f.is_normal())
            return f;
        auto const& cell = workgroup[where.cell];
        auto const count = leaf_count_of(m, type);
        for (auto i = isize(0); i < count; ++i)
            if (!cell.is_written[offset + i])
                return fail(run_status::program_error,
                            cc::format("a read of {}.{} where nothing was stored", m.at(cell.binding).name,
                                       m.at(m.bindings[m.at(cell.binding).info].members)[cell.member].name));
        result.type = type;
        result.leaves.clear();
        result.leaves.push_back_range(
            cc::span<scalar const>(cell.memory.leaves).subspan({.offset = offset, .size = count}));
        return {};
    }

    /// What a place is part of: a mutable local, or a cell of workgroup memory.
    struct place_ref
    {
        local_id local = local_id::none;
        isize cell = -1;
    };

    flow locate_place(flat_expr_id place, place_ref& where, isize& offset, type_id& type)
    {
        // the steps from the place down to its local, innermost first: a member, or an element's index expression
        struct step
        {
            i32 member = -1;
            flat_expr_id index = flat_expr_id::none;
        };
        auto path = cc::vector<step>();
        auto id = place;
        where = {};
        for (auto i = 0; i < k_max_depth && is_known(e, id); ++i)
        {
            auto const& x = e.at(id);
            if (auto const* const ref = x.node.try_as<flat_local_ref>())
            {
                where.local = ref->local;
                break;
            }
            if (auto const* const b = x.node.try_as<flat_binding_member>(); b != nullptr && b->is_workgroup)
            {
                where.cell = cell_of(*b);
                break;
            }
            if (auto const* const element = x.node.try_as<flat_element>())
            {
                path.push_back({.index = element->index});
                id = element->object;
                continue;
            }
            auto const* const member = x.node.try_as<flat_member>();
            if (member == nullptr)
                break;
            path.push_back({.member = member->member});
            id = member->object;
        }
        if (where.cell < 0 && (!is_known(e, where.local) || !e.at(where.local).is_mut))
            return type_error("an assignment to what is no mutable local");

        type = where.cell >= 0 ? workgroup[where.cell].memory.type : e.at(where.local).type;
        offset = 0;
        for (auto k = path.size() - 1; k >= 0; --k)
        {
            if (is_valid(path[k].index))
            {
                auto index = value();
                if (auto const f = eval(path[k].index, index); !f.is_normal())
                    return f;
                auto at = isize(0);
                auto count = isize(0);
                if (auto const f = element_offset(type, index, at, count); !f.is_normal())
                    return f;
                offset += at;
                // a vector's lane is of its fields' type, and an array's element of its element type
                type = m.at(type).kind == type_kind::array ? m.at(type).element : m.at(m.at(type).members)[0].type;
                continue;
            }
            auto const members = members_of(m, type);
            if (path[k].member < 0 || path[k].member >= members.size())
                return type_error("an assignment to a member its type does not have");
            for (auto i = 0; i < path[k].member; ++i)
                offset += leaf_count_of(m, members[i].type);
            type = members[path[k].member].type;
        }
        return {};
    }

    flow assign(flat_expr_id place, value const& v)
    {
        auto where = place_ref();
        auto offset = isize(0);
        auto type = type_id::none;
        if (auto const f = locate_place(place, where, offset, type); !f.is_normal())
            return f;
        return write(where, offset, type, v);
    }

    flow write(place_ref const& where, isize offset, type_id type, value const& v)
    {
        auto& target = where.cell >= 0 ? workgroup[where.cell].memory : locals[index_of(where.local)];
        auto const count = leaf_count_of(m, type);
        if (v.leaves.size() != count || offset + count > target.leaves.size())
            return type_error("an assignment of a value of the wrong size");
        for (auto i = isize(0); i < count; ++i)
            target.leaves[offset + i] = v.leaves[i];
        if (where.cell >= 0)
            for (auto i = isize(0); i < count; ++i)
                workgroup[where.cell].is_written[offset + i] = true;
        else
            is_set[index_of(where.local)] = true;
        return {};
    }

    /// CHK-352: the operand's indices first, then the value, then one store per letter.
    flow assign_swizzle(flat_member const& place, flat_expr_id stored)
    {
        if (!is_known(e, place.object))
            return type_error("an expression id that names nothing");
        auto const object_type = e.at(place.object).type;
        auto const members = members_of(m, object_type);
        auto field_offset = [&](i32 field)
        {
            auto offset = isize(0);
            for (auto i = 0; i < field; ++i)
                offset += leaf_count_of(m, members[i].type);
            return offset;
        };
        for (auto i = 0; i < place.letters.count; ++i)
            if (place.letters.fields[i] < 0 || place.letters.fields[i] >= members.size()
                || leaf_count_of(m, members[place.letters.fields[i]].type) != 1)
                return type_error("a swizzle of a field its type does not have");

        if (auto const* const element = e.at(place.object).node.try_as<flat_buffer_element>())
        {
            auto buffer = isize(-1);
            auto offset = isize(0);
            if (auto const f = locate(*element, object_type, buffer, offset); !f.is_normal())
                return f;
            auto v = value();
            if (auto const f = eval(stored, v); !f.is_normal())
                return f;
            if (v.leaves.size() != place.letters.count)
                return type_error("a store of a value of the wrong size");
            for (auto i = 0; i < place.letters.count; ++i)
                out.buffers[buffer].leaves[offset + field_offset(place.letters.fields[i])] = v.leaves[i];
            is_stored[buffer] = true;
            return {};
        }

        auto where = place_ref();
        auto offset = isize(0);
        auto type = type_id::none;
        if (auto const f = locate_place(place.object, where, offset, type); !f.is_normal())
            return f;
        auto v = value();
        if (auto const f = eval(stored, v); !f.is_normal())
            return f;
        if (v.leaves.size() != place.letters.count)
            return type_error("an assignment of a value of the wrong size");
        for (auto i = 0; i < place.letters.count; ++i)
        {
            auto const field = place.letters.fields[i];
            auto component = value();
            component.leaves.push_back(v.leaves[i]);
            if (auto const f = write(where, offset + field_offset(field), members[field].type, component); !f.is_normal())
                return f;
        }
        return {};
    }

    /// EVAL-14: the index first, then the value, then the store.
    flow store_element(flat_buffer_element const& element, type_id element_type, flat_expr_id stored)
    {
        auto buffer = isize(-1);
        auto offset = isize(0);
        if (auto const f = locate(element, element_type, buffer, offset); !f.is_normal())
            return f;
        auto v = value();
        if (auto const f = eval(stored, v); !f.is_normal())
            return f;
        auto const count = leaf_count_of(m, element_type);
        if (v.leaves.size() != count)
            return type_error("a store of a value of the wrong size");
        for (auto i = isize(0); i < count; ++i)
            out.buffers[buffer].leaves[offset + i] = v.leaves[i];
        is_stored[buffer] = true;
        return {};
    }

    flow run_body(ast::range_of<flat_stmt_id> range)
    {
        if (!is_known(e, range))
            return type_error("a statement list that reaches outside the tree");
        if (depth > k_max_depth)
            return type_error("a statement nested beyond any program");
        ++depth;
        auto result = flow();
        for (auto const id : e.at(range))
        {
            result = run(id);
            if (!result.is_normal())
                break;
        }
        --depth;
        return result;
    }

    /// EVAL-65 to EVAL-70: the scrutinee once, then the arms in order, and the patterns only where they are reached.
    /// `captures_break` is the `switch`, which a `break` directly inside ends the way one inside a `once` ends that.
    flow run_arms(flat_expr_id scrutinee,
                  ast::range_of<flat_arm> arms,
                  ast::range_of<flat_stmt_id> default_body,
                  bool captures_break)
    {
        auto chosen = value();
        if (auto const f = eval(scrutinee, chosen); !f.is_normal())
            return f;
        if (!is_known(e, arms))
            return type_error("a case whose arms reach outside the tree");

        auto selected = default_body;
        auto is_selected = false;
        for (auto const& arm : e.at(arms))
        {
            if (is_selected)
                break;
            if (!is_known(e, arm.patterns))
                return type_error("a case arm whose patterns reach outside the tree");
            for (auto const id : e.at(arm.patterns))
            {
                auto pattern = value();
                if (auto const f = eval(id, pattern); !f.is_normal())
                    return f;
                if (pattern.leaves.size() != chosen.leaves.size())
                    return type_error("a case pattern of another type than its scrutinee");
                auto is_equal = true;
                for (auto i = isize(0); i < pattern.leaves.size(); ++i)
                    is_equal = is_equal && pattern.leaves[i] == chosen.leaves[i];
                if (is_equal)
                {
                    selected = arm.body;
                    is_selected = true;
                    break;
                }
            }
        }

        auto const f = run_body(selected);
        return captures_break && f.kind == flow_kind::break_ ? flow() : f;
    }

    /// What one iteration's end means for its loop: go on, stop, or hand the exit further out.
    enum class after : u8
    {
        next,
        stop,
        propagate,
    };

    after after_iteration(flow const& f, label_id label) const
    {
        if (f.is_normal() || (f.kind == flow_kind::continue_ && f.label == label))
            return after::next;
        if (f.kind == flow_kind::break_ || (f.kind == flow_kind::leave && f.label == label))
            return after::stop;
        return after::propagate;
    }

    /// EVAL-75: the body leaves every node's value in its `var`, and a false condition is recorded with all of them.
    flow check(flat_check const& k)
    {
        if (!limits.run_checks)
            return {};
        if (k.site < 0 || k.site >= e.check_sites.size())
            return type_error("a check whose site the tree does not have");
        if (auto const f = run_body(k.body); !f.is_normal())
            return f;

        auto const& site = e.check_sites[k.site];
        auto const nodes = e.at(site.nodes);
        if (nodes.empty() || !is_known(e, nodes[0].value) || !is_set[index_of(nodes[0].value)])
            return type_error("a check whose condition has no value");
        auto const& condition = locals[index_of(nodes[0].value)];
        if (condition.leaves.size() != 1 || condition.leaves[0].kind != value_kind::boolean)
            return type_error("a check whose condition is no bool");
        ++out.checks_run;
        if (site.stops)
            ++out.asserts_run;
        auto& tally = out.sites[k.site];
        if (condition.leaves[0].as_bool())
        {
            ++tally.passed;
            return {};
        }
        ++tally.failed;

        if (out.failures.size() < limits.max_failures)
        {
            auto failure = check_failure{.site = k.site};
            for (auto const& node : nodes)
            {
                auto const known = is_known(e, node.value) && is_set[index_of(node.value)];
                failure.values.push_back(known ? locals[index_of(node.value)] : value{.type = e.at(node.value).type});
                failure.is_evaluated.push_back(known);
            }
            if (is_known(e, site.loop_variables))
                for (auto const id : e.at(site.loop_variables))
                {
                    auto v = value();
                    if (auto const f = eval(id, v); !f.is_normal())
                        return f;
                    failure.loop_values.push_back(cc::move(v));
                }
            if (is_known(e, site.message_values))
                for (auto const id : e.at(site.message_values))
                {
                    auto v = value();
                    if (auto const f = eval(id, v); !f.is_normal())
                        return f;
                    failure.message_values.push_back(cc::move(v));
                }
            out.failures.push_back(cc::move(failure));
        }
        else
            ++out.failures_dropped;
        return site.stops ? fail(run_status::assertion_failed, "an assert was false") : flow();
    }

    flow run(flat_stmt_id id)
    {
        if (!is_known(e, id))
            return type_error("a statement id that names nothing");
        if (!burn())
            return {.kind = flow_kind::failed};
        auto const& s = e.at(id);

        if (auto const* const let = s.node.try_as<flat_let>())
        {
            auto v = value();
            if (auto const f = eval(let->value, v); !f.is_normal())
                return f;
            return store(let->local, cc::move(v));
        }
        if (auto const* const var = s.node.try_as<flat_var>())
        {
            if (!is_known(e, var->local))
                return type_error("a local id that names nothing");
            if (!is_valid(var->value))
            {
                locals[index_of(var->local)] = zero_value(m, e.at(var->local).type);
                is_set[index_of(var->local)] = e.at(var->local).is_zeroed;
                return {};
            }
            auto v = value();
            if (auto const f = eval(var->value, v); !f.is_normal())
                return f;
            return store(var->local, cc::move(v));
        }
        if (auto const* const a = s.node.try_as<flat_assign>())
        {
            if (auto const* const member = is_known(e, a->place) ? e.at(a->place).node.try_as<flat_member>() : nullptr;
                member != nullptr && member->is_swizzle())
                return assign_swizzle(*member, a->value);
            auto const* const element
                = is_known(e, a->place) ? e.at(a->place).node.try_as<flat_buffer_element>() : nullptr;
            if (element != nullptr)
                return store_element(*element, e.at(a->place).type, a->value);
            // EVAL-14: the place's indices, then the value
            auto where = place_ref();
            auto offset = isize(0);
            auto type = type_id::none;
            if (auto const f = locate_place(a->place, where, offset, type); !f.is_normal())
                return f;
            auto v = value();
            if (auto const f = eval(a->value, v); !f.is_normal())
                return f;
            return write(where, offset, type, v);
        }
        if (auto const* const p = s.node.try_as<flat_print>())
        {
            auto printed = printed_message{.message = p->message};
            for (auto const id : e.at(p->values))
            {
                auto v = value();
                if (auto const f = eval(id, v); !f.is_normal())
                    return f;
                out.trace.push_back(v);
                printed.values.push_back(cc::move(v));
            }
            out.printed.push_back(cc::move(printed));
            return {};
        }
        if (auto const* const dropped = s.node.try_as<flat_eval>())
        {
            auto v = value();
            return eval(dropped->value, v);
        }
        if (auto const* const branch = s.node.try_as<flat_if>())
        {
            auto condition = false;
            if (auto const f = eval_bool(branch->condition, condition); !f.is_normal())
                return f;
            return run_body(condition ? branch->then_body : branch->else_body);
        }
        if (auto const* const block = s.node.try_as<flat_block>())
        {
            auto const f = run_body(block->body);
            if (f.kind == flow_kind::leave && f.label == block->label)
            {
                carried = {};
                return {};
            }
            return f;
        }
        if (auto const* const leave = s.node.try_as<flat_leave>())
        {
            auto v = value();
            if (is_valid(leave->value))
                if (auto const f = eval(leave->value, v); !f.is_normal())
                    return f;
            carried = cc::move(v);
            return {.kind = flow_kind::leave, .label = leave->target};
        }
        if (auto const* const loop = s.node.try_as<flat_loop>())
        {
            while (burn())
            {
                auto const f = run_body(loop->body);
                auto const next = after_iteration(f, loop->label);
                if (next == after::stop)
                    return {};
                if (next == after::propagate)
                    return f;
            }
            return {.kind = flow_kind::failed};
        }
        if (auto const* const w = s.node.try_as<flat_while>())
        {
            while (burn())
            {
                auto condition = false;
                if (auto const f = eval_bool(w->condition, condition); !f.is_normal())
                    return f;
                if (!condition)
                    return {};
                auto const f = run_body(w->body);
                auto const next = after_iteration(f, w->label);
                if (next == after::stop)
                    return {};
                if (next == after::propagate)
                    return f;
            }
            return {.kind = flow_kind::failed};
        }
        if (auto const* const loop = s.node.try_as<flat_for>())
        {
            auto first = value();
            auto end = value();
            if (auto const f = eval(loop->first, first); !f.is_normal())
                return f;
            if (auto const f = eval(loop->end, end); !f.is_normal())
                return f;
            auto const is_int
                = [](value const& v) { return v.leaves.size() == 1 && v.leaves[0].kind == value_kind::scalar_int; };
            if (!is_int(first) || !is_int(end))
                return type_error("a `for` whose bounds are no int");
            for (auto index = first.leaves[0].as_int(); index < end.leaves[0].as_int(); ++index)
            {
                if (!burn())
                    return {.kind = flow_kind::failed};
                auto v = value();
                v.leaves.push_back(scalar::of(index));
                if (auto const f = store(loop->index, cc::move(v)); !f.is_normal())
                    return f;
                auto const f = run_body(loop->body);
                auto const next = after_iteration(f, loop->label);
                if (next == after::stop)
                    return {};
                if (next == after::propagate)
                    return f;
            }
            return {};
        }
        if (auto const* const c = s.node.try_as<flat_continue>())
            return {.kind = flow_kind::continue_, .label = c->target};
        if (s.node.is<flat_discard>())
            return fail(run_status::discarded, "");
        if (auto const* const once = s.node.try_as<flat_once>())
        {
            auto const f = run_body(once->body);
            return f.kind == flow_kind::break_ ? flow() : f;
        }
        if (s.node.is<flat_break>())
            return {.kind = flow_kind::break_};
        if (auto const* const c = s.node.try_as<flat_case>())
            return run_arms(c->scrutinee, c->arms, c->default_body, false);
        if (auto const* const sw = s.node.try_as<flat_switch>())
            return run_arms(sw->scrutinee, sw->arms, sw->default_body, true);
        if (auto const* const r = s.node.try_as<flat_return>())
        {
            auto v = value();
            if (is_valid(r->value))
                if (auto const f = eval(r->value, v); !f.is_normal())
                    return f;
            carried = cc::move(v);
            return {.kind = flow_kind::return_};
        }
        if (auto const* const k = s.node.try_as<flat_check>())
            return check(*k);
        return type_error("a statement of a kind the machine does not know");
    }
};
} // namespace

scalar sgl::check::scalar::of(f32 v)
{
    return {.kind = value_kind::scalar_float, .bits = cc::bit_cast<u32>(v)};
}

scalar sgl::check::scalar::of(i32 v)
{
    return {.kind = value_kind::scalar_int, .bits = u32(v)};
}

scalar sgl::check::scalar::of(bool v)
{
    return {.kind = value_kind::boolean, .bits = v ? 1u : 0u};
}

scalar sgl::check::scalar::of_half(f64 v)
{
    return {.kind = value_kind::scalar_half, .bits = builtins::impl::half_bits_of(v)};
}

sgl::f32 sgl::check::scalar::as_float() const
{
    return cc::bit_cast<f32>(bits);
}

scalar sgl::check::scalar::of_double(f64 v)
{
    return of_wide(value_kind::scalar_double, cc::bit_cast<u64>(v));
}

sgl::f64 sgl::check::scalar::as_double() const
{
    return cc::bit_cast<f64>(wide_bits());
}

scalar sgl::check::scalar::widened() const
{
    switch (kind)
    {
    case value_kind::scalar_half:
        return of(builtins::impl::float_of_half_bits(bits));
    case value_kind::scalar_short:
        return of(i32(i16(u16(bits))));
    case value_kind::scalar_ushort:
        return of_uint(bits & 0xffffu);
    default:
        return *this;
    }
}

scalar sgl::check::scalar::narrowed_to(value_kind to) const
{
    if (!is_16_bit(to) || wide_kind_of(to) != wide_kind_of(kind) || is_16_bit(kind))
        return *this;
    if (to == value_kind::scalar_half)
        return {.kind = to, .bits = builtins::impl::half_bits_of(as_float())};
    return {.kind = to, .bits = bits & 0xffffu};
}

cc::string_view sgl::check::to_string(run_status s)
{
    switch (s)
    {
    case run_status::ok:
        return "ok";
    case run_status::out_of_fuel:
        return "out-of-fuel";
    case run_status::fell_off_the_end:
        return "fell-off-the-end";
    case run_status::type_error:
        return "type-error";
    case run_status::uninitialized_read:
        return "uninitialized-read";
    case run_status::program_error:
        return "program-error";
    case run_status::discarded:
        return "discarded";
    case run_status::assertion_failed:
        return "assertion-failed";
    case run_status::stopped:
        return "stopped";
    }
    return "";
}

sgl::isize sgl::check::leaf_count_of(checked_module const& m, type_id type)
{
    auto leaves = cc::vector<scalar>();
    append_zero(m, type, leaves, 0);
    return leaves.size();
}

value sgl::check::zero_value(checked_module const& m, type_id type)
{
    auto result = value{.type = type};
    append_zero(m, type, result.leaves, 0);
    return result;
}

namespace
{
/// One lane's run of `e`, starting from `memory` and leaving it to the lane after it (EVAL-142).
outcome run_lane(checked_module const& m,
                 flat_entry_point const& e,
                 run_inputs const& inputs,
                 run_limits const& limits,
                 shared_memory& memory,
                 i32 lane,
                 exchange_table const& previous,
                 exchange_table& current)
{
    auto run = machine{.m = m, .e = e, .inputs = inputs, .limits = limits, .fuel = limits.fuel};
    run.out.buffers = cc::move(memory.buffers);
    run.is_stored = cc::move(memory.is_stored);
    run.textures = cc::move(memory.textures);
    run.is_texture_stored = cc::move(memory.is_texture_stored);
    run.is_stated = cc::move(memory.is_stated);
    run.samplers = cc::move(memory.samplers);
    run.lane = lane;
    run.lane_count = inputs.lane_count < 1 ? 1 : inputs.lane_count;
    run.previous = &previous;
    run.current = &current;
    run.out.sites.resize_to_defaulted(e.check_sites.size());
    run.locals.resize_to_defaulted(e.locals.size());
    run.is_set.resize_to_filled(e.locals.size(), false);
    // A test has no parameter, and its first local is one of its own; an entry point may have stage inputs alone.
    if (is_valid(e.input) && !e.locals.empty() && e.locals[0].kind == local_kind::parameter)
    {
        run.locals[0] = lane < inputs.lane_parameters.size() ? inputs.lane_parameters[lane] : inputs.parameter;
        run.is_set[0] = true;
    }
    // A stage input the caller did not state is zero, which is what one invocation of a draw or a dispatch would read.
    for (auto i = isize(0); i < e.stage_inputs.size(); ++i)
    {
        auto const local = index_of(e.stage_inputs[i].local);
        run.locals[local] = i < inputs.stage_inputs.size() ? inputs.stage_inputs[i] : zero_value(m, e.locals[local].type);
        run.is_set[local] = true;
    }

    auto const f = run.run_body(e.body);
    auto const is_returned
        = f.kind == flow_kind::return_ || (f.kind == flow_kind::leave && is_valid(e.root) && f.label == e.root);
    if (is_returned)
    {
        run.out.result = cc::move(run.carried);
        run.out.result.type = e.result;
    }
    else if (f.is_normal())
    {
        // A test and a compute entry point return void, whose one value a run has however it ends.
        if (e.result != checked_module::void_type)
            run.fail(run_status::fell_off_the_end, "the function");
    }
    else if (f.kind != flow_kind::failed)
        run.type_error("an exit that nothing encloses");

    memory.buffers = cc::move(run.out.buffers);
    memory.is_stored = cc::move(run.is_stored);
    memory.textures = cc::move(run.textures);
    memory.is_texture_stored = cc::move(run.is_texture_stored);
    memory.is_stated = cc::move(run.is_stated);
    memory.samplers = cc::move(run.samplers);
    run.out.buffers.clear();
    return cc::move(run.out);
}

/// The name of the builtin the exchanging call `id` calls, for a report.
cc::string_view exchanged_name(checked_module const& m, flat_entry_point const& e, flat_expr_id id)
{
    if (!is_known(e, id))
        return "?";
    auto const* const c = e.at(id).node.try_as<flat_call>();
    auto const* const record = c != nullptr ? m.builtin_function(c->intrinsic) : nullptr;
    return record != nullptr ? cc::string_view(record->name) : cc::string_view("?");
}

/// The run of every lane as one outcome: the first that did not end normally decides how it ended (EVAL-145).
outcome merged(checked_module const& m,
               flat_entry_point const& e,
               run_limits const& limits,
               cc::vector<outcome> runs,
               shared_memory memory,
               exchange_table const& exchanges,
               bool is_settled)
{
    auto const lanes = i32(runs.size());
    auto out = outcome();
    out.sites.resize_to_defaulted(e.check_sites.size());
    for (auto lane = i32(0); lane < lanes; ++lane)
    {
        auto& r = runs[lane];
        if (out.status == run_status::ok && r.status != run_status::ok)
        {
            out.status = r.status;
            out.detail = lanes > 1 ? cc::format("in lane {} of {}: {}", lane, lanes, r.detail) : cc::move(r.detail);
        }
        if (lane == 0)
            out.result = r.result;
        out.lane_results.push_back(cc::move(r.result));
        out.trace.push_back_range(r.trace);
        out.printed.push_back_range(r.printed);
        for (auto& f : r.failures)
        {
            f.lane = lane;
            if (out.failures.size() < limits.max_failures)
                out.failures.push_back(cc::move(f));
            else
                ++out.failures_dropped;
        }
        out.failures_dropped += r.failures_dropped;
        out.checks_run += r.checks_run;
        out.asserts_run += r.asserts_run;
        for (auto s = isize(0); s < out.sites.size() && s < r.sites.size(); ++s)
        {
            out.sites[s].passed += r.sites[s].passed;
            out.sites[s].failed += r.sites[s].failed;
        }
    }
    // CHK-377 puts a subgroup operation where every lane arrives together, so one a lane missed is a program's error
    if (out.status == run_status::ok)
        for (auto const& x : exchanges.entries)
            for (auto lane = i32(0); lane < x.has.size(); ++lane)
                if (!x.has[lane] && out.status == run_status::ok)
                {
                    out.status = run_status::program_error;
                    out.detail = cc::format("lane {} of {} did not reach {} with the others, which every lane reaches "
                                            "together or none does",
                                            lane, lanes, exchanged_name(m, e, x.call));
                }
    if (out.status == run_status::ok && !is_settled)
    {
        out.status = run_status::program_error;
        out.detail = "what the lanes exchange never settled";
    }
    for (auto i = isize(0); i < memory.buffers.size(); ++i)
        if (memory.is_stored[i])
            out.buffers.push_back(cc::move(memory.buffers[i]));
    for (auto i = isize(0); i < memory.textures.size(); ++i)
        if (memory.is_texture_stored[i])
            out.images.push_back(cc::move(memory.textures[i]));
    return out;
}
} // namespace

outcome sgl::check::interpret(checked_module const& m,
                              flat_entry_point const& e,
                              run_inputs const& inputs,
                              run_limits const& limits)
{
    // EVAL-143: each round runs every lane with what the round before exchanged, until nothing a lane gives changes
    auto const lanes = inputs.lane_count < 1 ? 1 : inputs.lane_count;
    auto previous = exchange_table();
    for (auto round = isize(0);; ++round)
    {
        auto current = exchange_table();
        auto memory = shared_memory{
            .buffers = inputs.buffers,
            .is_stored = cc::vector<bool>::create_filled(inputs.buffers.size(), false),
            .textures = inputs.textures,
            .is_texture_stored = cc::vector<bool>::create_filled(inputs.textures.size(), false),
            .is_stated = cc::vector<bool>::create_filled(inputs.textures.size(), false),
            .samplers = inputs.samplers,
        };
        auto runs = cc::vector<outcome>();
        auto is_stopped = false;
        for (auto lane = i32(0); lane < lanes && !is_stopped; ++lane)
        {
            runs.push_back(run_lane(m, e, inputs, limits, memory, lane, previous, current));
            is_stopped = runs.back().status == run_status::stopped;
        }
        if (is_stopped)
            return cc::move(runs.back());
        // every round settles one exchange more than the one before, in the order the lanes reach them
        auto const is_settled = current.is_same_as(previous);
        if (is_settled || round > current.entries.size() + 1)
            return merged(m, e, limits, cc::move(runs), cc::move(memory), current, is_settled);
        previous = cc::move(current);
    }
}
namespace
{
/// The bytes `member_data` gives a leaf of `kind`: a word, and two for a 64-bit leaf, its low word first.
isize leaf_bytes(value_kind kind)
{
    return is_64_bit(kind) ? 8 : 4;
}

/// The bytes `member_data` gives `leaves`.
isize bytes_of(cc::span<scalar const> leaves)
{
    auto size = isize(0);
    for (auto const& leaf : leaves)
        size += leaf_bytes(leaf.kind);
    return size;
}

/// Fills the scalars of `leaves`, whose kinds they keep, from `bytes` as `member_data` lays them out.
void read_leaves(cc::span<byte const> bytes, cc::span<scalar> leaves)
{
    auto at = isize(0);
    auto const word_at = [&]
    {
        auto word = u32(0);
        for (auto b = 0; b < 4; ++b)
            word |= u32(bytes[at + b]) << (8 * b);
        at += 4;
        return word;
    };
    for (auto& leaf : leaves)
    {
        auto const word = word_at();
        leaf.bits = leaf.kind == value_kind::boolean ? u32(word != 0) : is_16_bit(leaf.kind) ? word & 0xffffu : word;
        if (is_64_bit(leaf.kind))
            leaf.high = word_at();
    }
}

/// The contents of one texture or image of type `t` from what a driver binds to it (EVAL-120).
cc::result<texture_contents> texture_of(checked_module const& m, check::type_info const& t, texture_data const& d)
{
    auto contents = texture_contents{.width = d.width,
                                     .height = d.height,
                                     .depth = d.depth,
                                     .layers = d.layers,
                                     .levels = d.levels,
                                     .samples = d.samples};
    // a texel as the shader reads it: a depth texture's one float, an image's format's channels, a texture's element
    auto texel = cc::vector<scalar>();
    if (t.kind == type_kind::image && t.format >= 0)
    {
        auto const& format = k_image_formats[t.format];
        for (auto c = 0; c < format.channels; ++c)
            texel.push_back({.kind = format.component});
    }
    else if (t.is_depth)
        texel.push_back(scalar::of(0.0f));
    else
        texel = zero_value(m, t.element).leaves;
    contents.channels = i32(texel.size());
    auto const axes = builtins::texture::axes_of(t.shape);
    if (d.width < 1 || d.height < 1 || d.depth < 1 || d.layers < 1 || d.levels < 1 || d.samples < 1)
        return cc::error(cc::format("a texture of {} by {} by {}, {} layers, {} levels and {} samples, which holds no "
                                    "texel",
                                    d.width, d.height, d.depth, d.layers, d.levels, d.samples));
    if (axes.is_cube && d.layers % 6 != 0)
        return cc::error(cc::format("a cube of {} faces, which is no whole number of cubes", d.layers));
    auto texels = isize(0);
    for (auto l = 0; l < d.levels; ++l)
    {
        auto const extent = [&](i32 n, bool has) { return !has ? 1 : (n >> l) < 1 ? 1 : n >> l; };
        texels += isize(extent(d.width, true)) * extent(d.height, axes.dim >= 2)
                * extent(d.depth, axes.dim == 3 && !axes.is_cube) * d.layers * d.samples;
    }
    auto const bytes = !d.mutable_texels.empty() ? cc::span<byte const>(d.mutable_texels.span()) : d.texels.span();
    if (bytes.size() != texels * contents.channels * 4)
        return cc::error(cc::format("{} bytes of texels, where {} texels of {} channels take {}", bytes.size(), texels,
                                    contents.channels, texels * contents.channels * 4));
    for (auto k = isize(0); k < texels; ++k)
        contents.texels.push_back_range(texel);
    read_leaves(bytes, contents.texels);
    return contents;
}

member_data const* find_member(driver_bindings const& bound, cc::string_view binding, cc::string_view member)
{
    for (auto const& g : bound.groups)
        if (g.name == binding)
            for (auto const& d : g.members)
                if (d.name == member)
                    return &d;
    return nullptr;
}
} // namespace

cc::result<run_inputs> sgl::check::resolve_inputs(checked_module const& m,
                                                  flat_entry_point const& e,
                                                  driver_bindings const& bound)
{
    auto inputs = run_inputs{.acceleration_pool = bound.acceleration_pool};
    for (auto const binding : e.bindings)
    {
        auto const& name = m.at(binding).name;
        auto const members = m.at(m.bindings[m.at(binding).info].members);
        for (auto const& g : bound.groups)
            if (g.name == name)
                for (auto const& d : g.members)
                {
                    auto is_known = false;
                    for (auto const& member : members)
                        is_known = is_known || member.name == d.name;
                    if (!is_known)
                        return cc::error(cc::format("the binding {} has no member {}", name, d.name));
                }

        auto whole = value();
        for (auto i = isize(0); i < members.size(); ++i)
        {
            auto const& member = members[i];
            auto const* const d = find_member(bound, name, member.name);
            auto const bytes = d == nullptr              ? cc::span<byte const>()
                             : !d->mutable_bytes.empty() ? cc::span<byte const>(d->mutable_bytes.span())
                                                         : d->bytes.span();
            auto const& type = m.at(member.type);
            // EVAL-120: a texture, an image, a sampler, or a binding array of them
            auto const& resource = type.kind == type_kind::array ? m.at(type.element) : type;
            if (resource.kind == type_kind::texture || resource.kind == type_kind::image)
            {
                // an unbounded binding array holds what its group binds (CHK-490)
                auto const count = type.kind == type_kind::array ? type.count : 1;
                if (d != nullptr && count > 0 && d->textures.size() > count)
                    return cc::error(cc::format("{}.{} is bound {} textures, and holds {}", name, member.name,
                                                d->textures.size(), count));
                for (auto k = isize(0); d != nullptr && k < d->textures.size(); ++k)
                {
                    auto made = texture_of(m, resource, d->textures[k]);
                    if (made.has_error())
                        return cc::error(cc::format("{}.{}: {}", name, member.name, made.error().to_string()));
                    auto contents = cc::move(made.value());
                    contents.binding = binding;
                    contents.member = i32(i);
                    contents.element = i32(k);
                    inputs.textures.push_back(cc::move(contents));
                }
                continue;
            }
            if (resource.kind == type_kind::sampler)
            {
                for (auto k = isize(0); d != nullptr && k < d->samplers.size(); ++k)
                    inputs.samplers.push_back(
                        {.binding = binding, .member = i32(i), .element = i32(k), .state = d->samplers[k]});
                continue;
            }
            if (type.kind == type_kind::acceleration_structure)
            {
                inputs.acceleration_roots.push_back(d == nullptr ? 0 : d->acceleration_root);
                continue;
            }
            if (type.kind == type_kind::bytes)
            {
                if (bytes.size() % 4 != 0)
                    return cc::error(cc::format("{}.{} is {} bytes, which is no whole number of 4-byte words", name,
                                                member.name, bytes.size()));
                auto contents = buffer_contents{.binding = binding, .member = i32(i)};
                for (auto k = isize(0); k < bytes.size() / 4; ++k)
                    contents.leaves.push_back(scalar::of_uint(0));
                read_leaves(bytes, contents.leaves);
                inputs.buffers.push_back(cc::move(contents));
                continue;
            }
            if (type.kind == type_kind::buffer)
            {
                auto const element = zero_value(m, type.element);
                auto const stride = bytes_of(element.leaves);
                if (stride == 0 || bytes.size() % stride != 0)
                    return cc::error(cc::format("{}.{} is {} bytes, which is no whole number of {}-byte elements", name,
                                                member.name, bytes.size(), stride));
                auto contents = buffer_contents{.binding = binding, .member = i32(i)};
                for (auto k = isize(0); k < bytes.size() / stride; ++k)
                    contents.leaves.push_back_range(element.leaves);
                read_leaves(bytes, contents.leaves);
                inputs.buffers.push_back(cc::move(contents));
                continue;
            }

            auto v = zero_value(m, member.type);
            if (d != nullptr && bytes.size() != bytes_of(v.leaves))
                return cc::error(cc::format("{}.{} is {} bytes, and its value is {}", name, member.name, bytes.size(),
                                            bytes_of(v.leaves)));
            if (d != nullptr)
                read_leaves(bytes, v.leaves);
            whole.leaves.push_back_range(v.leaves);
        }
        inputs.bindings.push_back(cc::move(whole));
    }
    return inputs;
}

void sgl::check::write_back(checked_module const& m, outcome const& o, driver_bindings const& bound)
{
    for (auto const& b : o.buffers)
    {
        auto const members = m.at(m.bindings[m.at(b.binding).info].members);
        if (b.member < 0 || b.member >= members.size())
            continue;
        auto const* const d = find_member(bound, m.at(b.binding).name, members[b.member].name);
        if (d == nullptr || d->mutable_bytes.empty())
            continue;
        CC_ASSERT(d->mutable_bytes.size() == bytes_of(b.leaves), "a run never changes the size of a buffer");
        auto at = isize(0);
        for (auto const& leaf : b.leaves)
            for (auto k = 0; k < leaf_bytes(leaf.kind); ++k)
                d->mutable_bytes[at++] = byte(u8(leaf.wide_bits() >> (8 * k)));
    }
    for (auto const& image : o.images)
    {
        auto const members = m.at(m.bindings[m.at(image.binding).info].members);
        if (image.member < 0 || image.member >= members.size())
            continue;
        auto const* const d = find_member(bound, m.at(image.binding).name, members[image.member].name);
        if (d == nullptr || image.element < 0 || image.element >= d->textures.size())
            continue;
        auto& texels = d->textures[image.element].mutable_texels;
        if (texels.empty())
            continue;
        CC_ASSERT(texels.size() == image.texels.size() * 4, "a run never changes the size of an image");
        auto at = isize(0);
        for (auto const& leaf : image.texels)
            for (auto k = 0; k < 4; ++k)
                texels[at++] = byte(u8(leaf.bits >> (8 * k)));
    }
}

bool sgl::check::is_constant(checked_module const& m, flat_entry_point const& e, flat_expr_id id)
{
    if (!is_known(e, id))
        return false;
    auto const& x = e.at(id);
    auto const all_constant = [&](ast::range_of<flat_expr_id> range)
    {
        if (!is_known(e, range))
            return false;
        for (auto const argument : e.at(range))
            if (!is_constant(m, e, argument))
                return false;
        return true;
    };
    if (x.node.is<flat_literal>() || x.node.is<flat_int_literal>() || x.node.is<flat_wide_literal>()
        || x.node.is<flat_bool_literal>() || x.node.is<flat_enum_value>())
        return true;
    if (auto const* const construct = x.node.try_as<flat_construct>())
        return all_constant(construct->arguments);
    if (auto const* const member = x.node.try_as<flat_member>())
        return is_constant(m, e, member->object);
    if (auto const* const n = x.node.try_as<flat_not>())
        return is_constant(m, e, n->operand);
    if (auto const* const a = x.node.try_as<flat_and>())
        return is_constant(m, e, a->lhs) && is_constant(m, e, a->rhs);
    if (auto const* const o = x.node.try_as<flat_or>())
        return is_constant(m, e, o->lhs) && is_constant(m, e, o->rhs);
    if (auto const* const c = x.node.try_as<flat_call>())
    {
        auto const* const record = m.builtin_function(c->intrinsic);
        return c->is_pure && record != nullptr && record->evaluate != nullptr && !record->is_atomic
            && all_constant(c->arguments);
    }
    return false;
}

outcome sgl::check::evaluate_constant(checked_module const& m, flat_entry_point const& e, flat_expr_id id)
{
    auto const inputs = run_inputs();
    auto const limits = run_limits();
    auto run = machine{.m = m, .e = e, .inputs = inputs, .limits = limits, .fuel = limits.fuel};
    if (!is_constant(m, e, id))
    {
        run.type_error("an expression that is no constant");
        return cc::move(run.out);
    }
    auto result = value();
    if (run.eval(id, result).is_normal())
        run.out.result = cc::move(result);
    return cc::move(run.out);
}

cc::string sgl::check::dump(outcome const& o)
{
    auto const write = [](cc::string& out, value const& v)
    {
        for (auto const& leaf : v.leaves)
        {
            auto const wide = leaf.widened();
            if (wide.kind == value_kind::scalar_float)
                out.appendf(" {}{}", wide.as_float(), leaf.kind == value_kind::scalar_half ? "f16" : "");
            else if (wide.kind == value_kind::scalar_int)
                out.appendf(" {}{}", wide.as_int(), leaf.kind == value_kind::scalar_short ? "i16" : "");
            else if (wide.kind == value_kind::scalar_uint)
                out.appendf(" {}u{}", wide.as_uint(), leaf.kind == value_kind::scalar_ushort ? "16" : "");
            else if (leaf.kind == value_kind::scalar_long)
                out.appendf(" {}i64", leaf.as_long());
            else if (leaf.kind == value_kind::scalar_ulong)
                out.appendf(" {}u64", leaf.as_ulong());
            else if (leaf.kind == value_kind::scalar_double)
                out.appendf(" {}f64", leaf.as_double());
            else
                out.appendf(" {}", leaf.as_bool() ? "true" : "false");
        }
    };
    auto out = cc::string(to_string(o.status));
    if (!o.detail.empty())
        out.appendf(" ({})", o.detail);
    write(out, o.result);
    for (auto const& v : o.trace)
    {
        out += " | print";
        write(out, v);
    }
    for (auto const& b : o.buffers)
    {
        out += " | buffer";
        write(out, value{.leaves = b.leaves});
    }
    for (auto const& image : o.images)
    {
        out += " | image";
        write(out, value{.leaves = image.texels});
    }
    return out;
}
