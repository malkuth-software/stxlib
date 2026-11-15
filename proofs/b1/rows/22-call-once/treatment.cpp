// Row 22: call_once (include/stx_auxlib/threads.hpp:154-158, inline,
// lifted with OnceFlag and the Mutex and ScopedLock declarations) and call_once_slow (src/hosted/threads.cpp:190-196)
// (B1.md section 3.1).
// P3, under RC11 and IMM (GenMC 0.20.0), with 2 and 3 threads: `entry` runs
//     exactly once; every caller returns after `entry` finished and sees its
//     writes (non-atomic reads after call_once, which GenMC reports as a
//     race if they are not ordered after the writes); no data race.
// Environment (recorded): stx_auxlib::Mutex is written over the host mutex
//     (src/hosted/threads.cpp:141-152, over std::mutex, whose members are in
//     libc++'s dylib); here its members are over pthread_mutex_t, which
//     GenMC models. The row's code is call_once and call_once_slow; the
//     mutex is its environment.
// Bound: STX_THREADS callers, 2 or 3.
#include "verify.h"

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

// threads.hpp is lifted by spans (Mutex, ScopedLock, OnceFlag,
// call_once_slow's declaration and call_once), not included whole: its
// includes (<chrono> through clocks.hpp, <memory>, <variant>) need C library
// and POSIX thread functions that GenMC 0.20's runtime headers do not
// declare (smoke runs in the Ubuntu VM, 2026-11-13). The spans are verbatim.
//@ lift source=stx_auxlib path=include/stx_auxlib/threads.hpp lines=60-76 namespace=stx_auxlib sha256=971f6ad78ea0ffe081e7e29787fbfc90fd7bc7de4f69115a6b00b8d576e50c58
//@ lift source=stx_auxlib path=include/stx_auxlib/threads.hpp lines=80-92 namespace=stx_auxlib sha256=bef362f71e32a684174a9ee8b822d2c0f8d2eb1431857caebd1db36e58571fcf
//@ lift source=stx_auxlib path=include/stx_auxlib/threads.hpp lines=124-158 namespace=stx_auxlib sha256=a2ace00ffaef272a23a5c0d5eddb846492b4f5b0f24d2cf6edfe4c57bbcfe8ed

//@ model-begin reason="stx_auxlib::Mutex members over pthread_mutex_t, in place of threads.cpp:141-152 over std::mutex"
namespace stx_auxlib {
namespace {
pthread_mutex_t* model_mutex(std::byte* const storage) noexcept {
    pthread_mutex_t* const result = std::launder(reinterpret_cast<pthread_mutex_t*>(storage));
    return result;
}
} // namespace
Mutex::Mutex() noexcept {
    ::new (static_cast<void*>(storage_)) pthread_mutex_t;
    pthread_mutex_init(model_mutex(storage_), nullptr);
}
Mutex::~Mutex() { pthread_mutex_destroy(model_mutex(storage_)); }
void Mutex::lock() noexcept { pthread_mutex_lock(model_mutex(storage_)); }
bool Mutex::try_lock() noexcept {
    const bool result = pthread_mutex_trylock(model_mutex(storage_)) == 0;
    return result;
}
void Mutex::unlock() noexcept { pthread_mutex_unlock(model_mutex(storage_)); }
} // namespace stx_auxlib
//@ model-end

//@ lift source=stx_auxlib path=src/hosted/threads.cpp lines=190-196 namespace=stx_auxlib sha256=b9d223bb380aa7bd67d9afa2292ff1cec37ca7d5f285b746def012aa7e590048
//@ mutant id=m1 source=stx_auxlib path=src/hosted/threads.cpp line=195 from="std::memory_order_release" to="std::memory_order_relaxed"

namespace {

struct Shared final {
    stx_auxlib::OnceFlag flag{};
    int runs{0};
    int payload{0};
};

void entry(void* const context) noexcept {
    Shared* const shared = static_cast<Shared*>(context);
    (*shared).runs += 1;
    (*shared).payload = 42;
}

void* caller(void* const argument) {
    Shared* const shared = static_cast<Shared*>(argument);
    stx_auxlib::call_once((*shared).flag, entry, shared);
    STX_ASSERT((*shared).runs == 1, "P3: entry ran once before call_once returned");
    STX_ASSERT((*shared).payload == 42, "P3: the caller sees entry's writes");
    return nullptr;
}

} // namespace

int main() {
    Shared shared{};
    pthread_t threads[STX_THREADS];
    for (std::size_t index = 0; index < STX_THREADS; ++index) pthread_create(&threads[index], nullptr, caller, &shared);
    for (std::size_t index = 0; index < STX_THREADS; ++index) pthread_join(threads[index], nullptr);
    STX_ASSERT(shared.runs == 1, "P3: entry ran exactly once");
    STX_VACUITY_PROBE();
    return 0;
}
