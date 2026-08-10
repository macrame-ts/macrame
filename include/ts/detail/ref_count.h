#pragma once

// Generic intrusive reference counting - one allocation and one-pointer handles, vs
// `std::shared_ptr`'s separate control block + two-word handle. A type `T` that inherits
// `detail::Ref_counted<T>` (CRTP) gets an intrusive count and is held by `detail::Ref_ptr<T>`.
// This generalizes the pattern `Task_control_block`/`Task_ptr` already use by hand (which keeps
// its own `destroy`-thunk variant because it is aliased inside `Executable`/coroutine-frame
// wrappers; a plain `Ref_counted<T>` deletes as `T*`, correct for types that are the allocation).
//
// Prefer this over `std::shared_ptr` for types we control (see CLAUDE.md). Ordering matches the
// standard intrusive idiom: `fetch_add(relaxed)` on inc, `fetch_sub(acq_rel)` + destroy-at-0.

#include <atomic>
#include <cstdint>
#include <utility>

namespace ts::detail
{

// CRTP base: `struct Foo : Ref_counted<Foo> {...};`. Held by `Ref_ptr<Foo>`, deleted as `Foo*`
// when the last handle drops. The count starts at 0 (the first `Ref_ptr` takes it to 1).
template<typename T>
struct Ref_counted
{
    std::atomic<std::uint32_t> refcount_{ 0 };

    void add_ref() noexcept
    {
        refcount_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept
    {
        if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete static_cast<T*>(this);
    }
};

// One-pointer owning handle for a `Ref_counted<T>`. Copy adds a ref, destroy releases.
template<typename T>
class Ref_ptr
{
public:
    Ref_ptr() noexcept = default;
    Ref_ptr(std::nullptr_t) noexcept {}
    explicit Ref_ptr(T* p) noexcept : p_(p) { if (p_) p_->add_ref(); }
    Ref_ptr(const Ref_ptr& o) noexcept : p_(o.p_) { if (p_) p_->add_ref(); }
    Ref_ptr(Ref_ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Ref_ptr& operator=(const Ref_ptr& o) noexcept
    {
        if (o.p_) o.p_->add_ref();
        if (p_) p_->release();
        p_ = o.p_;
        return *this;
    }
    Ref_ptr& operator=(Ref_ptr&& o) noexcept
    {
        if (this != &o)
        {
            if (p_) p_->release();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ~Ref_ptr() { if (p_) p_->release(); }

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept { if (p_) p_->release(); p_ = nullptr; }

    friend bool operator==(const Ref_ptr&, const Ref_ptr&) noexcept = default;

private:
    T* p_ = nullptr;
};

// Allocate a `T` (deriving `Ref_counted<T>`) and return the owning handle (refcount 1).
template<typename T, typename... Args>
Ref_ptr<T> make_ref(Args&&... args)
{
    return Ref_ptr<T>(new T(std::forward<Args>(args)...));
}

} // namespace ts::detail
