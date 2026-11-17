// __CxxFrameHandler3: the language handler clang names for every function
// with C++ exception tables on x86_64-pc-windows-msvc (clang/lib/CodeGen/
// CGException.cpp: MSVC_CxxFrameHandler3; clang emits no other format).
//
// The operating system calls it twice per frame (Microsoft Learn, "x64
// exception handling", "Language-specific handler"): first while searching
// for a handler (no EXCEPTION_UNWINDING flag), then while unwinding
// (EXCEPTION_UNWINDING, plus EXCEPTION_TARGET_UNWIND for the frame that
// catches). The DISPATCHER_CONTEXT gives the frame's image base, its
// RUNTIME_FUNCTION, its establisher frame, the PC within it and
// HandlerData, which holds the 32-bit image-relative reference to the
// FunctionInfo (WinException.cpp: endFuncletImpl writes `$cppxdata$` after
// .seh_handlerdata, for the parent and each catch funclet, never for a
// cleanup funclet).
//
// The design, in execution order.
// 1. Search. The frame's state is the IP-to-state map's entry for the PC,
//    unless one of the frame's catches is running, in which case the state
//    is kept in the frame's override slot (below). Try blocks whose body
//    holds the state are tried innermost first; a catch funclet's frame
//    tries only blocks nested inside its own handler (try_low above its
//    catch state), and leaves outer blocks to the parent frame.
// 2. A match unwinds the frames below with RtlUnwindEx (winnt.h; Microsoft
//    Learn, "RtlUnwindEx function"), each frame's handler running its
//    cleanups (the unwind map's actions) down to its empty state, and the
//    catching frame's down to the try block's own state.
// 3. The unwind passes an exception record with code
//    STATUS_UNWIND_CONSOLIDATE (winnt.h, 0x80000029) whose first parameter
//    is a callback. At the end of the unwind the system calls the callback
//    on the current, still deep, stack and continues the catching frame at
//    the address the callback returns, as though the frames between were
//    gone. That callback, run_catch, builds the catch object, runs the catch
//    funclet (whose return value is the continuation), and destroys the
//    exception object, while the throwing frame, which holds the object,
//    is still intact below. This use of frame consolidation is general
//    knowledge of the Windows unwinder (the code is in winnt.h; Microsoft
//    does not document the callback); it is confirmed by the conformance
//    suite (tests/msvc_runtime), whose nested and rethrow cases fail
//    without it.
// 4. While a catch runs, its frame's state is the catch state (try_high +
//    1, shared by the block's handlers). It is kept per frame instance:
//    for a parent frame in its UnwindHelp slot (the 8-byte slot whose
//    offset the FunctionInfo gives and which the prologue sets to -2), for
//    a catch funclet's frame in the CatchRecord of the thunk that called
//    that funclet (CatchRecord::callee_state), reached through the parent
//    frame pointer the funclet homes (HandlerType::parent_frame_offset;
//    X86FrameLowering.cpp, getWinEHParentFrameOffset). The slot is reset
//    when the catch finishes normally, and stays while an exception leaves
//    the catch, so the frame's handler unwinds from the catch state.
//
// Foreign code (cleanup and catch funclets, copy constructors, destructors)
// runs only through the thunks in thunks_x64.S, whose guards turn an
// exception leaving a cleanup or a copy into std::terminate
// ([except.terminate]) and end an active catch that an exception leaves.

#include "runtime_state.hpp"

#include "stxlib/detail/early_exit.hpp"

#include <type_traits>

namespace stxlib::msvc_runtime {

namespace {

// --- the frame's tables -------------------------------------------------------

struct FrameTables final {
    uptr image_base;
    const FunctionInfo* info;
    const UnwindMapEntry* unwind_map;
    const TryBlockMapEntry* try_blocks;
    const IpStateEntry* ip_map;
};

enum class FrameKind {
    parent,
    catch_funclet,
};

// One frame instance: the parent function's frame or a catch funclet's.
struct Frame final {
    FrameKind kind;
    uptr establisher;
    uptr parent_establisher;
    i32 base_state;                      // empty_state in the parent; the catch state in a funclet
    i64* state_override;
};

// The catch a search found, kept in the searching handler's frame, which
// stays intact below every frame the unwind passes and until run_catch
// returns (step 3 above).
inline constexpr u64 catch_context_cookie = 0x5354584C434F4E53ull; // "STXLCONS"

struct CatchContext final {
    u64 cookie;
    HandledException exception;
    FrameTables tables;
    const HandlerType* handler;
    const CatchableType* catchable;      // null for catch(...)
    uptr throw_image_base;
    uptr target_establisher;
    uptr parent_establisher;
    i64* target_state_override;
    i32 unwind_to_state;                 // the try block's own state
    i32 catch_state;
};

// The unwind record's parameters (step 3).
inline constexpr u32 consolidate_callback = 0;
inline constexpr u32 consolidate_cookie = 1;
inline constexpr u32 consolidate_context = 2;
inline constexpr u32 consolidate_parameter_count = 3;

void* run_catch(EXCEPTION_RECORD* record) noexcept;

// --- reading the tables -----------------------------------------------------------

bool read_tables(const DISPATCHER_CONTEXT& dispatcher, FrameTables& tables) noexcept {
    const uptr image_base = static_cast<uptr>(dispatcher.ImageBase);
    const ImageOffset info_offset{*static_cast<const u32*>(dispatcher.HandlerData)};
    const FunctionInfo* const info = reinterpret_cast<const FunctionInfo*>(resolve(image_base, info_offset));
    if (info == nullptr) return false;
    if ((*info).magic < function_magic_oldest || (*info).magic > function_magic_newest) return false;
    if ((*info).state_count < 0) fail_fast();
    tables.image_base = image_base;
    tables.info = info;
    tables.unwind_map = reinterpret_cast<const UnwindMapEntry*>(resolve(image_base, (*info).unwind_map));
    tables.try_blocks = reinterpret_cast<const TryBlockMapEntry*>(resolve(image_base, (*info).try_block_map));
    tables.ip_map = reinterpret_cast<const IpStateEntry*>(resolve(image_base, (*info).ip_map));
    return true;
}

const HandlerType* handler_at(const FrameTables& tables, const TryBlockMapEntry& block, const i32 index) noexcept {
    const HandlerType* const handlers = reinterpret_cast<const HandlerType*>(resolve(tables.image_base, block.handlers));
    if (handlers == nullptr) fail_fast();
    const HandlerType* const result = handlers + index;
    return result;
}

// The state "PCs after `ip`" have: the last entry at or before the PC.
i32 ip_state(const FrameTables& tables, const uptr pc) noexcept {
    const uptr offset = pc - tables.image_base;
    i32 result = empty_state;
    u32 best = 0;
    bool found = false;
    for (u32 index = 0; index < (*tables.info).ip_map_count; ++index) {
        const IpStateEntry& entry = tables.ip_map[index];
        const bool at_or_before = static_cast<uptr>(entry.ip.value) <= offset;
        if (at_or_before && (!found || entry.ip.value >= best)) {
            best = entry.ip.value;
            result = entry.state;
            found = true;
        }
    }
    return result;
}

void check_state(const FrameTables& tables, const i32 state) noexcept {
    if (state < empty_state || state >= (*tables.info).state_count) fail_fast();
}

// Which frame this is: a catch funclet when the RUNTIME_FUNCTION begins at
// one of the handlers (WinException.cpp, beginFunclet: the funclet's
// symbol starts its unwind entry and is the handler's address).
Frame identify_frame(const FrameTables& tables, const DISPATCHER_CONTEXT& dispatcher, const uptr establisher) noexcept {
    Frame frame{};
    frame.kind = FrameKind::parent;
    frame.establisher = establisher;
    frame.parent_establisher = establisher;
    frame.base_state = empty_state;
    const u32 begin = (*dispatcher.FunctionEntry).BeginAddress;
    const HandlerType* funclet_handler = nullptr;
    for (u32 block_index = 0; block_index < (*tables.info).try_block_count && funclet_handler == nullptr; ++block_index) {
        const TryBlockMapEntry& block = tables.try_blocks[block_index];
        for (i32 handler_index = 0; handler_index < block.handler_count; ++handler_index) {
            const HandlerType* const handler = handler_at(tables, block, handler_index);
            if ((*handler).handler.value == begin) {
                funclet_handler = handler;
                frame.base_state = block.try_high + 1;
                break;
            }
        }
    }
    if (funclet_handler == nullptr) {
        Byte* const slot = reinterpret_cast<Byte*>(establisher) + (*tables.info).unwind_help;
        frame.state_override = reinterpret_cast<i64*>(slot);
        return frame;
    }
    // The funclet homed RDX, the parent's establisher frame, into its
    // caller's home area; the caller is stxlib_msvc_call_catch, whose frame
    // holds the record 24 bytes and the cookie 32 bytes above that slot.
    frame.kind = FrameKind::catch_funclet;
    Byte* const homed = reinterpret_cast<Byte*>(establisher) + (*funclet_handler).parent_frame_offset;
    frame.parent_establisher = *reinterpret_cast<const uptr*>(homed);
    CatchRecord* const record = *reinterpret_cast<CatchRecord* const*>(homed + 24);
    const u64 cookie = *reinterpret_cast<const u64*>(homed + 32);
    if (cookie != catch_record_cookie || record == nullptr || (*record).cookie != catch_record_cookie) fail_fast();
    frame.state_override = &(*record).callee_state;
    return frame;
}

i32 current_state(const FrameTables& tables, const Frame& frame, const uptr pc) noexcept {
    const i64 kept = *frame.state_override;
    i32 result = empty_state;
    if (kept != no_state_override) {
        result = static_cast<i32>(kept);
    } else {
        result = ip_state(tables, pc);
    }
    check_state(tables, result);
    return result;
}

// --- unwinding a frame --------------------------------------------------------------

// Runs the cleanups from `from` down to, not including, `to`. States nest
// with each parent numbered below its children (calculateCXXStateNumbers),
// so the walk only descends. The frame's slot records each step before its
// cleanup runs, so a collided unwind resumes where this one stopped.
void unwind_frame(const FrameTables& tables, const Frame& frame, const i32 from, const i32 to) noexcept {
    i32 state = from;
    i32 steps = 0;
    while (state > to) {
        if (state >= (*tables.info).state_count || steps > (*tables.info).state_count) fail_fast();
        const UnwindMapEntry& entry = tables.unwind_map[state];
        const i32 next = entry.to_state;
        if (next >= state) fail_fast();
        *frame.state_override = next;
        const Byte* const action = resolve(tables.image_base, entry.action);
        if (action != nullptr) {
            // Cleanup funclets take the parent's establisher frame in RDX
            // (X86FrameLowering.cpp, emitPrologue: "Find the funclet
            // establisher parameter").
            [[maybe_unused]] const early_exit::Whole cleanup_marker{"the frame's slot already names the next state; an exception "
                                                   "leaving the cleanup terminates in the guard"};
            stxlib_msvc_call_guarded(action, nullptr, reinterpret_cast<void*>(frame.parent_establisher), nullptr);
        }
        state = next;
        steps = steps + 1;
    }
}

// --- matching -----------------------------------------------------------------------------

struct Match final {
    const TryBlockMapEntry* block;
    const HandlerType* handler;
    const CatchableType* catchable;
};

const TypeDescriptor* handler_type(const FrameTables& tables, const HandlerType& handler) noexcept {
    const TypeDescriptor* const type = reinterpret_cast<const TypeDescriptor*>(resolve(tables.image_base, handler.type));
    if (type == nullptr || (*type).name[0] == '\0') return nullptr;
    return type;
}

// catch(...): every C++ exception; a structured exception only when the
// handler lacks 0x40 and the function was compiled for asynchronous
// exceptions (-EHa).
bool catch_all_matches(const FrameTables& tables, const HandlerType& handler, const bool cxx) noexcept {
    if (cxx) return true;
    if ((handler.adjectives & handler_catch_all_cxx_only) != 0) return false;
    const bool result = ((*tables.info).flags & function_synchronous_only) == 0;
    return result;
}

// [except.handle]p3 as the throw side encodes it: each catchable type is a
// type the object may be caught as (exact, unambiguous public bases, void*
// for object pointers), with its displacement; a pointer's pointee
// qualifiers must not exceed the handler's.
const CatchableType* typed_match(const TypeDescriptor& wanted, const HandlerType& handler, const ThrowInfo& info,
                                 const uptr throw_image_base) noexcept {
    const CatchableTypeArray* const array =
        reinterpret_cast<const CatchableTypeArray*>(resolve(throw_image_base, info.catchable_type_array));
    if (array == nullptr) return nullptr;
    const u32 qualifiers = info.attributes & (throw_is_const | throw_is_volatile | throw_is_unaligned);
    const u32 allowed = static_cast<u32>(handler.adjectives) & (handler_is_const | handler_is_volatile | handler_is_unaligned);
    if ((qualifiers & ~allowed) != 0) return nullptr;
    for (i32 index = 0; index < (*array).count; ++index) {
        const CatchableType* const type = reinterpret_cast<const CatchableType*>(resolve(throw_image_base, (*array).types[index]));
        if (type == nullptr) continue;
        const TypeDescriptor* const descriptor = reinterpret_cast<const TypeDescriptor*>(resolve(throw_image_base, (*type).type));
        if (descriptor == nullptr || !same_type(wanted, *descriptor)) continue;
        const bool reference = (handler.adjectives & handler_is_reference) != 0;
        if (((*type).properties & catchable_by_reference_only) != 0 && !reference) continue;
        return type;
    }
    return nullptr;
}

bool find_match(const FrameTables& tables, const Frame& frame, const i32 state, const EXCEPTION_RECORD& record, Match& match) noexcept {
    const bool cxx = is_cxx_exception(record);
    const ThrowInfo* info = nullptr;
    uptr throw_image_base = 0;
    if (cxx) {
        info = reinterpret_cast<const ThrowInfo*>(record.ExceptionInformation[2]);
        throw_image_base = record.NumberParameters >= cxx_exception_parameters ? static_cast<uptr>(record.ExceptionInformation[3]) : 0;
        if (info == nullptr) return false;
    }
    // Candidate blocks, innermost first: the block with the greatest
    // try_low below the previous one's (nested blocks have higher states).
    i32 bound = 0x7FFFFFFF;
    for (;;) {
        const TryBlockMapEntry* chosen = nullptr;
        for (u32 index = 0; index < (*tables.info).try_block_count; ++index) {
            const TryBlockMapEntry& block = tables.try_blocks[index];
            const bool holds = block.try_low <= state && state <= block.try_high;
            const bool own = frame.kind == FrameKind::parent || block.try_low > frame.base_state;
            const bool inner = block.try_low < bound;
            if (holds && own && inner && (chosen == nullptr || block.try_low > (*chosen).try_low)) chosen = &block;
        }
        if (chosen == nullptr) return false;
        bound = (*chosen).try_low;
        for (i32 handler_index = 0; handler_index < (*chosen).handler_count; ++handler_index) {
            const HandlerType* const handler = handler_at(tables, *chosen, handler_index);
            const TypeDescriptor* const wanted = handler_type(tables, *handler);
            if (wanted == nullptr) {
                if (!catch_all_matches(tables, *handler, cxx)) continue;
                match.block = chosen;
                match.handler = handler;
                match.catchable = nullptr;
                return true;
            }
            if (!cxx) continue;
            const CatchableType* const catchable = typed_match(*wanted, *handler, *info, throw_image_base);
            if (catchable == nullptr) continue;
            match.block = chosen;
            match.handler = handler;
            match.catchable = catchable;
            return true;
        }
    }
}

// --- the catch object ---------------------------------------------------------------------

void copy_bytes(void* const destination, const void* const source, const i32 size) noexcept {
    if (size <= 0) return;
    __builtin_memcpy(destination, source, static_cast<usize>(size));
}

void copy_class(const CatchableType& type, const uptr image_base, void* const destination, void* const source) noexcept {
    const Byte* const constructor = resolve(image_base, type.copy_constructor);
    if (constructor == nullptr) {
        copy_bytes(destination, source, type.size);
        return;
    }
    // A class with virtual bases takes the Microsoft ABI's "most derived"
    // flag after the source.
    void* const most_derived = (type.properties & catchable_has_virtual_bases) != 0 ? reinterpret_cast<void*>(1) : nullptr;
    [[maybe_unused]] const early_exit::Whole copy_marker{"no runtime state is half-updated during the copy; a copy that throws "
                                        "terminates in the guard"};
    stxlib_msvc_call_guarded(constructor, destination, source, most_derived);
}

// A thrown pointer, caught by value as a pointer to a base, is adjusted;
// decorated pointer names begin ".P" (".PEAV...", ".PEAX").
bool is_pointer(const CatchableType& type, const uptr image_base) noexcept {
    if (type.size != static_cast<i32>(sizeof(void*))) return false;
    const TypeDescriptor* const descriptor = reinterpret_cast<const TypeDescriptor*>(resolve(image_base, type.type));
    if (descriptor == nullptr) return false;
    const bool result = (*descriptor).name[0] == '.' && (*descriptor).name[1] == 'P';
    return result;
}

void build_catch_object(const CatchContext& context) noexcept {
    const HandlerType& handler = *context.handler;
    if (handler.catch_object_offset == 0 || context.catchable == nullptr) return;
    const CatchableType& type = *context.catchable;
    Byte* const destination = reinterpret_cast<Byte*>(context.parent_establisher) + handler.catch_object_offset;
    Byte* const object = static_cast<Byte*>(exception_object(context.exception));
    if ((handler.adjectives & handler_is_reference) != 0) {
        // A reference binds to the object, or to its base subobject; a
        // scalar is bound as thrown.
        Byte* target = object;
        if ((type.properties & catchable_is_scalar) == 0) target = displace(object, type.displacement);
        *reinterpret_cast<void**>(destination) = target;
        return;
    }
    if ((type.properties & catchable_is_scalar) != 0) {
        if (is_pointer(type, context.throw_image_base)) {
            Byte* const pointee = *reinterpret_cast<Byte* const*>(object);
            Byte* const adjusted = pointee == nullptr ? nullptr : displace(pointee, type.displacement);
            *reinterpret_cast<void**>(destination) = adjusted;
            return;
        }
        copy_bytes(destination, object, type.size);
        return;
    }
    Byte* const source = displace(object, type.displacement);
    copy_class(type, context.throw_image_base, destination, source);
}

// --- running the catch (the consolidation callback, step 3) ----------------------------

void* run_catch(EXCEPTION_RECORD* const record) noexcept {
    CatchContext& context = *reinterpret_cast<CatchContext*>((*record).ExceptionInformation[consolidate_context]);
    if (context.cookie != catch_context_cookie) fail_fast();
    build_catch_object(context);
    const bool cxx = is_cxx_exception(context.exception);
    // The handler is active once its parameter is initialised.
    if (cxx) add_uncaught(-1);
    *context.target_state_override = context.catch_state;
    CatchRecord active{};
    active.cookie = catch_record_cookie;
    active.exception = context.exception;
    active.callee_state = no_state_override;
    active.rethrown = false;
    active.outer = innermost_catch();
    set_innermost_catch(&active);
    const Byte* const funclet = resolve(context.tables.image_base, (*context.handler).handler);
    if (funclet == nullptr) fail_fast();
    [[maybe_unused]] const early_exit::Whole catch_marker{"the record is linked and the frame's slot names the catch state; an exception "
                                         "leaving the catch is ended by the catch guard"};
    void* const continuation = stxlib_msvc_call_catch(funclet, context.parent_establisher, &active);
    set_innermost_catch(active.outer);
    if (cxx && !object_still_handled(exception_object(active.exception), nullptr)) {
        destroy_exception_object(active.exception);
    }
    *context.target_state_override = no_state_override;
    return continuation;
}

[[noreturn]] void begin_catch(const EXCEPTION_RECORD& record, const DISPATCHER_CONTEXT& dispatcher, const FrameTables& tables,
                              const Frame& frame, const Match& match) noexcept {
    CatchContext context{};
    context.cookie = catch_context_cookie;
    copy_handled_exception(record, context.exception);
    context.tables = tables;
    context.handler = match.handler;
    context.catchable = match.catchable;
    context.throw_image_base = exception_image_base(context.exception);
    context.target_establisher = frame.establisher;
    context.parent_establisher = frame.parent_establisher;
    context.target_state_override = frame.state_override;
    context.unwind_to_state = (*match.block).try_low;
    context.catch_state = (*match.block).try_high + 1;
    EXCEPTION_RECORD unwind_record{};
    unwind_record.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    unwind_record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    unwind_record.NumberParameters = consolidate_parameter_count;
    unwind_record.ExceptionInformation[consolidate_callback] = reinterpret_cast<ULONG_PTR>(&run_catch);
    unwind_record.ExceptionInformation[consolidate_cookie] = static_cast<ULONG_PTR>(catch_context_cookie);
    unwind_record.ExceptionInformation[consolidate_context] = reinterpret_cast<ULONG_PTR>(&context);
    // RtlUnwindEx fills this with the unwound context; its first contents
    // are not read.
    CONTEXT unwind_context{};
    // The target IP is the PC the search found in the catching frame: an
    // address inside that function's body, so the frames above it unwind
    // normally while the catch runs; the callback's result replaces it.
    [[maybe_unused]] const early_exit::Whole unwind_marker{"the catch context is complete before the unwind and lives in this frame, "
                                          "which stays intact until the callback returns"};
    RtlUnwindEx(reinterpret_cast<void*>(frame.establisher), reinterpret_cast<void*>(dispatcher.ControlPc), &unwind_record, nullptr,
                &unwind_context, dispatcher.HistoryTable);
    fail_fast();
}

bool is_our_unwind(const EXCEPTION_RECORD& record) noexcept {
    if (record.ExceptionCode != static_cast<DWORD>(STATUS_UNWIND_CONSOLIDATE)) return false;
    if (record.NumberParameters < consolidate_parameter_count) return false;
    if (record.ExceptionInformation[consolidate_cookie] != static_cast<ULONG_PTR>(catch_context_cookie)) return false;
    const bool result = record.ExceptionInformation[consolidate_callback] == reinterpret_cast<ULONG_PTR>(&run_catch);
    return result;
}

// The exception an unwind carries: the C++ exception behind our own unwind,
// or the record itself.
const void* unwinding_object(const EXCEPTION_RECORD& record) noexcept {
    if (is_our_unwind(record)) {
        const CatchContext& context = *reinterpret_cast<const CatchContext*>(record.ExceptionInformation[consolidate_context]);
        if (!is_cxx_exception(context.exception)) return nullptr;
        return exception_object(context.exception);
    }
    if (!is_cxx_exception(record)) return nullptr;
    const void* const result = reinterpret_cast<const void*>(record.ExceptionInformation[1]);
    return result;
}

EXCEPTION_DISPOSITION handle_frame(EXCEPTION_RECORD& record, const uptr establisher, DISPATCHER_CONTEXT& dispatcher) noexcept {
    FrameTables tables{};
    if (!read_tables(dispatcher, tables)) return ExceptionContinueSearch;
    const Frame frame = identify_frame(tables, dispatcher, establisher);
    const i32 state = current_state(tables, frame, static_cast<uptr>(dispatcher.ControlPc));
    if ((record.ExceptionFlags & EXCEPTION_UNWINDING) != 0) {
        i32 to = frame.base_state;
        if ((record.ExceptionFlags & EXCEPTION_TARGET_UNWIND) != 0) {
            if (is_our_unwind(record)) {
                const CatchContext& context = *reinterpret_cast<const CatchContext*>(record.ExceptionInformation[consolidate_context]);
                if (context.target_establisher != establisher) fail_fast();
                to = context.unwind_to_state;
            } else {
                // Another unwinder's target in this frame (longjmp): unwind
                // to the state of the address it resumes.
                const i32 target = ip_state(tables, static_cast<uptr>(dispatcher.TargetIp));
                if (target > to) to = target;
            }
        }
        unwind_frame(tables, frame, state, to);
        return ExceptionContinueSearch;
    }
    Match match{};
    if (find_match(tables, frame, state, record, match)) begin_catch(record, dispatcher, tables, frame, match);
    if (((*tables.info).flags & function_noexcept) != 0 && is_cxx_exception(record)) terminate_now();
    return ExceptionContinueSearch;
}

} // namespace

// --- objects (runtime_state.hpp) ----------------------------------------------------------

void destroy_exception_object(const HandledException& exception) noexcept {
    if (!is_cxx_exception(exception)) return;
    const ThrowInfo* const info = exception_throw_info(exception);
    if (info == nullptr) return;
    const Byte* const destructor = resolve(exception_image_base(exception), (*info).cleanup);
    if (destructor == nullptr) return;
    [[maybe_unused]] const early_exit::Whole destroy_marker{"the object's last handler has ended; a destructor that throws terminates "
                                           "in the guard"};
    stxlib_msvc_call_guarded(destructor, exception_object(exception), nullptr, nullptr);
}

const CatchableType* exact_catchable_type(const ThrowInfo& info, const uptr image_base) noexcept {
    const CatchableTypeArray* const array = reinterpret_cast<const CatchableTypeArray*>(resolve(image_base, info.catchable_type_array));
    if (array == nullptr || (*array).count < 1) return nullptr;
    const CatchableType* const result = reinterpret_cast<const CatchableType*>(resolve(image_base, (*array).types[0]));
    return result;
}

void copy_exception_object(const CatchableType& type, const uptr image_base, void* const destination, void* const source) noexcept {
    if ((type.properties & catchable_is_scalar) != 0) {
        copy_bytes(destination, source, type.size);
        return;
    }
    copy_class(type, image_base, destination, source);
}

} // namespace stxlib::msvc_runtime

// --- the entry points ---------------------------------------------------------------------------

extern "C" {

EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD* const record, void* const establisher_frame, CONTEXT* const context,
                                         void* const dispatcher_context) {
    static_cast<void>(context);
    DISPATCHER_CONTEXT& dispatcher = *static_cast<DISPATCHER_CONTEXT*>(dispatcher_context);
    const EXCEPTION_DISPOSITION result =
        stxlib::msvc_runtime::handle_frame(*record, reinterpret_cast<stxlib::uptr>(establisher_frame), dispatcher);
    return result;
}

// The terminate guard (stxlib_msvc_call_guarded's frame): a C++ exception
// about to leave a cleanup, a copy constructor or a destructor the runtime
// called terminates, at the search, before anything unwinds.
EXCEPTION_DISPOSITION stxlib_msvc_terminate_guard(EXCEPTION_RECORD* const record, void* const establisher_frame,
                                                  CONTEXT* const context, void* const dispatcher_context) {
    static_cast<void>(establisher_frame);
    static_cast<void>(context);
    static_cast<void>(dispatcher_context);
    if (((*record).ExceptionFlags & EXCEPTION_UNWINDING) != 0) return ExceptionContinueSearch;
    if (stxlib::msvc_runtime::is_cxx_exception(*record)) stxlib::msvc_runtime::terminate_now();
    return ExceptionContinueSearch;
}

// The catch guard (stxlib_msvc_call_catch's frame): an exception leaving a
// catch body ends that catch. Its record is unlinked and its object
// destroyed, unless the exception leaving is that same object rethrown,
// or an outer active catch still handles it.
EXCEPTION_DISPOSITION stxlib_msvc_catch_guard(EXCEPTION_RECORD* const record, void* const establisher_frame, CONTEXT* const context,
                                              void* const dispatcher_context) {
    using namespace stxlib::msvc_runtime;
    static_cast<void>(context);
    static_cast<void>(dispatcher_context);
    if (((*record).ExceptionFlags & EXCEPTION_UNWINDING) == 0) return ExceptionContinueSearch;
    stxlib::Byte* const frame = static_cast<stxlib::Byte*>(establisher_frame);
    CatchRecord* const active = *reinterpret_cast<CatchRecord* const*>(frame + 32);
    const stxlib::u64 cookie = *reinterpret_cast<const stxlib::u64*>(frame + 40);
    if (cookie != catch_record_cookie || active == nullptr) fail_fast();
    // A collided unwind may call the guard again for a catch already ended.
    if ((*active).cookie == ended_catch_cookie) return ExceptionContinueSearch;
    if ((*active).cookie != catch_record_cookie || innermost_catch() != active) fail_fast();
    set_innermost_catch((*active).outer);
    (*active).cookie = ended_catch_cookie;
    if (!is_cxx_exception((*active).exception)) return ExceptionContinueSearch;
    const void* const object = exception_object((*active).exception);
    const bool passed_on = (*active).rethrown && unwinding_object(*record) == object;
    if (!passed_on && !object_still_handled(object, nullptr)) destroy_exception_object((*active).exception);
    return ExceptionContinueSearch;
}

} // extern "C"

static_assert(std::is_same_v<decltype(&__CxxFrameHandler3), PEXCEPTION_ROUTINE>);
static_assert(std::is_same_v<decltype(&stxlib_msvc_terminate_guard), PEXCEPTION_ROUTINE>);
static_assert(std::is_same_v<decltype(&stxlib_msvc_catch_guard), PEXCEPTION_ROUTINE>);
