/* setjmp.h for x86_64-pc-windows-msvc (C17 7.13).
 *
 * Neither half comes from ucrtbase.dll: STXlib's C++ runtime supplies
 * _setjmp, _setjmpex and longjmp in each image (projects queue,
 * claude__windows-msvc-target.md, the owner table), so the buffer's layout
 * is the runtime's and no declaration here carries an import.
 *
 * setjmp expands to _setjmp, which clang recognises on this target and calls
 * as _setjmp(buffer, frame address) (clang 22.1.8, lib/CodeGen/CGBuiltin.cpp,
 * EmitMSVCRTSetJmp, kind _setjmp on x86_64). The frame address lets longjmp
 * unwind the frames in between; the runtime documents what that unwind runs.
 *
 * jmp_buf: 16 elements of 16 bytes, aligned to 16 (256 bytes). It holds the
 * x64 nonvolatile state (Learn, "x64 calling convention", "Callee/Caller
 * saved registers": RBX, RBP, RDI, RSI, RSP, R12-R15 and XMM6-XMM15) plus the
 * resume address, the frame, MXCSR and the x87 control word: 10 * 8 + 10 * 16
 * + 16 = 256 bytes at most.
 */
#ifndef STXLIB_CRT_SETJMP_H
#define STXLIB_CRT_SETJMP_H

#include "stxlib_crt_common.h"

_STXLIB_CRT_BEGIN

typedef struct __declspec(align(16)) _SETJMP_FLOAT128 {
    unsigned long long Part[2];
} SETJMP_FLOAT128;

#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[_JBLEN];

int __cdecl _setjmp(jmp_buf environment);
_STXLIB_NORETURN void __cdecl longjmp(jmp_buf environment, int value);

_STXLIB_CRT_END

#define setjmp(environment) _setjmp(environment)

#endif /* STXLIB_CRT_SETJMP_H */
