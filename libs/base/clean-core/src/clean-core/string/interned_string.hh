#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/mutex.hh>

#include <compare> // std::strong_ordering, which the language requires for operator<=>

/// One canonical copy of a byte sequence, plus its precomputed hash.
/// The bytes follow this header inline, so an entry is one allocation and as_string_view() costs no lookup.
/// Entries never move and are never freed while their table lives, which is what lets a handle be a bare pointer.
struct cc::impl::intern_entry
{
    u64 hash = 0;
    isize size = 0;

    [[nodiscard]] cc::string_view as_string_view() const
    {
        return cc::string_view(reinterpret_cast<char const*>(this + 1), size);
    }
};

/// A handle to one canonical copy of a string: comparison is a pointer compare, hashing is a load, and the bytes
/// exist once however many times they were interned.
/// Trivially copyable and 8 bytes, so pass it by value.
///
/// **The identity inside is process-local and must never leave the process.**
/// Nothing about a handle survives a save, a wire protocol or a hash of persistent data — two runs will disagree.
/// Serialize `as_string_view()`, and hash durable data over those bytes.
/// The pointer is unreachable from outside for exactly that reason: there is no id to accidentally write down.
///
/// **There are no relational operators, deliberately.**
/// Two total orders are available and they are not interchangeable, so the call site names the one it means
/// rather than a `<` quietly picking.
/// compare_bytes is reproducible and costs a memcmp; compare_identity is a pointer compare and differs every run.
class cc::interned_string
{
public:
    /// The empty string, which is what a default-constructed handle is.
    constexpr interned_string() = default;

    [[nodiscard]] cc::string_view as_string_view() const
    {
        return _entry ? _entry->as_string_view() : cc::string_view();
    }
    [[nodiscard]] isize size() const { return _entry ? _entry->size : 0; }
    [[nodiscard]] bool empty() const { return _entry == nullptr; }

    // comparison operators (hidden friends), mirroring string_view's set
public:
    /// Sound as a pointer compare because a table hands out exactly one entry per distinct byte sequence.
    [[nodiscard]] friend constexpr bool operator==(interned_string lhs, interned_string rhs)
    {
        return lhs._entry == rhs._entry;
    }
    [[nodiscard]] friend constexpr bool operator!=(interned_string lhs, interned_string rhs)
    {
        return lhs._entry != rhs._entry;
    }

    // ordering — two of them, neither spelled `<`
public:
    /// Orders by the canonical bytes, so the result is the same in every process that interns the same strings.
    /// This is what a sorted structure whose order is saved, sent or displayed must use.
    /// Costs a memcmp, unlike everything else on this type.
    ///
    /// The comparison is by UNSIGNED byte value, so a non-ASCII id orders the way its bytes do rather than the way a
    /// signed `char` would put it — which matters the moment the order reaches a file, a hash or a peer.
    [[nodiscard]] std::strong_ordering compare_bytes(interned_string rhs) const
    {
        return as_string_view().compare(rhs.as_string_view()) <=> 0;
    }

    /// Orders by the process-local identity: one pointer compare, and the cheapest total order available.
    /// **The order is different on every run.** Reach for it only where the order itself is never observed —
    /// a scratch sort before a dedup, a lookup structure nobody iterates in order.
    /// Anything saved, sent or shown to a person wants compare_bytes.
    [[nodiscard]] std::strong_ordering compare_identity(interned_string rhs) const { return _entry <=> rhs._entry; }

    /// Sort predicate over the canonical bytes: cc::sort(v, cc::interned_string::by_bytes{}).
    struct by_bytes
    {
        [[nodiscard]] bool operator()(interned_string lhs, interned_string rhs) const
        {
            return lhs.compare_bytes(rhs) < 0;
        }
    };

    /// Sort predicate over the process-local identity, with compare_identity's run-to-run instability.
    struct by_identity
    {
        [[nodiscard]] bool operator()(interned_string lhs, interned_string rhs) const
        {
            return lhs.compare_identity(rhs) < 0;
        }
    };

    // hashing
public:
    /// The same value hash(string_view) gives for the same bytes, so an interned key finds a string-keyed entry.
    [[nodiscard]] friend u64 hash(interned_string v) { return v._entry ? v._entry->hash : cc::make_hash_of_bytes({}); }

private:
    friend class cc::string_interner;

    explicit constexpr interned_string(impl::intern_entry const* entry) : _entry(entry) {}

    impl::intern_entry const* _entry = nullptr;
};

/// One shard of a string_interner: an index over the entries plus the arena their bytes live in.
/// Sharding is only about contention — which shard a string lands in is a function of its hash, so it changes
/// nothing observable.
struct cc::impl::intern_shard
{
    cc::map<cc::string_view, intern_entry const*> index;

    cc::vector<cc::allocation<byte>> blocks;
    byte* cursor = nullptr;
    byte* block_end = nullptr;
};

/// A table of interned strings.
///
/// Reach for cc::intern() instead unless you specifically want isolation — a test that wants to observe an empty
/// table, or to prove that two tables are independent.
///
/// **Every handle a table hands out dangles once the table dies.**
/// Entries live in the table's own arena and are freed with it, so a table must outlive every handle taken from it.
/// The process-wide one never dies, which is why cc::intern() is the path with no lifetime question attached.
///
/// Interning from several threads at once is safe and needs no external locking.
/// Without threads (CC_HAS_THREADS == 0) the locks compile away and the table costs nothing extra.
class cc::string_interner
{
public:
    string_interner() = default;

    string_interner(string_interner&&) = delete;
    string_interner& operator=(string_interner&&) = delete;

    /// Returns the handle for `s`, adding it to the table on first sight.
    /// The bytes are copied, so `s` need not outlive the call.
    [[nodiscard]] interned_string intern(cc::string_view s);

    /// How many distinct strings the table holds, not counting the empty string (which needs no entry).
    /// Not const: reading it takes every shard lock in turn.
    [[nodiscard]] isize size();

private:
    /// Contention only; 16 is enough that concurrent interning from a normal thread count rarely collides.
    static constexpr isize shard_count = 16;

    cc::mutex<impl::intern_shard> _shards[shard_count];
};

namespace cc
{
/// Interns into the process-wide table, which is created on first use and never destroyed.
/// This is the default: handles from it are valid for the rest of the process, with no lifetime to track.
[[nodiscard]] interned_string intern(cc::string_view s);
} // namespace cc
