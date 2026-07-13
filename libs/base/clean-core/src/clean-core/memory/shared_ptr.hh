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
//     fixed offset after T, so nothing intrudes on T itself. e.g. shared_ptr<int> is a 12 B node (16 B class).
//   * intrusive (e.g. cc::async's own traits): the counts are members of T, so the node IS the object and the
//     handle needs no extra storage. Requires that destroy_object tears down only the payload and leaves the
//     counts alive (weak must outlive the object) — see the lifetime note below.
//
// Traits protocol — all static, operating on T* (so a base-class Traits also serves a derived T through the
// implicit upcast, which is how async uses one Traits keyed on async_node_base for every async<U>):
//   static constexpr bool supports_weak;                              // false => strong-only, no weak_ptr
//   static constexpr cc::isize node_size(cc::isize psize, cc::isize palign);  // total node bytes for a T
//   static constexpr cc::isize node_align(cc::isize palign);                  // total node alignment
//   static void init_control(T*);          // establish strong = 1 (and weak = 1 if supports_weak)
//   static void inc_strong(T*);
//   static bool dec_strong(T*);            // true iff the strong count reached 0
//   static void destroy_object(T*);        // tear down the payload; called exactly once, when strong hits 0
//   static void free_storage(T*);          // free the node; called exactly once (see lifetime)
//   // required only when supports_weak:
//   static void inc_weak(T*);
//   static bool dec_weak(T*);              // true iff the weak count reached 0
//   static bool try_lock_strong(T*);       // atomically inc strong iff currently != 0; true on success
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
// ============================================================================
// default_shared_traits — non-intrusive: control trails the payload in the node
// ============================================================================

template <class T>
struct default_shared_traits
{
    struct control
    {
        std::atomic<cc::u32> strong;
        std::atomic<cc::u32> weak;
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
        new (cc::placement_new, c) control; // begin the atomics' lifetime in the raw trailing storage
        c->strong.store(1, std::memory_order_relaxed);
        c->weak.store(1, std::memory_order_relaxed);
    }
    static void inc_strong(T* p) { ctrl(p)->strong.fetch_add(1, std::memory_order_relaxed); }
    static bool dec_strong(T* p) { return ctrl(p)->strong.fetch_sub(1, std::memory_order_acq_rel) == 1; }
    static void inc_weak(T* p) { ctrl(p)->weak.fetch_add(1, std::memory_order_relaxed); }
    static bool dec_weak(T* p) { return ctrl(p)->weak.fetch_sub(1, std::memory_order_acq_rel) == 1; }
    static bool try_lock_strong(T* p)
    {
        auto& s = ctrl(p)->strong;
        cc::u32 cur = s.load(std::memory_order_relaxed);
        while (cur != 0)
            if (s.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                return true;
        return false; // lost the race to the last strong drop -> object is (being) destroyed
    }
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
        if (_ptr != nullptr && Traits::dec_strong(_ptr))
        {
            Traits::destroy_object(_ptr);
            if constexpr (Traits::supports_weak)
            {
                if (Traits::dec_weak(_ptr)) // release the strong owners' collective weak count
                    Traits::free_storage(_ptr);
            }
            else
                Traits::free_storage(_ptr);
        }
        _ptr = nullptr;
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
        if (_ptr != nullptr && Traits::dec_weak(_ptr))
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
