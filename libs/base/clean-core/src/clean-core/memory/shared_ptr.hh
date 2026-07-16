#pragma once

#include <clean-core/common/utility.hh> // cc::move, cc::forward, cc::placement_new, cc::align_up
#include <clean-core/fwd.hh>
#include <clean-core/memory/node_allocation.hh>

#include <atomic>
#include <type_traits> // std::is_convertible_v

// cc::shared_ptr<T, Traits> / cc::weak_ptr<T, Traits> — an 8 B, single-pointer, INTRUSIVE-refcount smart
// pointer pair. Unlike std::shared_ptr there is no separate control block reached through a second pointer:
// the handle stores exactly the payload pointer (get() is a no-op), and the reference counts live in the SAME
// node, located by a Traits type. Two layouts fall out of one protocol:
//   * default (cc::default_shared_traits): the node is [ T payload | trailing control ]; the counts sit at a
//     fixed offset after T, so nothing intrudes on T itself. e.g. shared_ptr<int> is a 16 B node (16 B class).
//   * intrusive (e.g. cc::async's own traits): the counts are members of T, so the node IS the object and the
//     handle needs no extra storage. Requires that destroy_object tears down only the payload and leaves the
//     counts alive (weak must outlive the object) — see the lifetime note below.
//
// Traits protocol — all static, operating on T* (so a base-class Traits also serves a derived T through the
// implicit upcast, which is how async uses one Traits keyed on async_node_base for every async<U>):
//   static constexpr bool supports_weak;                              // false => strong-only, no weak_ptr
//   static constexpr cc::isize node_size(cc::isize psize, cc::isize palign);  // total node bytes for a T
//   static constexpr cc::isize node_align(cc::isize palign);                  // total node alignment
//   static void init_control(T*);              // establish strong = 1 (and weak = 1 if supports_weak)
//   static void inc_strong(T*);
//   static shared_release release_strong(T*);  // drop one strong ref; says what the caller must do next
//   static void destroy_object(T*);            // tear down the payload; called exactly once, when strong hits 0
//   static void free_storage(T*);              // free the node; called exactly once (see lifetime)
//   // required only when supports_weak:
//   static void inc_weak(T*);
//   static bool release_weak(T*);          // drop one weak ref; true iff the weak count reached 0 => free_storage
//   static bool try_lock_strong(T*);       // atomically inc strong iff currently != 0; true on success
//
// release_strong reports what the caller must do, IN ORDER:
//   {false, false} -> nothing.
//   {true,  false} -> destroy_object(p), THEN release_weak(p) to drop the strong owners' collective weak count;
//                     free_storage(p) iff that returns true. The order is load-bearing: releasing the collective
//                     weak before destroy_object would let a racing weak_ptr drop free the storage under it.
//   {true,  true}  -> destroy_object(p), then free_storage(p). Do NOT call release_weak: no other reference
//                     exists (a strong-only Traits, or the sole-owner fast path — see cc::fused_refcount).
// It is one call rather than a dec_strong(T*) -> bool so a Traits CAN answer both questions from a single load.
// The protocol says nothing about representation: two u32s, one fused u64, or a lone strong count all fit.
//
// Lifetime — standard shared/weak counting: the strong owners collectively hold ONE weak count. With weak
// support destroy_object fires when strong hits 0 and free_storage fires when weak then hits 0; without weak
// support free_storage immediately follows destroy_object. The counts must stay readable between those two
// points, so destroy_object must NOT destroy them: with trailing control this is automatic (separate storage);
// intrusive control must run a payload-only teardown and leave the count members intact until free_storage.
//
// Create only via cc::make_shared<T, Traits>(...): no adoption of an arbitrary raw pointer (a node is born
// owned, strong = 1). Upcasts to a base with the SAME Traits are allowed; aliasing/projection to a subobject
// is not (deferred). from_alive() is the one intrusive escape hatch: mint a handle from a pointer whose
// storage is known to still be alive (strong or weak > 0).

namespace cc
{
/// What dropping a strong reference leaves for the caller to do, in this order. `free` without `destroy` never
/// happens. See the protocol block above for the full contract.
struct shared_release
{
    bool destroy; ///< this was the last strong reference: run destroy_object
    bool free;    ///< also the last reference of any kind: run free_storage, and skip release_weak
};

/// The fused strong/weak counter both stock Traits build on: strong in the high 32 bits, weak in the low 32.
/// The weak half counts every weak_ptr PLUS one collective count shared by all strong owners, so the two halves
/// move independently — never as a unit. Fusing buys exactly one thing: release_strong can test both counts
/// with a single load, and the sole-owner case then needs no locked RMW at all.
///
/// Weak is the LOW half, so a weak overflow carries straight into the strong count. 2^32 live weak refs is
/// unreachable in practice (each costs a pointer somewhere) — an invariant, not a check.
struct fused_refcount
{
    static constexpr cc::u64 strong_unit = cc::u64(1) << 32;
    static constexpr cc::u64 weak_unit = 1;
    static constexpr cc::u64 sole_owner = strong_unit | weak_unit; // strong == 1 && weak == 1

    static void init(std::atomic<cc::u64>& c) { c.store(sole_owner, std::memory_order_relaxed); }
    static void inc_strong(std::atomic<cc::u64>& c) { c.fetch_add(strong_unit, std::memory_order_relaxed); }
    static void inc_weak(std::atomic<cc::u64>& c) { c.fetch_add(weak_unit, std::memory_order_relaxed); }

    static shared_release release_strong(std::atomic<cc::u64>& c)
    {
        // Reading exactly (1,1) proves we hold the only reference of any kind: no other thread can mint one,
        // because minting requires already holding one. So there is nobody to race and no RMW is needed — this
        // is the whole point of fusing. The ACQUIRE is load-bearing: it pairs with the release of whoever
        // dropped the second-to-last reference, ordering their writes before our teardown. Note this leaves the
        // counts reading (1,1) through destroy_object / free_storage; nothing may read them there.
        if (c.load(std::memory_order_acquire) == sole_owner)
            return {true, true};

        cc::u64 const old = c.fetch_sub(strong_unit, std::memory_order_acq_rel);
        return {(old >> 32) == 1, false}; // free is never decided here: the collective weak is still held
    }
    static bool release_weak(std::atomic<cc::u64>& c)
    {
        return (c.fetch_sub(weak_unit, std::memory_order_acq_rel) & 0xFFFF'FFFF) == 1;
    }
    /// Strong +1 iff strong != 0. Guards the HIGH half, so concurrent weak traffic cannot end the loop early —
    /// though it can make the CAS spuriously fail, which is the one price fusing charges.
    static bool try_lock_strong(std::atomic<cc::u64>& c)
    {
        cc::u64 cur = c.load(std::memory_order_relaxed);
        while ((cur >> 32) != 0)
            if (c.compare_exchange_weak(cur, cur + strong_unit, std::memory_order_acq_rel, std::memory_order_relaxed))
                return true; // strong > 0 held, so the collective weak already exists and is shared
        return false;        // lost the race to the last strong drop -> object is (being) destroyed
    }
};
static_assert(std::atomic<cc::u64>::is_always_lock_free, "fused refcounts need a lock-free 64-bit atomic");

// ============================================================================
// default_shared_traits — non-intrusive: control trails the payload in the node
// ============================================================================

template <class T>
struct default_shared_traits
{
    struct control
    {
        std::atomic<cc::u64> counts; // fused strong:weak — see cc::fused_refcount
    };

    static constexpr bool supports_weak = true;

    // node layout: payload at offset 0, control at the next control-aligned offset after it
    static constexpr cc::isize control_offset = cc::align_up(cc::isize(sizeof(T)), cc::isize(alignof(control)));

    static constexpr cc::isize node_size(cc::isize /*psize*/, cc::isize /*palign*/)
    {
        return control_offset + cc::isize(sizeof(control));
    }
    static constexpr cc::isize node_align(cc::isize palign)
    {
        return palign > cc::isize(alignof(control)) ? palign : cc::isize(alignof(control));
    }

    static void init_control(T* p)
    {
        control* const c = ctrl(p);
        new (cc::placement_new, c) control; // begin the atomic's lifetime in the raw trailing storage
        fused_refcount::init(c->counts);
    }
    static void inc_strong(T* p) { fused_refcount::inc_strong(ctrl(p)->counts); }
    static shared_release release_strong(T* p) { return fused_refcount::release_strong(ctrl(p)->counts); }
    static void inc_weak(T* p) { fused_refcount::inc_weak(ctrl(p)->counts); }
    static bool release_weak(T* p) { return fused_refcount::release_weak(ctrl(p)->counts); }
    static bool try_lock_strong(T* p) { return fused_refcount::try_lock_strong(ctrl(p)->counts); }
    static void destroy_object(T* p) { p->~T(); } // control trails in separate storage, so this is weak-safe
    static void free_storage(T* p)
    {
        cc::node_allocation_free(reinterpret_cast<cc::byte*>(p),
                                 cc::node_class_index_from_size_and_align(node_size(0, 0), node_align(alignof(T))));
    }

private:
    static control* ctrl(T* p) { return reinterpret_cast<control*>(reinterpret_cast<cc::byte*>(p) + control_offset); }
};

// ============================================================================
// shared_ptr / weak_ptr
// ============================================================================

template <class T, class Traits>
struct weak_ptr;

template <class T, class Traits = default_shared_traits<T>, class... Args>
[[nodiscard]] shared_ptr<T, Traits> make_shared(Args&&... args);

template <class T, class Traits = default_shared_traits<T>>
struct shared_ptr
{
    // ctors / dtor
public:
    shared_ptr() = default;
    shared_ptr(std::nullptr_t) {}

    shared_ptr(shared_ptr const& o) : _ptr(o._ptr) { inc(); }
    shared_ptr(shared_ptr&& o) noexcept : _ptr(o._ptr) { o._ptr = nullptr; }

    /// Upcast from a derived handle sharing the same Traits (U* must convert to T*).
    template <class U>
        requires(std::is_convertible_v<U*, T*>)
    shared_ptr(shared_ptr<U, Traits> const& o) : _ptr(o.get())
    {
        inc();
    }
    template <class U>
        requires(std::is_convertible_v<U*, T*>)
    shared_ptr(shared_ptr<U, Traits>&& o) noexcept : _ptr(o.get())
    {
        o._ptr = nullptr; // steal the ref (befriended below)
    }

    shared_ptr& operator=(shared_ptr o) noexcept // by value: copy-and-swap, handles self-assignment
    {
        swap(o);
        return *this;
    }
    shared_ptr& operator=(std::nullptr_t)
    {
        reset();
        return *this;
    }

    ~shared_ptr() { reset(); }

    /// Drop this strong reference; destroys the object when the last strong goes, frees storage when the last
    /// weak (or the last strong, without weak support) goes.
    void reset()
    {
        if (_ptr != nullptr)
        {
            auto const r = Traits::release_strong(_ptr);
            if (r.destroy)
            {
                Traits::destroy_object(_ptr);
                bool do_free = r.free;
                if constexpr (Traits::supports_weak)
                    if (!do_free) // release the strong owners' collective weak count — only AFTER teardown
                        do_free = Traits::release_weak(_ptr);
                if (do_free)
                    Traits::free_storage(_ptr);
            }
            _ptr = nullptr;
        }
    }

    // access
public:
    [[nodiscard]] T* get() const { return _ptr; }
    [[nodiscard]] T& operator*() const { return *_ptr; }
    [[nodiscard]] T* operator->() const { return _ptr; }
    [[nodiscard]] bool is_valid() const { return _ptr != nullptr; }
    explicit operator bool() const { return _ptr != nullptr; }

    void swap(shared_ptr& o) noexcept
    {
        T* const t = _ptr;
        _ptr = o._ptr;
        o._ptr = t;
    }

    // comparison (C++20 synthesizes != and reversed orders)
public:
    friend bool operator==(shared_ptr const& a, shared_ptr const& b) { return a._ptr == b._ptr; }
    friend bool operator==(shared_ptr const& a, std::nullptr_t) { return a._ptr == nullptr; }
    friend bool operator==(shared_ptr const& a, T const* b) { return a._ptr == b; }

    // low-level
public:
    /// Mint a strong handle from a raw pointer whose storage is known to still be alive (strong or weak > 0):
    /// intrusive self-recovery. Undefined if the storage has already been freed.
    [[nodiscard]] static shared_ptr from_alive(T* p)
    {
        shared_ptr r;
        r._ptr = p;
        if (p != nullptr)
            Traits::inc_strong(p);
        return r;
    }

private:
    // adopt an already-held strong reference without bumping (make_shared's birth ref, weak::lock's upgrade).
    [[nodiscard]] static shared_ptr adopt(T* p)
    {
        shared_ptr r;
        r._ptr = p;
        return r;
    }
    void inc()
    {
        if (_ptr != nullptr)
            Traits::inc_strong(_ptr);
    }

    T* _ptr = nullptr;

    template <class U, class Tr>
    friend struct shared_ptr;
    template <class U, class Tr>
    friend struct weak_ptr;
    template <class U, class Tr, class... Args>
    friend shared_ptr<U, Tr> make_shared(Args&&...);
};

template <class T, class Traits = default_shared_traits<T>>
struct weak_ptr
{
    static_assert(Traits::supports_weak, "weak_ptr requires a Traits with supports_weak == true");

    // ctors / dtor
public:
    weak_ptr() = default;
    weak_ptr(std::nullptr_t) {}

    weak_ptr(weak_ptr const& o) : _ptr(o._ptr) { inc(); }
    weak_ptr(weak_ptr&& o) noexcept : _ptr(o._ptr) { o._ptr = nullptr; }

    /// From a strong handle (same type or upcast to a base with the same Traits).
    template <class U>
        requires(std::is_convertible_v<U*, T*>)
    weak_ptr(shared_ptr<U, Traits> const& s) : _ptr(s.get())
    {
        inc();
    }
    template <class U>
        requires(std::is_convertible_v<U*, T*>)
    weak_ptr(weak_ptr<U, Traits> const& o) : _ptr(o.get())
    {
        inc();
    }

    weak_ptr& operator=(weak_ptr o) noexcept
    {
        swap(o);
        return *this;
    }
    weak_ptr& operator=(std::nullptr_t)
    {
        reset();
        return *this;
    }

    ~weak_ptr() { reset(); }

    void reset()
    {
        if (_ptr != nullptr && Traits::release_weak(_ptr))
            Traits::free_storage(_ptr);
        _ptr = nullptr;
    }

    // access
public:
    /// Try to obtain a strong handle; empty if the object has already been destroyed.
    [[nodiscard]] shared_ptr<T, Traits> lock() const
    {
        if (_ptr != nullptr && Traits::try_lock_strong(_ptr))
            return shared_ptr<T, Traits>::adopt(_ptr);
        return {};
    }

    /// Raw address of the (possibly already destroyed) object — for identity comparison only, never deref.
    [[nodiscard]] T* get() const { return _ptr; }
    explicit operator bool() const { return _ptr != nullptr; }

    void swap(weak_ptr& o) noexcept
    {
        T* const t = _ptr;
        _ptr = o._ptr;
        o._ptr = t;
    }

    // low-level
public:
    /// Mint a weak handle from a raw pointer whose storage is known alive (strong or weak > 0): self-recovery.
    [[nodiscard]] static weak_ptr from_alive(T* p)
    {
        weak_ptr r;
        r._ptr = p;
        if (p != nullptr)
            Traits::inc_weak(p);
        return r;
    }

    /// Take over one weak count the caller already holds — no inc_weak. The twin of release(): together they
    /// move a weak reference into and out of hand-rolled storage (e.g. a tagged word) without a redundant
    /// inc/dec pair. The caller must not also release its own count.
    [[nodiscard]] static weak_ptr adopt(T* p)
    {
        weak_ptr r;
        r._ptr = p;
        return r;
    }

    /// Give up ownership of the weak count without dec_weak, returning the raw pointer (null if empty). The
    /// caller takes over releasing it.
    [[nodiscard]] T* release()
    {
        T* const p = _ptr;
        _ptr = nullptr;
        return p;
    }

private:
    void inc()
    {
        if (_ptr != nullptr)
            Traits::inc_weak(_ptr);
    }

    T* _ptr = nullptr;

    template <class U, class Tr>
    friend struct weak_ptr;
    friend struct shared_ptr<T, Traits>;
};

/// Create a T in one slab-allocated node (sized by the Traits: T plus a trailing control block by default, or
/// the object itself for intrusive control) with strong = 1. The only way to construct a shared_ptr.
template <class T, class Traits, class... Args>
[[nodiscard]] shared_ptr<T, Traits> make_shared(Args&&... args)
{
    constexpr cc::isize sz = Traits::node_size(cc::isize(sizeof(T)), cc::isize(alignof(T)));
    constexpr cc::isize al = Traits::node_align(cc::isize(alignof(T)));
    constexpr auto idx = cc::node_class_index_from_size_and_align(sz, al);
    auto* raw = cc::default_node_allocator().allocate_node_bytes(idx, sz, al);
    T* const p = new (cc::placement_new, raw) T(cc::forward<Args>(args)...);
    Traits::init_control(p);
    return shared_ptr<T, Traits>::adopt(p);
}
} // namespace cc
