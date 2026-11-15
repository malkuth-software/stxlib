// Per-thread records (include/stxlib/thread_records.hpp; notes
// planning/stages/E-thread-records.md): the public calls over
// stxlib_state's exports (state.hpp), which own the records, their memory
// and the thread-exit hook. This file turns stxlib_state's statuses into
// error records.

#include "stxlib/thread_records.hpp"

#include "state/state.hpp"

namespace stxlib {

static_assert(thread_record_type_capacity == state::thread_record_type_capacity, "the record limits agree");
static_assert(thread_record_size_limit == state::thread_record_size_limit, "the record limits agree");
static_assert(thread_record_alignment_limit == state::thread_record_alignment_limit, "the record limits agree");

namespace {

ErrorRecord failure_of(const state::RecordStatus status, const Operation operation) noexcept {
    switch (status) {
    case state::RecordStatus::layout_mismatch:
        state::refuse_layout(state::layout_refusal);
    case state::RecordStatus::invalid_argument:
        return ErrorRecord::make(
            ErrorKind::invalid_argument, operation,
            ErrorDetail{.message = "a record type out of range, an unknown slot, or a call from its own initialize"});
    case state::RecordStatus::limit_exceeded:
        return ErrorRecord::make(ErrorKind::limit_exceeded, operation,
                                 ErrorDetail{.message = "thread_record_type_capacity record types exist"});
    case state::RecordStatus::out_of_memory:
        return ErrorRecord::make(ErrorKind::out_of_memory, operation,
                                 ErrorDetail{.message = "the thread's record or its block could not be allocated"});
    case state::RecordStatus::disposed:
        return ErrorRecord::make(ErrorKind::disposed, operation,
                                 ErrorDetail{.message = "this thread's records or the process are ending or ended"});
    case state::RecordStatus::no_hook:
    case state::RecordStatus::ok:
        break;
    }
    return ErrorRecord::make(ErrorKind::unsupported, operation,
                             ErrorDetail{.message = "the host gave no thread-exit hook"});
}

} // namespace

Result<ThreadRecordSlot> register_thread_record(const ThreadRecordType& type) noexcept {
    const state::ThreadRecordTypeEntry entry{.size = type.size,
                                             .alignment = type.alignment,
                                             .initialize = type.initialize,
                                             .release = type.release,
                                             .context = type.context};
    u32 slot = 0;
    const state::RecordStatus status = state::thread_record_register(state::layout_key, &entry, &slot);
    if (status != state::RecordStatus::ok) {
        return Result<ThreadRecordSlot>::failure(failure_of(status, "stxlib.thread_records.register"));
    }
    return Result<ThreadRecordSlot>::success(ThreadRecordSlot::from_index(slot));
}

Result<void*> thread_record(const ThreadRecordSlot slot) noexcept {
    void* record = nullptr;
    const state::RecordStatus status = state::thread_record_get(state::layout_key, slot.index(), &record);
    if (status != state::RecordStatus::ok) {
        return Result<void*>::failure(failure_of(status, "stxlib.thread_records.get"));
    }
    return Result<void*>::success(record);
}

void* existing_thread_record(const ThreadRecordSlot slot) noexcept {
    void* const result = state::thread_record_existing(state::layout_key, slot.index());
    return result;
}

void end_thread_records() noexcept { state::thread_records_end(state::layout_key); }

} // namespace stxlib
