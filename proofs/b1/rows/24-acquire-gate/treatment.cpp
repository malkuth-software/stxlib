// Row 24: the call-gate stand-in acquire (src/hosted/thread_pool.cpp:72-79)
// (B1.md section 3.1): the nearest existing code that contains a foreign
// exception and turns it into a value; the row measures each tool's
// exception support.
// P1: no exception leaves the function; false exactly when the foreign
//     lock threw std::system_error; the lock is held exactly when true.
// Environment (recorded): the foreign lock is a model whose lock() returns
//     holding the lock or throws std::system_error, as
//     std::unique_lock<std::mutex>::lock may. The parameter type is
//     substituted for the model, and `noexcept` is dropped with it, so an
//     escaping exception reaches the harness's handler and fails the
//     property instead of calling std::terminate, which the tools model
//     differently.
// Bound: one call; the lock's behaviour nondeterministic.
#include "verify.h"

#include <mutex>
#include <new>
#include <system_error>

//@ model-begin reason="the foreign lock: returns holding the lock, or throws std::system_error"
class HarnessLock final {
public:
    void lock() {
        if (nondet_bool()) {
            threw = true;
            throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
        }
        held = true;
    }
    bool held{false};
    bool threw{false};
};
//@ model-end

//@ lift source=stx_auxlib path=src/hosted/thread_pool.cpp lines=72-79 namespace=stx_auxlib anonymous sha256=f142d7588fe697ddbabfb682d4a4316ebdf6a6c5eab827b59a2847a10cc089c9
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=72 from="std::unique_lock<std::mutex>& lock) noexcept" to="HarnessLock& lock)" reason="the foreign lock model; noexcept dropped so an escaping exception reaches the harness"
//@ mutant id=m1 source=stx_auxlib path=src/hosted/thread_pool.cpp line=75 from="std::system_error" to="std::bad_alloc"

int main() {
    HarnessLock lock{};
    bool escaped = false;
    bool acquired = false;
    try {
        acquired = stx_auxlib::acquire(lock);
    } catch (...) {
        escaped = true;
    }
    STX_ASSERT(!escaped, "P1: no exception leaves acquire");
    STX_ASSERT(acquired == !lock.threw, "P1: false exactly when the lock threw");
    STX_ASSERT(lock.held == acquired, "P1: the lock is held exactly when true");
    STX_VACUITY_PROBE();
    return 0;
}
