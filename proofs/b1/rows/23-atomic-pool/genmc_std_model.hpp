#pragma once

// Environment models for row 23 under GenMC: the host mutex, condition
// variable, thread and locks the pool is written over (std::mutex and
// friends, whose members live in libc++'s dylib, which GenMC does not
// load), built on the pthread primitives GenMC models, and the global
// allocation functions over malloc and free.
//
// Semantics kept: mutual exclusion and the happens-before a mutex gives;
// thread start and join. Semantics given up, on purpose: a condition
// variable wait is an immediate spurious wake-up (unlock, then lock again)
// and a notification does nothing. Every wait in thread_pool.cpp is in a
// loop that re-checks its condition, so safety properties are unaffected;
// liveness (that a worker sleeps until notified) is not checked.
//
// Nothing here throws: the substitutions in treatment.cpp remove the try
// and catch blocks, and the run passes -fno-exceptions.

#include <pthread.h>
#include <stdlib.h>

#include <cstddef>
#include <mutex>
#include <new>

namespace stx_model {

class mutex final {
public:
    mutex() noexcept { pthread_mutex_init(&handle_, nullptr); }
    ~mutex() { pthread_mutex_destroy(&handle_); }
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock() noexcept { pthread_mutex_lock(&handle_); }
    void unlock() noexcept { pthread_mutex_unlock(&handle_); }

private:
    pthread_mutex_t handle_;
};

class unique_lock final {
public:
    unique_lock() noexcept = default;
    explicit unique_lock(mutex& held) noexcept : mutex_(&held) {
        held.lock();
        owns_ = true;
    }
    unique_lock(mutex& held, std::defer_lock_t) noexcept : mutex_(&held) {}
    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;
    unique_lock(unique_lock&& other) noexcept : mutex_(other.mutex_), owns_(other.owns_) {
        other.mutex_ = nullptr;
        other.owns_ = false;
    }
    unique_lock& operator=(unique_lock&& other) noexcept {
        if (owns_) (*mutex_).unlock();
        mutex_ = other.mutex_;
        owns_ = other.owns_;
        other.mutex_ = nullptr;
        other.owns_ = false;
        return *this;
    }
    ~unique_lock() {
        if (owns_) (*mutex_).unlock();
    }

    void lock() noexcept {
        (*mutex_).lock();
        owns_ = true;
    }
    void unlock() noexcept {
        (*mutex_).unlock();
        owns_ = false;
    }

private:
    mutex* mutex_{nullptr};
    bool owns_{false};
};

class lock_guard final {
public:
    explicit lock_guard(mutex& held) noexcept : mutex_(held) { mutex_.lock(); }
    ~lock_guard() { mutex_.unlock(); }
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    mutex& mutex_;
};

class condition_variable final {
public:
    condition_variable() noexcept = default;
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    // An immediate spurious wake-up: the hold is released and taken again.
    void wait(unique_lock& hold) noexcept {
        hold.unlock();
        hold.lock();
    }
    void notify_one() noexcept {}
    void notify_all() noexcept {}
};

template <typename Function, typename Argument>
struct ThreadStart final {
    Function function;
    Argument argument;
};

template <typename Function, typename Argument>
void* thread_trampoline(void* const address) {
    ThreadStart<Function, Argument>* const start = static_cast<ThreadStart<Function, Argument>*>(address);
    (*start).function((*start).argument);
    delete start;
    return nullptr;
}

class thread final {
public:
    class id final {
    public:
        id() noexcept = default;
        explicit id(const pthread_t handle) noexcept : handle_(handle), named_(true) {}
        [[nodiscard]] bool operator==(const id& other) const noexcept {
            if (!named_ || !other.named_) return named_ == other.named_;
            const bool result = pthread_equal(handle_, other.handle_) != 0;
            return result;
        }

    private:
        pthread_t handle_{};
        bool named_{false};
    };

    thread() noexcept = default;
    template <typename Function, typename Argument>
    thread(Function function, Argument argument) {
        ThreadStart<Function, Argument>* const start = new ThreadStart<Function, Argument>{function, argument};
        pthread_create(&handle_, nullptr, &thread_trampoline<Function, Argument>, start);
        joinable_ = true;
    }
    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;
    thread(thread&& other) noexcept : handle_(other.handle_), joinable_(other.joinable_) { other.joinable_ = false; }
    thread& operator=(thread&& other) noexcept {
        if (joinable_) abort();
        handle_ = other.handle_;
        joinable_ = other.joinable_;
        other.joinable_ = false;
        return *this;
    }
    ~thread() {
        if (joinable_) abort();
    }

    void join() noexcept {
        pthread_join(handle_, nullptr);
        joinable_ = false;
    }
    [[nodiscard]] id get_id() const noexcept {
        if (!joinable_) return id();
        const id result(handle_);
        return result;
    }

private:
    pthread_t handle_{};
    bool joinable_{false};
};

inline thread::id this_thread_id() noexcept {
    const thread::id result(pthread_self());
    return result;
}

} // namespace stx_model

// The global allocation functions (libc++abi's, not loaded under GenMC).
void* operator new(std::size_t size) {
    void* block = malloc(size == 0 ? 1 : size);
    if (block == nullptr) abort();
    return block;
}
void* operator new[](std::size_t size) {
    void* block = malloc(size == 0 ? 1 : size);
    if (block == nullptr) abort();
    return block;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return malloc(size == 0 ? 1 : size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return malloc(size == 0 ? 1 : size); }
void operator delete(void* block) noexcept { free(block); }
void operator delete[](void* block) noexcept { free(block); }
// No top-level const on the parameters: clang 22 rejected the sized
// delete as a non-usual deallocation function in libc++'s
// __builtin_operator_delete with `const std::size_t` (census-1). The sized
// forms only where the compiler uses sized deallocation.
#ifndef STX_MODEL_SIZED_DELETE
#  if defined(__cpp_sized_deallocation)
#    define STX_MODEL_SIZED_DELETE 1
#  else
#    define STX_MODEL_SIZED_DELETE 0
#  endif
#endif
#if STX_MODEL_SIZED_DELETE
void operator delete(void* block, std::size_t) noexcept { free(block); }
void operator delete[](void* block, std::size_t) noexcept { free(block); }
#endif
