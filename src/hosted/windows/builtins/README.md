# Compiler builtins for x86_64-pc-windows-msvc

Empty on purpose: the build needs no compiler-rt builtin.

Measured 2026-11-17 on the stock MSVC-environment build of GUI.Forms
22cb45f59, stx 91dd981 and STXlib 4d1ae28, and on the libc++ 22.1.8 built
by `tools/windows_libcxx/build_libcxx.sh`: the undefined symbols of every
object (`llvm-nm -u`), less the symbols any of them defines, less the names
`ucrtbase.dll` exports, the Windows API, vcruntime's EH and RTTI entries and
operator new and delete, leave no compiler-rt name (`__udivti3`,
`__muloti4`, `__floattidf` and the like): clang inlines every 128-bit
operation and every conversion these sources use on x86_64.

Two compiler-support symbols remain and are not compiler-rt's on this
target: `__chkstk` (the stack probe for frames over a page) and `_fltused`
(the floating-point marker). Visual C++ supplies them from its static C
runtime library; here STXlib's runtime or startup shim must define them.
Which of the two owns them is an open decision.

Rerun the measurement whenever the compiler, a source pin or an
optimisation flag changes; a new builtin goes here with the source of its
definition.
