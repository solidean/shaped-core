#include "llvm_bitstream.hh"

#include <clean-core/container/map.hh>
#include <clean-core/string/format.hh>

namespace babel::llvm
{
namespace
{
constexpr u32 k_end_block = 0;
constexpr u32 k_enter_subblock = 1;
constexpr u32 k_define_abbreviation = 2;
constexpr u32 k_unabbreviated_record = 3;
constexpr u32 k_blockinfo = 0;
constexpr u32 k_set_block_id = 1;
constexpr u32 k_wrapper_magic = 0x0B17C0DEu;

enum class encoding : u8
{
    literal,
    fixed,
    vbr,
    array,
    char6,
    blob,
};

struct abbreviation_op
{
    encoding kind = encoding::literal;
    u64 value = 0;
};

using abbreviation = cc::vector<abbreviation_op>;

class reader
{
public:
    explicit reader(bitstream& out) : _out(out) {}

    cc::result<bool> run(isize first_byte, isize end_byte)
    {
        _bit = u64(first_byte) * 8;
        _end = u64(end_byte) * 8;
        _out.blocks.push_back({.id = 0xffffffffu, .parent = -1});
        auto const top = cc::vector<abbreviation>();
        // the top level holds blocks alone, written with 2-bit abbreviation ids, and may end in padding
        while (_bit + 32 <= _end)
        {
            auto const id = read(2);
            if (id != k_enter_subblock)
            {
                if (id == 0 && rest_is_zero())
                    break;
                return cc::error(cc::format("llvm bitstream: abbreviation id {} at the top level, where only a block "
                                            "may stand",
                                            id));
            }
            if (auto r = enter_block(0); r.has_error())
                return cc::error(r.error().to_string());
        }
        return true;
    }

private:
    bitstream& _out;
    u64 _bit = 0;
    u64 _end = 0;
    bool _failed = false;
    cc::map<u32, cc::vector<abbreviation>> _blockinfo;

    [[nodiscard]] u64 read(u32 width)
    {
        if (_bit + width > _end)
        {
            _failed = true;
            _bit = _end;
            return 0;
        }
        auto value = u64(0);
        for (auto k = u32(0); k < width;)
        {
            auto const byte_at = _bit / 8;
            auto const shift = u32(_bit % 8);
            auto const take = cc::min(8u - shift, width - k);
            auto const bits = (u64(u8(_out.bytes[isize(byte_at)])) >> shift) & ((u64(1) << take) - 1);
            value |= bits << k;
            k += take;
            _bit += take;
        }
        return value;
    }
    [[nodiscard]] u64 read_vbr(u32 width)
    {
        auto const high = u64(1) << (width - 1);
        auto value = u64(0);
        for (auto shift = u32(0); shift < 70; shift += width - 1)
        {
            auto const piece = read(width);
            value |= (piece & (high - 1)) << shift;
            if ((piece & high) == 0 || _failed)
                return value;
        }
        _failed = true;
        return value;
    }
    void align32() { _bit = (_bit + 31) / 32 * 32; }
    [[nodiscard]] bool rest_is_zero() const
    {
        for (auto b = (_bit + 7) / 8; b < _end / 8; ++b)
            if (_out.bytes[isize(b)] != byte(0))
                return false;
        return true;
    }

    /// The block whose ENTER_SUBBLOCK was just read, under the block at `parent`.
    cc::result<bool> enter_block(u32 parent)
    {
        auto const id = u32(read_vbr(8));
        auto const width = u32(read_vbr(4));
        align32();
        auto const words = read(32);
        auto const end = _bit + words * 32;
        if (_failed || end > _end || width == 0 || width > 32)
            return cc::error(cc::format("llvm bitstream: block {} runs past the end of the stream", id));
        if (id == k_blockinfo)
        {
            if (auto r = read_blockinfo(width, end); r.has_error())
                return r;
            _bit = end;
            return true;
        }
        auto const index = u32(_out.blocks.size());
        _out.blocks.push_back({.id = id, .parent = i32(parent)});
        _out.blocks[parent].entries.push_back({.is_block = true, .index = index});
        auto abbreviations = cc::vector<abbreviation>();
        if (auto const* const shared = _blockinfo.get_ptr(id))
            for (auto const& a : *shared)
                abbreviations.push_back(a);
        while (true)
        {
            if (_bit >= end)
                return cc::error(cc::format("llvm bitstream: block {} has no END_BLOCK", id));
            auto const code = u32(read(width));
            if (_failed)
                return cc::error(cc::format("llvm bitstream: block {} runs past the end of the stream", id));
            if (code == k_end_block)
            {
                align32();
                return true;
            }
            if (code == k_enter_subblock)
            {
                if (auto r = enter_block(index); r.has_error())
                    return r;
                continue;
            }
            if (code == k_define_abbreviation)
            {
                auto a = read_abbreviation();
                if (a.has_error())
                    return cc::error(a.error().to_string());
                abbreviations.push_back(cc::move(a.value()));
                continue;
            }
            auto r = read_record(code, abbreviations);
            if (r.has_error())
                return cc::error(r.error().to_string());
            _out.blocks[index].entries.push_back({.is_block = false, .index = r.value()});
        }
    }

    cc::result<bool> read_blockinfo(u32 width, u64 end)
    {
        auto target = i64(-1);
        auto const none = cc::vector<abbreviation>();
        while (_bit < end)
        {
            auto const code = u32(read(width));
            if (code == k_end_block)
            {
                align32();
                return true;
            }
            if (code == k_define_abbreviation)
            {
                auto a = read_abbreviation();
                if (a.has_error())
                    return cc::error(a.error().to_string());
                if (target < 0)
                    return cc::error("llvm bitstream: BLOCKINFO defines an abbreviation before naming its block");
                _blockinfo[u32(target)].push_back(cc::move(a.value()));
                continue;
            }
            if (code == k_enter_subblock)
                return cc::error("llvm bitstream: a block inside BLOCKINFO");
            auto r = read_record(code, none);
            if (r.has_error())
                return cc::error(r.error().to_string());
            auto const& rec = _out.records[isize(r.value())];
            if (rec.code == k_set_block_id && rec.count > 0)
                target = i64(_out.operands[isize(rec.first)]);
            // BLOCKINFO's records say nothing a reader of the tree needs, so they leave it again
            _out.operands.resize_down_to(isize(rec.first));
            _out.records.pop_back();
        }
        return cc::error("llvm bitstream: BLOCKINFO has no END_BLOCK");
    }

    cc::result<abbreviation> read_abbreviation()
    {
        auto const count = read_vbr(5);
        auto a = abbreviation();
        for (auto k = u64(0); k < count; ++k)
        {
            if (read(1) == 1)
            {
                a.push_back({.kind = encoding::literal, .value = read_vbr(8)});
                continue;
            }
            switch (read(3))
            {
            case 1:
                a.push_back({.kind = encoding::fixed, .value = read_vbr(5)});
                break;
            case 2:
                a.push_back({.kind = encoding::vbr, .value = read_vbr(5)});
                break;
            case 3:
                a.push_back({.kind = encoding::array});
                break;
            case 4:
                a.push_back({.kind = encoding::char6});
                break;
            case 5:
                a.push_back({.kind = encoding::blob});
                break;
            default:
                return cc::error("llvm bitstream: an abbreviation operand of no known encoding");
            }
        }
        if (_failed)
            return cc::error("llvm bitstream: an abbreviation runs past the end of the stream");
        return a;
    }

    [[nodiscard]] u64 scalar(abbreviation_op const& op)
    {
        switch (op.kind)
        {
        case encoding::literal:
            return op.value;
        case encoding::fixed:
            return op.value == 0 ? 0 : read(u32(op.value));
        case encoding::vbr:
            return op.value == 0 ? 0 : read_vbr(u32(op.value));
        case encoding::char6:
        {
            constexpr char k_char6[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
            return u64(u8(k_char6[read(6)]));
        }
        default:
            _failed = true;
            return 0;
        }
    }

    cc::result<u32> read_record(u32 id, cc::span<abbreviation const> abbreviations)
    {
        auto rec = record{.abbreviation = id, .first = u32(_out.operands.size())};
        if (id == k_unabbreviated_record)
        {
            rec.code = u32(read_vbr(6));
            auto const count = read_vbr(6);
            for (auto k = u64(0); k < count && !_failed; ++k)
                _out.operands.push_back(read_vbr(6));
        }
        else
        {
            auto const at = isize(id) - 4;
            if (at < 0 || at >= abbreviations.size())
                return cc::error(cc::format("llvm bitstream: abbreviation id {} is not defined here", id));
            auto const& a = abbreviations[at];
            auto values = cc::vector<u64>();
            for (auto k = isize(0); k < a.size(); ++k)
            {
                auto const& op = a[k];
                if (op.kind == encoding::array)
                {
                    if (k + 1 >= a.size())
                        return cc::error("llvm bitstream: an array abbreviation names no element encoding");
                    auto const length = read_vbr(6);
                    auto const& element = a[k + 1];
                    for (auto e = u64(0); e < length && !_failed; ++e)
                        values.push_back(scalar(element));
                    ++k;
                    continue;
                }
                if (op.kind == encoding::blob)
                {
                    auto const length = read_vbr(6);
                    align32();
                    rec.blob_first = u32(_bit / 8);
                    rec.blob_count = u32(length);
                    _bit += length * 8;
                    align32();
                    if (_bit > _end)
                        _failed = true;
                    continue;
                }
                values.push_back(scalar(op));
            }
            if (values.empty())
                return cc::error("llvm bitstream: an abbreviated record states no code");
            rec.code = u32(values[0]);
            for (auto k = isize(1); k < values.size(); ++k)
                _out.operands.push_back(values[k]);
        }
        if (_failed)
            return cc::error("llvm bitstream: a record runs past the end of the stream");
        rec.count = u32(_out.operands.size()) - rec.first;
        _out.records.push_back(rec);
        return u32(_out.records.size() - 1);
    }
};

[[nodiscard]] u32 word_at(cc::span<byte const> b, isize at)
{
    return u32(u8(b[at])) | (u32(u8(b[at + 1])) << 8) | (u32(u8(b[at + 2])) << 16) | (u32(u8(b[at + 3])) << 24);
}
} // namespace

cc::result<bitstream> read_bitstream(cc::span<byte const> bytes)
{
    auto first = isize(0);
    auto end = bytes.size();
    // a wrapper header: its magic, a version, then the bitcode's offset and size
    if (bytes.size() >= 20 && word_at(bytes, 0) == k_wrapper_magic)
    {
        first = isize(word_at(bytes, 8));
        end = first + isize(word_at(bytes, 12));
        if (end > bytes.size())
            return cc::error("llvm bitstream: the wrapper header's bitcode runs past the end");
    }
    if (end - first < 4 || bytes[first] != byte('B') || bytes[first + 1] != byte('C') || bytes[first + 2] != byte(0xC0)
        || bytes[first + 3] != byte(0xDE))
        return cc::error("llvm bitstream: no `BC 0xC0DE` magic");
    auto out = bitstream();
    out.bytes = cc::vector<byte>::create_copy_of(bytes);
    auto r = reader(out);
    if (auto result = r.run(first + 4, end); result.has_error())
        return cc::error(result.error().to_string());
    return out;
}
} // namespace babel::llvm
