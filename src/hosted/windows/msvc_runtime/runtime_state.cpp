// The MSVC C++ runtime's per-thread state (runtime_state.hpp).
//
// Two TLS slots, allocated once per runtime instance on first use: the
// uncaught-exception count (stored as a pointer-sized integer) and the
// innermost active CatchRecord. TlsAlloc, TlsGetValue and TlsSetValue are
// documented on Microsoft Learn ("Using Thread Local Storage", and each
// function's page); TlsGetValue returns zero for a slot the thread never
// set, which is the empty state of both. Slot allocation is published with
// one compare-exchange; the loser frees its slot. The slots are never
// freed: the runtime lives as long as the process (shared form) or its
// module (static form), and an unloaded module's slots are a bounded leak
// of two indexes.
//
// Why not thread_local: a thread_local needs the PE TLS directory that the
// startup objects supply (_tls_used, _tls_index), and this runtime must
// work before, and without, any particular startup code.

#include "runtime_state.hpp"

#include <atomic>

namespace stxlib::msvc_runtime {

namespace {

constexpr DWORD no_slot = TLS_OUT_OF_INDEXES;

static_assert(std::atomic<DWORD>::is_always_lock_free, "atomic<DWORD> must be lock-free");

struct Slots final {
    std::atomic<DWORD> uncaught;
    std::atomic<DWORD> innermost;
};

constinit Slots slots{no_slot, no_slot};

DWORD allocate_slot(std::atomic<DWORD>& slot) noexcept {
    DWORD current = slot.load(std::memory_order_acquire);
    if (current != no_slot) return current;
    const DWORD fresh = TlsAlloc();
    if (fresh == TLS_OUT_OF_INDEXES) fail_fast();
    DWORD expected = no_slot;
    const bool won = slot.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel);
    if (won) return fresh;
    TlsFree(fresh);
    return expected;
}

} // namespace

i32 uncaught_count() noexcept {
    const DWORD slot = allocate_slot(slots.uncaught);
    const uptr stored = reinterpret_cast<uptr>(TlsGetValue(slot));
    const i32 result = static_cast<i32>(static_cast<i64>(stored));
    return result;
}

void add_uncaught(const i32 change) noexcept {
    const DWORD slot = allocate_slot(slots.uncaught);
    const uptr stored = reinterpret_cast<uptr>(TlsGetValue(slot));
    const i64 updated = static_cast<i64>(stored) + change;
    TlsSetValue(slot, reinterpret_cast<void*>(static_cast<uptr>(updated)));
}

CatchRecord* innermost_catch() noexcept {
    const DWORD slot = allocate_slot(slots.innermost);
    CatchRecord* const result = static_cast<CatchRecord*>(TlsGetValue(slot));
    return result;
}

void set_innermost_catch(CatchRecord* const record) noexcept {
    const DWORD slot = allocate_slot(slots.innermost);
    TlsSetValue(slot, record);
}

bool object_still_handled(const void* const object, const CatchRecord* const except) noexcept {
    if (object == nullptr) return false;
    const CatchRecord* record = innermost_catch();
    while (record != nullptr) {
        if (record != except && is_cxx_exception((*record).exception) && exception_object((*record).exception) == object) {
            return true;
        }
        record = (*record).outer;
    }
    return false;
}

// --- HandledException --------------------------------------------------------

namespace {

bool has_cxx_shape(const u32 code, const u32 parameter_count, const ULONG_PTR magic) noexcept {
    if (code != cxx_exception_code) return false;
    if (parameter_count < 3) return false;
    const bool result = magic >= cxx_exception_magic && magic <= cxx_exception_magic_newest;
    return result;
}

} // namespace

bool is_cxx_exception(const EXCEPTION_RECORD& record) noexcept {
    const ULONG_PTR magic = record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0;
    const bool result = has_cxx_shape(record.ExceptionCode, record.NumberParameters, magic);
    return result;
}

bool is_cxx_exception(const HandledException& exception) noexcept {
    const ULONG_PTR magic = exception.parameter_count > 0 ? exception.parameters[0] : 0;
    const bool result = has_cxx_shape(exception.code, exception.parameter_count, magic);
    return result;
}

void* exception_object(const HandledException& exception) noexcept {
    void* const result = reinterpret_cast<void*>(exception.parameters[1]);
    return result;
}

const ThrowInfo* exception_throw_info(const HandledException& exception) noexcept {
    const ThrowInfo* const result = reinterpret_cast<const ThrowInfo*>(exception.parameters[2]);
    return result;
}

uptr exception_image_base(const HandledException& exception) noexcept {
    if (exception.parameter_count < cxx_exception_parameters) return 0;
    const uptr result = static_cast<uptr>(exception.parameters[3]);
    return result;
}

void copy_handled_exception(const EXCEPTION_RECORD& record, HandledException& out) noexcept {
    out.code = record.ExceptionCode;
    out.flags = record.ExceptionFlags;
    u32 count = record.NumberParameters;
    if (count > EXCEPTION_MAXIMUM_PARAMETERS) count = EXCEPTION_MAXIMUM_PARAMETERS;
    out.parameter_count = count;
    for (u32 index = 0; index < EXCEPTION_MAXIMUM_PARAMETERS; ++index) {
        const ULONG_PTR value = index < count ? record.ExceptionInformation[index] : 0;
        out.parameters[index] = value;
    }
}

} // namespace stxlib::msvc_runtime
