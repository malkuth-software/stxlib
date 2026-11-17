// __C_specific_handler: the language handler for structured exception
// handling in C and C++ (__try with __except or __finally), which clang
// names as the personality of every function holding such a block on this
// target (clang/lib/CodeGen/CGException.cpp, EHPersonality::
// MSVC_C_specific_handler). Exported by vcruntime140.dll and ucrtbase.dll
// in Microsoft's runtime; supplied here so an image built without
// vcruntime needs neither.
//
// Its data (HandlerData; LLVM 22.1.8 llvm/lib/CodeGen/AsmPrinter/
// WinException.cpp, emitCSpecificHandlerTable and emitSEHActionsForRange):
// a 32-bit entry count, then 16-byte entries {BeginAddress, EndAddress,
// HandlerAddress, JumpTarget}, image-relative. An entry covers code from
// BeginAddress up to, not including, EndAddress. For __finally,
// HandlerAddress names the cleanup funclet and JumpTarget is 0; for
// __except, HandlerAddress names the filter function, or is 1 for a filter
// that always accepts, and JumpTarget is the code after the __except where
// the handler body starts. For each code range, LLVM writes the entries of
// every enclosing block, innermost first.
//
// Behaviour (Microsoft Learn, "x64 exception handling", the language
// handler's two calls; "try-except statement" and "try-finally statement"
// for what each block does):
// - Search (no unwind flag): the entries covering ControlPc, innermost
//   first. A __finally entry is skipped. A filter returning
//   EXCEPTION_CONTINUE_EXECUTION (negative) resumes at the fault;
//   EXCEPTION_CONTINUE_SEARCH (0) tries the next entry;
//   EXCEPTION_EXECUTE_HANDLER (positive) starts a target unwind to
//   JumpTarget in this frame, with the exception code as the value the
//   handler body finds in EAX (llvm.eh.exceptioncode reads it there), and
//   never returns.
// - Unwind: each covering __finally runs, innermost first, with abnormal
//   termination TRUE. In the target frame the walk stops at the __except
//   being entered (its JumpTarget is the unwind's TargetIp) or at a block
//   that also covers TargetIp (the continuation lies inside it, so it and
//   every outer block stay active). DISPATCHER_CONTEXT.ScopeIndex is set
//   past each __finally before it is called, so a collided unwind resumes
//   after it rather than running it twice.
//
// The filter is called as LONG filter(EXCEPTION_POINTERS*, establisher
// frame) and the __finally funclet as funclet(abnormal, establisher frame):
// the two arguments clang's outlined filter and finally functions take
// (the frame is how they reach the parent's locals, through
// llvm.eh.recoverfp). Both are foreign code; an exception they raise is
// dispatched as a nested exception by the system.
//
// Compiled without exceptions; it never unwinds itself except through the
// RtlUnwindEx call that enters an __except body.

#include "system_interim.h"

#include "stxlib/types.hpp"

namespace stxlib::msvc_runtime {

namespace {

struct ScopeEntry final {
    u32 begin;
    u32 end;
    u32 handler;
    u32 target;
};

struct ScopeTable final {
    u32 count;
    ScopeEntry entries[1];
};

static_assert(sizeof(ScopeEntry) == 16, "LLVM writes 16-byte scope entries");

// HandlerAddress 1: an __except whose filter is the constant
// EXCEPTION_EXECUTE_HANDLER.
constexpr u32 filter_accepts_all = 1;
// JumpTarget 0: a __finally.
constexpr u32 no_target = 0;
constexpr u64 abnormal_termination = 1;

using Filter = LONG (*)(EXCEPTION_POINTERS* pointers, u64 establisher);
using Termination = void (*)(u64 abnormal, u64 establisher);

bool covers(const ScopeEntry& entry, const u32 offset) {
    const bool inside = offset >= entry.begin && offset < entry.end;
    return inside;
}

EXCEPTION_DISPOSITION search(EXCEPTION_RECORD& record, CONTEXT& context, const DISPATCHER_CONTEXT& dispatcher, const u64 establisher) {
    const ScopeTable& table = *static_cast<const ScopeTable*>(dispatcher.HandlerData);
    const u64 image_base = dispatcher.ImageBase;
    const u32 pc = static_cast<u32>(dispatcher.ControlPc - image_base);
    EXCEPTION_POINTERS pointers{&record, &context};
    for (u32 index = 0; index < table.count; ++index) {
        const ScopeEntry& entry = table.entries[index];
        if (!covers(entry, pc)) continue;
        if (entry.target == no_target) continue;
        LONG verdict = EXCEPTION_EXECUTE_HANDLER;
        if (entry.handler != filter_accepts_all) {
            const Filter filter = reinterpret_cast<Filter>(image_base + entry.handler);
            verdict = filter(&pointers, establisher);
        }
        if (verdict < 0) return ExceptionContinueExecution;
        if (verdict == 0) continue;
        // Enters the __except body; RtlUnwindEx uses the context as scratch
        // and does not return here.
        CONTEXT unwind_context{};
        void* const continuation = reinterpret_cast<void*>(image_base + entry.target);
        void* const code = reinterpret_cast<void*>(static_cast<uptr>(record.ExceptionCode));
        RtlUnwindEx(reinterpret_cast<void*>(establisher), continuation, &record, code, &unwind_context, dispatcher.HistoryTable);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION unwind(const EXCEPTION_RECORD& record, DISPATCHER_CONTEXT& dispatcher, const u64 establisher) {
    const ScopeTable& table = *static_cast<const ScopeTable*>(dispatcher.HandlerData);
    const u64 image_base = dispatcher.ImageBase;
    const u32 pc = static_cast<u32>(dispatcher.ControlPc - image_base);
    const u32 target = static_cast<u32>(dispatcher.TargetIp - image_base);
    const bool target_frame = (record.ExceptionFlags & EXCEPTION_TARGET_UNWIND) != 0;
    for (u32 index = dispatcher.ScopeIndex; index < table.count; ++index) {
        const ScopeEntry& entry = table.entries[index];
        if (!covers(entry, pc)) continue;
        if (target_frame) {
            if (covers(entry, target)) break;
            if (entry.target != no_target && entry.target == target) break;
        }
        if (entry.target != no_target) continue;
        dispatcher.ScopeIndex = index + 1;
        const Termination termination = reinterpret_cast<Termination>(image_base + entry.handler);
        termination(abnormal_termination, establisher);
    }
    return ExceptionContinueSearch;
}

} // namespace

} // namespace stxlib::msvc_runtime

extern "C" EXCEPTION_DISPOSITION __C_specific_handler(EXCEPTION_RECORD* const record, void* const establisher_frame,
                                                     CONTEXT* const context, void* const dispatcher_context) {
    DISPATCHER_CONTEXT& dispatcher = *static_cast<DISPATCHER_CONTEXT*>(dispatcher_context);
    const stxlib::u64 establisher = reinterpret_cast<stxlib::u64>(establisher_frame);
    const DWORD unwinding = (*record).ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND);
    EXCEPTION_DISPOSITION result = ExceptionContinueSearch;
    if (unwinding == 0) {
        result = stxlib::msvc_runtime::search(*record, *context, dispatcher, establisher);
    } else {
        result = stxlib::msvc_runtime::unwind(*record, dispatcher, establisher);
    }
    return result;
}

static_assert(__is_same(decltype(&__C_specific_handler), PEXCEPTION_ROUTINE));
