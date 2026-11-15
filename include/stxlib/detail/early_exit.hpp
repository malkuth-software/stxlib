#pragma once

// The early-exit annotations (AGENTS.md, "Early exits and termination";
// notes audits/E-early-exit.md, section 1, which the shared check in
// template_harness reads).
//
// Declaration marks expand to [[clang::annotate("early_exit.<name>")]] where
// the attribute exists and to nothing elsewhere; the shared check keys on the
// string. Site markers are local declarations of the empty types below,
// placed as the statement immediately before the foreign call's statement;
// they cover every foreign call in that one statement. Every marker states
// its reason as a string literal, so the reason is in the AST. They generate
// no code.

#include "stxlib/types.hpp"

namespace stxlib::early_exit {

// Form 1: every invariant is whole before the call.
struct Whole final {
    template <usize Count>
    consteval explicit Whole(const char (&reason)[Count]) noexcept {
        static_cast<void>(reason);
        static_assert(Count > 1, "a form-1 marker states which invariants hold");
    }
};

// Form 2: `guard`, an object of a class marked STXLIB_EARLY_EXIT_GUARD and
// live at the call, restores or completes the partial state. Valid only in a
// translation unit compiled with exceptions.
struct Guarded final {
    // constexpr, not consteval: the guard is a local object.
    template <typename Guard, usize Count>
    constexpr Guarded(const Guard& guard, const char (&reason)[Count]) noexcept {
        static_cast<void>(guard);
        static_cast<void>(reason);
        static_assert(Count > 1, "a form-2 marker states what the guard restores");
    }
};

// The callee cannot exit early, or its only early exit is termination the
// owner accepted; the reason says which (the check lists these for review).
struct NoExit final {
    template <usize Count>
    consteval explicit NoExit(const char (&reason)[Count]) noexcept {
        static_cast<void>(reason);
        static_assert(Count > 1, "a no-exit marker states why the callee cannot exit early");
    }
};

} // namespace stxlib::early_exit

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::annotate)
#define STXLIB_EARLY_EXIT_ANNOTATE(name) [[clang::annotate("early_exit." name)]]
#endif
#endif
#if !defined(STXLIB_EARLY_EXIT_ANNOTATE)
#define STXLIB_EARLY_EXIT_ANNOTATE(name)
#endif

// Declaration marks (audit E, section 1.2).
#define STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_ANNOTATE("foreign")
#define STXLIB_EARLY_EXIT_CANCEL STXLIB_EARLY_EXIT_ANNOTATE("cancel")
#define STXLIB_EARLY_EXIT_NO_EXIT STXLIB_EARLY_EXIT_ANNOTATE("no_exit")
#define STXLIB_EARLY_EXIT_OWN STXLIB_EARLY_EXIT_ANNOTATE("own")
#define STXLIB_NO_UNWIND STXLIB_EARLY_EXIT_ANNOTATE("no_unwind")
#define STXLIB_EARLY_EXIT_GUARD STXLIB_EARLY_EXIT_ANNOTATE("guard")
#define STXLIB_EARLY_EXIT_LOCK STXLIB_EARLY_EXIT_ANNOTATE("lock")
#define STXLIB_EARLY_EXIT_OPEN STXLIB_EARLY_EXIT_ANNOTATE("open")
#define STXLIB_EARLY_EXIT_NO_CANCEL STXLIB_EARLY_EXIT_ANNOTATE("no_cancel")
#define STXLIB_EARLY_EXIT_WHOLE_BODY STXLIB_EARLY_EXIT_ANNOTATE("whole")
#define STXLIB_EARLY_EXIT_FORCED_UNWIND STXLIB_EARLY_EXIT_ANNOTATE("forced_unwind")

// Site markers (audit E, section 1.3). The name is unique per line.
#define STXLIB_EARLY_EXIT_SITE_NAME_JOIN(prefix, line) prefix##line
#define STXLIB_EARLY_EXIT_SITE_NAME(line) STXLIB_EARLY_EXIT_SITE_NAME_JOIN(early_exit_site_, line)
#define STXLIB_EARLY_EXIT_WHOLE(reason)                                                                 \
    [[maybe_unused]] const ::stxlib::early_exit::Whole STXLIB_EARLY_EXIT_SITE_NAME(__LINE__) { reason }
#define STXLIB_EARLY_EXIT_GUARDED(guard, reason)                                                        \
    [[maybe_unused]] const ::stxlib::early_exit::Guarded STXLIB_EARLY_EXIT_SITE_NAME(__LINE__) {        \
        guard, reason                                                                                   \
    }
#define STXLIB_EARLY_EXIT_NONE(reason)                                                                  \
    [[maybe_unused]] const ::stxlib::early_exit::NoExit STXLIB_EARLY_EXIT_SITE_NAME(__LINE__) { reason }
