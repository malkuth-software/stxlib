#pragma once

// STXlib's floating-point discipline (house style "Floating point",
// adopted for STXlib 2026-11-13: notes
// decisions/2026-11-13-locals-float-mutability.md and
// planning/stages/A1-float-discipline.md), checked where the compiler can
// see it. Included by every STXlib source that computes in floating point.
// -ffp-contract has no macro on GCC or Clang; the CMake flag check covers it
// (rule 1), and the pragma below repeats it for Clang.

#include <cfloat>

#if defined(__FAST_MATH__)
#error "STXlib compiles without fast-math (Floating point, rule 1)"
#endif

#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "STXlib evaluates every intermediate in its own type: FLT_EVAL_METHOD must be 0 (Floating point, rule 5)"
#endif

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
