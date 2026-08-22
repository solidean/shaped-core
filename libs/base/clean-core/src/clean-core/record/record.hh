#pragma once

#include <clean-core/record/writer.hh>

// The macro seam every recording front end expands into.
//
// A site is a `static constexpr` descriptor plus a write, and the descriptor is CONSTANT-INITIALIZED — no guard
// variable, no one-time-initialization check, no first-hit cost.
// Which domain a site belongs to comes from `cc_rec_domain()`, resolved by ordinary unqualified name lookup, so a site
// never names one and a library gets its own by declaring one in its fwd.hh.
//
// These are the low-level spellings.
// CC_LOG_*, CC_RECORD_SCOPE, CC_RECORD, CC_RECORD_MARK and CC_RECORD_STAT are what code normally reaches for; they
// all come out here.

/// Defines this site's descriptor as `var_name`, visible for the rest of the enclosing block.
///
/// `fields_` and `field_count_` describe the payload for a consumer that has never heard of it; pass `nullptr, 0` when
/// there is none.
#define CC_REC_DEFINE_DESC(var_name, kind_, level_, enable_bit_, name_, unit_, fields_, field_count_, payload_size_) \
    static constexpr ::cc::rec::desc var_name = {                                                                    \
        .kind = (kind_),                                                                                             \
        .lvl = (level_),                                                                                             \
        .enable_bit = (enable_bit_),                                                                                 \
        .name = (name_),                                                                                             \
        .quantity = (unit_),                                                                                         \
        .dom = cc_rec_domain(),                                                                                      \
        .site = ::cc::rec::source_ref::from(::cc::source_location::current()),                                       \
        .fields = (fields_),                                                                                         \
        .field_count = (field_count_),                                                                               \
        .fixed_payload_size = (payload_size_),                                                                       \
    }

/// Records one event of `kind_` under `category_`, with no payload.
#define CC_RECORD_EVENT(kind_, category_, name_)                                                                    \
    do                                                                                                              \
    {                                                                                                               \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, (kind_), ::cc::rec::level::info, ::cc::rec::enable_bit_of(category_), \
                           (name_), nullptr, nullptr, 0, 0);                                                        \
        ::cc::rec::record_event(cc_rec_site_desc_);                                                                 \
    } while (false)

/// Records one event of `kind_` under `category_`, copying `payload_` inline.
/// `payload_` must be trivially copyable, and `fields_` must describe its layout.
#define CC_RECORD_EVENT_WITH(kind_, category_, name_, unit_, fields_, payload_)                                     \
    do                                                                                                              \
    {                                                                                                               \
        CC_REC_DEFINE_DESC(cc_rec_site_desc_, (kind_), ::cc::rec::level::info, ::cc::rec::enable_bit_of(category_), \
                           (name_), (unit_), (fields_), ::cc::u16(CC_ARRAY_COUNT_OF(fields_)),                      \
                           ::cc::u32(sizeof(payload_)));                                                            \
        ::cc::rec::record_event(cc_rec_site_desc_, (payload_));                                                     \
    } while (false)
