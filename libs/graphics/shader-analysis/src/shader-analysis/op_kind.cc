#include "op_kind.hh"

namespace shan
{
cc::string_view name_of(op_kind kind)
{
    switch (kind)
    {
    case op_kind::float_arithmetic:
        return "float_arithmetic";
    case op_kind::int_arithmetic:
        return "int_arithmetic";
    case op_kind::extended:
        return "extended";
    case op_kind::conversion:
        return "conversion";
    case op_kind::compare:
        return "compare";
    case op_kind::logic:
        return "logic";
    case op_kind::composite:
        return "composite";
    case op_kind::address:
        return "address";
    case op_kind::load:
        return "load";
    case op_kind::store:
        return "store";
    case op_kind::variable:
        return "variable";
    case op_kind::image:
        return "image";
    case op_kind::atomic:
        return "atomic";
    case op_kind::barrier:
        return "barrier";
    case op_kind::subgroup:
        return "subgroup";
    case op_kind::derivative:
        return "derivative";
    case op_kind::ray:
        return "ray";
    case op_kind::phi:
        return "phi";
    case op_kind::control:
        return "control";
    case op_kind::declaration:
        return "declaration";
    case op_kind::other:
        return "other";
    }
    return "other";
}

op_kind spirv_kind_of(u16 opcode)
{
    // the core grammar's numbers, by range where a range is one kind
    auto const in = [&](u16 first, u16 last) { return opcode >= first && opcode <= last; };
    if (opcode == 12)
        return op_kind::extended;
    if (opcode == 59)
        return op_kind::variable;
    if (opcode == 61)
        return op_kind::load;
    if (opcode == 62 || opcode == 63)
        return op_kind::store;
    if (opcode == 60 || in(65, 68))
        return op_kind::address;
    if (in(77, 84) || opcode == 400)
        return op_kind::composite;
    if (in(86, 107) || in(305, 320))
        return op_kind::image;
    if (in(109, 124))
        return op_kind::conversion;
    if (opcode == 126 || opcode == 128 || opcode == 130 || opcode == 132 || in(134, 135) || in(137, 139) || in(149, 152)
        || in(194, 205))
        return op_kind::int_arithmetic;
    if (opcode == 127 || opcode == 129 || opcode == 131 || opcode == 133 || opcode == 136 || in(140, 148))
        return op_kind::float_arithmetic;
    if (in(156, 163) || in(170, 191))
        return op_kind::compare;
    if (in(164, 169))
        return op_kind::logic;
    if (in(207, 215))
        return op_kind::derivative;
    if (in(224, 225))
        return op_kind::barrier;
    if (in(227, 242) || in(6035, 6035) || in(5614, 5615))
        return op_kind::atomic;
    if (opcode == 245)
        return op_kind::phi;
    if (in(54, 57) || in(246, 255) || in(4416, 4416) || in(5379, 5380))
        return op_kind::control;
    if (in(333, 366) || in(4421, 4432) || in(5571, 5578))
        return op_kind::subgroup;
    if (in(4445, 4479) || in(6016, 6031) || in(5334, 5344) || in(5249, 5250) || in(5337, 5341) || in(5360, 5396))
        return op_kind::ray;
    // the module's statements: debug, annotation, extension, mode, type and constant instructions
    if (in(0, 11) || in(14, 17) || in(19, 52) || in(71, 76) || in(330, 332) || in(4417, 4418))
        return op_kind::declaration;
    return op_kind::other;
}

i32 latency_of(op_kind kind)
{
    switch (kind)
    {
    case op_kind::load:
    case op_kind::image:
    case op_kind::atomic:
        return 16;
    case op_kind::extended:
    case op_kind::subgroup:
    case op_kind::derivative:
        return 4;
    case op_kind::float_arithmetic:
    case op_kind::int_arithmetic:
    case op_kind::conversion:
    case op_kind::compare:
    case op_kind::logic:
        return 1;
    default:
        return 0;
    }
}
} // namespace shan
