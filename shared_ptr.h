#pragma once

#include <iostream>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <functional>
#include <future>
#include <utility>
#include <new>
#include <malloc/malloc.h>
using std::cout;
using std::endl;
using namespace std::chrono_literals;
template<class T>
class Display;

template<class T>
struct DefaultDeleter {
    void operator()(T* p) {
        delete p;
    }
};

template<class T>
struct DefaultDeleter<T[]> {
    void operator()(T* p) {
        delete[] p;
    }
};

struct ControlBlock {
    std::atomic<size_t> mCount;
    ControlBlock() : mCount(1) {}
    ControlBlock(ControlBlock&&) = delete;
    virtual ~ControlBlock() = default;
    void incref() {
        mCount.fetch_add(1, std::memory_order_relaxed);
    }
    void decref() {
        if (mCount.fetch_sub(1, std::memory_order_relaxed) == 1) {
            delete this;
        }
    }
    size_t getCount() {
        return mCount.load(std::memory_order_relaxed);
    }
};

template<class T, class Deleter = DefaultDeleter<T>>
struct ControlBlockImpl : ControlBlock {
    T* m_ptr;
    Deleter mDeleter;
    explicit ControlBlockImpl(T* value) : m_ptr(value), mDeleter(DefaultDeleter<T>{}) {}
    explicit ControlBlockImpl(T* value, Deleter deleter) : m_ptr(value), mDeleter(deleter) {}
    ~ControlBlockImpl() override{
        mDeleter(m_ptr);
    }
};

template<class T, class Deleter = DefaultDeleter<T>>
struct ControlBlockImplFused : ControlBlock {
    T* m_ptr;
    Deleter mDeleter;
    void* m_mem;
    explicit ControlBlockImplFused(T* ptr, void* mem) :
        m_ptr(ptr), m_mem(mem), mDeleter(DefaultDeleter<T>()) {}
    explicit ControlBlockImplFused(T* ptr, void* mem, Deleter deleter) :
        m_ptr(ptr), m_mem(mem), mDeleter(deleter) {}
    ~ControlBlockImplFused() override {
        mDeleter(m_ptr);
    }

    void operator delete(void* p) {
        ::operator delete(
            p, std::align_val_t(std::max(alignof(T), alignof(ControlBlockImplFused)))
        );
    }
};


template<class T>
struct enable_shared_from_this;
template<class T>
void setUp_shared_from_thisControl(enable_shared_from_this<T>*, ControlBlock*);
template<class T>
    requires(std::is_base_of_v<enable_shared_from_this<T>, T>)
void setUp_shared_from_this(T*, ControlBlock*);


template<class T>
class shared_ptr {
private:
    ControlBlock* m_cb; //CRTP,使用基类指针即可, 能够存储任意类型的派生类.
    T* m_ptr;

    // 同一个类型能够访问private, 但是模板特化之后就不算同一个类型, 需要添加友元类.
    template<class U>
    friend class shared_ptr;
public:
    using element_type = T;
    using pointer = T*;

    shared_ptr(std::nullptr_t = nullptr) : m_cb(nullptr), m_ptr(nullptr) {}

    //is_convertible_v 类似于 static_cast, 要求是安全的类型转换.
    //例如: int* 运行时就不能转换成 long long*
    //这并不是两者相互能够转换, Y*能够转换成T*即可.
    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    explicit shared_ptr(Y* ptr) : m_ptr(ptr), m_cb(new ControlBlockImpl{ptr}) {
        setUp_shared_from_this(m_ptr, m_cb);
    }

    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    explicit shared_ptr(Y* ptr, ControlBlock* cb) : m_ptr(ptr), m_cb(cb) {
        setUp_shared_from_this(m_ptr, m_cb);
    }

    template<class Y, class Deleter>
        requires(std::is_convertible_v<Y*, T*>)
    explicit shared_ptr(Y* ptr, Deleter deleter)
    : m_ptr(ptr), m_cb{new ControlBlockImpl{ptr, deleter}} {
        setUp_shared_from_this(m_ptr, m_cb);
    }
    //拷贝构造
    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    shared_ptr(const shared_ptr<Y>& other) : m_ptr(other.m_ptr), m_cb(other.m_cb) {
        m_cb->incref();
    }
    //移动构造
    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    shared_ptr(shared_ptr<Y>&& other) : m_ptr(other.m_ptr), m_cb(other.m_cb) {
        other.m_cb = nullptr;
        other.m_ptr = nullptr;
    }
    //拷贝赋值
    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    shared_ptr<T>& operator=(const shared_ptr<Y>& other) {
        if (other == *this) return *this;
        reset();
        m_cb = other.m_cb;
        m_ptr = other.m_ptr;
        m_cb->incref();
    }
    //移动赋值
    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    shared_ptr<T>& operator=(shared_ptr<Y>&& other) {
        if (other == *this) return *this;
        reset();
        m_cb = other.m_cb;
        m_ptr = other.m_ptr;
        other.m_cb = nullptr;
        other.m_ptr = nullptr;
    }

    template<class U>
    shared_ptr(const shared_ptr<U>& other, T* ptr) : m_ptr(ptr), m_cb(other.m_cb) {
        m_cb->incref();
    }

    template<class U>
    shared_ptr(shared_ptr<U>&& other, T* ptr) : m_ptr(ptr), m_cb(other.m_cb) {
        other.m_cb = nullptr;
        other.m_ptr = nullptr;
    }

    ~shared_ptr() {
        if (m_cb) m_cb->decref();
    }

    //将shared_ptr置空
    void reset() {
        m_cb->decref();
        m_ptr = nullptr;
        m_cb = nullptr;
    }

    template<class Y>
    void reset(Y* ptr) {
        m_cb->decref();
        m_ptr = ptr;
        m_cb = new ControlBlockImpl{ptr};
        setUp_shared_from_this(m_ptr, m_cb);
    }

    template<class Y, class Deleter>
    void reset(Y* ptr, Deleter deleter) {
        m_cb->decref();
        m_cb = nullptr;
        m_ptr = nullptr;
        m_ptr = ptr;
        m_cb = new ControlBlockImpl{ptr, deleter};
        setUp_shared_from_this(m_ptr, m_cb);
    }

    operator T*() const {
        return m_ptr;
    }

    T* get() const{
        return m_ptr;
    }

    T* operator->() const {
        return m_ptr;
    }

    size_t use_count() const {
        return m_cb ? m_cb->getCount() : 0;
    }

    bool unique() const noexcept {
        return m_cb ? m_cb->getCount() == 1 : true;
    }

    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    void swap(shared_ptr<Y>& other) {
        std::swap(m_ptr, other.m_ptr);
        std::swap(m_cb, other.m_cb);
    }

    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    bool operator<(const shared_ptr<Y>& other) const noexcept {
        return m_ptr < other.m_ptr;
    }

    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    bool operator==(const shared_ptr<Y>& other) const noexcept {
            return m_ptr == other.m_ptr;
    }

    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    bool owner_before(const shared_ptr<Y>& other) {
        return m_cb < other.m_cb;
    }

    template<class Y>
        requires(std::is_convertible_v<Y*, T*>)
    bool owner_equal(const shared_ptr<Y>& other) {
        return m_cb == other.m_cb;
    }

    T& operator*() const {
        return *m_ptr;
    }
};

//对数组进行偏特化
template<class T>
class shared_ptr<T[]> : shared_ptr<T>{
public:
    using shared_ptr<T>::shared_ptr;

    std::add_lvalue_reference_t<T> operator[](size_t index) {
        return this->get()[index];
    }
};

template<class T>
shared_ptr<T> make_shared_fused(T* ptr, ControlBlock* m_cb) {
    return shared_ptr<T>(ptr, m_cb);
}

template<class T>
struct enable_shared_from_this {
private:
    ControlBlock* m_cb;  //让m_cb控制块的计数器+1就可以避免重复释放了
protected:
    enable_shared_from_this() = default;
    shared_ptr<T> shared_from_this() {
        if (!m_cb) throw std::bad_weak_ptr{};
        m_cb->incref();
        return make_shared_fused<T>(static_cast<T*>(this), m_cb);
    }
    shared_ptr<const T> shared_from_this() const {
        if (!m_cb) throw std::bad_weak_ptr{};
        m_cb->incref();
        return make_shared_fused<T>(static_cast<const T*>(this), m_cb);
    }
    template<class U>
    friend void setUp_shared_from_thisControl(enable_shared_from_this<U>*, ControlBlock*);
};

template<class T>
void setUp_shared_from_thisControl(enable_shared_from_this<T>* ptr, ControlBlock* cb) {
    ptr->m_cb = cb;
}

template<class T>
    requires(std::is_base_of_v<enable_shared_from_this<T>, T>)
void setUp_shared_from_this(T* ptr, ControlBlock* m_cb) {
    setUp_shared_from_thisControl(static_cast<enable_shared_from_this<T>*>(ptr), m_cb);
}

template<class T, class... Args>
    requires(!std::is_unbounded_array_v<T>)
shared_ptr<T> make_shared(Args&&... args) {
    const auto deleter = [](T* p) {
        p->~T();
    };
    using __counter = ControlBlockImplFused<T, decltype(deleter)>;
    //要求给controlBlock分配的内存空间, 一定要是alignof(T)的倍数, 否则T的起始地址就没法内存对齐
    size_t __offset = alignof(T);
    while (__offset < sizeof(__counter)) __offset += alignof(T);
    size_t __align = std::max(alignof(T), alignof(__counter));
    size_t __size = __offset + sizeof(T);
    void* mem = ::operator new(__size, std::align_val_t(__align));
    __counter* cb = reinterpret_cast<__counter*>(mem);
    T* p = reinterpret_cast<T*>(reinterpret_cast<char*>(mem) + __offset);
    new (p) T{std::forward<Args>(args)...};
    new (cb) __counter(p, mem, deleter);
    setUp_shared_from_this(p, cb);
    return make_shared_fused(p, cb);
}

template<class T>
    requires(!std::is_unbounded_array_v<T>)
shared_ptr<T> make_shared_for_overwrite() {
    const auto deleter = [](T* p) {
        p->~T();
    };
    using __counter = ControlBlockImplFused<T, decltype(deleter)>;
    size_t __offset = alignof(T);
    while (__offset < sizeof(__counter)) __offset += alignof(T);
    size_t __align = std::max(alignof(T), alignof(__counter));
    size_t __size = __offset + sizeof(T);
    void* mem = ::operator new(__size, std::align_val_t(__align));
    __counter* cb = reinterpret_cast<__counter*>(mem);
    T* p = reinterpret_cast<T*>(reinterpret_cast<char*>(mem) + __offset);
    new (cb) __counter(p, mem, deleter);
    setUp_shared_from_this(p, cb);
    return make_shared_fused(p, cb);
}

template<class T, class... Args>
    requires(std::is_unbounded_array_v<T>)
shared_ptr<T> make_shared(size_t len, Args&&... args) {
    return shared_ptr<T>(new std::remove_extent_t<T>[len]{std::forward<Args>(args)...});
}

template<class T>
    requires(std::is_unbounded_array_v<T>)
shared_ptr<T> make_shared_for_overwrite() {
    return shared_ptr<T>{new std::remove_extent_t<T>};
}

template<class T, class U>
shared_ptr<T> static_pointer_cast(const shared_ptr<U>& ptr) {
    return shared_ptr<T>(ptr, static_cast<T*>(ptr.get()));
}

template<class T, class U>
shared_ptr<T> const_pointer_cast(const shared_ptr<U>& ptr) {
    return shared_ptr<T>(ptr, const_cast<T*>(ptr.get()));
}

template<class T, class U>
shared_ptr<T> reinterpret_pointer_cast(const shared_ptr<U>& ptr) {
    return shared_ptr<T>(ptr, reinterpret_cast<T*>(ptr.get()));
}

// dynamic_cast需要特判, 它是运行时动态类型转换, 无法在编译期检测错误.
template<class T, class U>
shared_ptr<T> dynamic_pointer_cast(const shared_ptr<U>& ptr) {
    T* p = dynamic_cast<T*>(ptr.get());
    if (p) {
        return shared_ptr<T>(ptr, p);
    } else {
        return nullptr;
    }
}
