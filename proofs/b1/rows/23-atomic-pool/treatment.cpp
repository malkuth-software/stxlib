// Row 23: AtomicPool begin and wait with run_worker, publish_batch,
// valid_ticket, create and the pool's destruction (B1.md section 3.1; the
// whole file is lifted, so no constant is changed: begin always takes the
// worker path, where run would run small batches on the caller).
// P3, under RC11 and IMM (GenMC 0.20.0): every task index runs exactly once
//     per batch; wait returns only after every task finished and its write
//     is visible to the caller; no data race (on pool.tasks or on the
//     tasks' cells); `pending` never underflows (the assert at line 122).
// Environment (recorded): std::mutex, std::condition_variable, std::thread
//     and their locks are replaced by genmc_std_model.hpp's pthread models
//     (the substitutions below); a condition-variable wait is an immediate
//     spurious wake-up (sound for these safety properties; it gives up
//     liveness), notifications do nothing; the global allocation functions
//     are malloc and free. try and catch are removed because the models
//     never throw and GenMC runs without C++ exceptions.
// Bound: 2 workers; two consecutive batches of STX_BATCH tasks, STX_BATCH
//        in {1, 2, 3, 4}; loops unrolled to the run's unwind.
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <span>

//@ model-begin reason="pthread models of std::mutex, std::condition_variable, std::thread and the allocation functions (genmc_std_model.hpp, beside this file)"
#include "genmc_std_model.hpp"
//@ model-end

#include "stx_auxlib/thread_pool.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/thread_pool.hpp sha256=79df0f5650f50ad65e4daf78bbe863772b5ccf824faf5814924c6ad34bd227c8
//@ lift source=stx_auxlib path=src/hosted/thread_pool.cpp lines=1-330 sha256=3eafb5f2688e12a5e89e15140d90b80abf55dc2dd49b9d37871f975435575270
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=40 from="std::mutex" to="stx_model::mutex" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=41 from="std::mutex" to="stx_model::mutex" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=42 from="std::condition_variable" to="stx_model::condition_variable" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=43 from="std::condition_variable" to="stx_model::condition_variable" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=44 from="std::thread[]" to="stx_model::thread[]" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=57 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=72 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=73 from="try {" to="{" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=75 from="} catch (const std::system_error&) {" to="} if (false) {" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=83 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=132 from="std::lock_guard<std::mutex>" to="stx_model::lock_guard" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=169 from="try {" to="{" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=170 from="std::lock_guard<std::mutex>" to="stx_model::lock_guard" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=172 from="} catch (const std::system_error&) {" to="} if (false) {" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=197 from="try {" to="{" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=199 from="} catch (const std::system_error&) {" to="} if (false) {" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=204 from="std::thread[" to="stx_model::thread[" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=209 from="try {" to="{" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=211 from="std::thread(" to="stx_model::thread(" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=212 from="} catch (const std::system_error&) {" to="} if (false) {" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=213 from="} catch (const std::bad_alloc&) {" to="} if (false) {" reason="exceptions removed: the modelled mutex and thread never throw, and GenMC runs without C++ exceptions; each handler body is kept, unreachable"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=237 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=248 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=263 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=266 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=278 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=286 from="std::unique_lock<std::mutex>" to="stx_model::unique_lock" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ subst source=stx_auxlib path=src/hosted/thread_pool.cpp line=293 from="std::thread::id caller = std::this_thread::get_id()" to="stx_model::thread::id caller = stx_model::this_thread_id()" reason="the host mutex, condition variable and thread, modelled over pthreads for GenMC (genmc_std_model.hpp)"
//@ mutant id=m1 source=stx_auxlib path=src/hosted/thread_pool.cpp line=121 from="std::memory_order_release" to="std::memory_order_relaxed"

namespace {

constexpr std::uint32_t batch_size = STX_BATCH;
int cells[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};

void task(void* const argument) {
    int* const cell = static_cast<int*>(argument);
    *cell += 1;
}

} // namespace

int main() {
    stx_auxlib::PoolCreation creation = stx_auxlib::AtomicPool::create(2);
    STX_ASSERT(creation.succeeded(), "the pool starts");
    stx_auxlib::AtomicPool pool = creation.take_pool();
    for (std::size_t batch = 0; batch < 2; ++batch) {
        stx_auxlib::PoolTask tasks[4] = {};
        for (std::uint32_t index = 0; index < batch_size; ++index) {
            tasks[index].function = task;
            tasks[index].argument = &cells[batch][index];
        }
        const stx_auxlib::PoolResult begun = pool.begin(std::span<const stx_auxlib::PoolTask>(tasks, batch_size));
        STX_ASSERT(begun == stx_auxlib::PoolResult::ok, "begin accepts the batch");
        const stx_auxlib::PoolResult waited = pool.wait();
        STX_ASSERT(waited == stx_auxlib::PoolResult::ok, "wait succeeds");
        for (std::uint32_t index = 0; index < batch_size; ++index)
            STX_ASSERT(cells[batch][index] == 1, "P3: each task ran exactly once and its write is visible after wait");
    }
    STX_VACUITY_PROBE();
    return 0;
}
