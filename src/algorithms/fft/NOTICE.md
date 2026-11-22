# stx_fft: provenance

The kernels in this directory are copied from BFFT, the owner's own real
FFT library (falseywinchnet/bfft, MIT), at commit
`edf5f0134290d4cc368c50a77cc19040882beb48` (2026-11-13), on the owner's
direction of 2026-11-22 (projects `tracks/stxlib/stx_fft.md`): copy, do not
change BFFT; three modes, each its own kernel.

| STXlib file | BFFT file (at `edf5f01`) | What came over |
|---|---|---|
| `dif.inc` | `src/detail/bruun_dif_kernel.hpp` | the streaming kernels and the residue engine, both precisions |
| `bodft.inc` | `src/detail/bodft_kernel.hpp` | the whole kernel (combines, leaves, schedule); the plan's tables moved to `plan.cpp` |
| `dip.inc` | `src/detail/bruun_dip_kernel.hpp` | the whole kernel (walk, cells, seeds, tails, egress); the tables moved to `plan.cpp` |
| `lanes.inc` | `src/detail/bruun_simd_backend.hpp`, `src/detail/bodft_kernel.hpp` | the vector macros as functions, without the fused ones |
| `plan.cpp` | the three kernels' table construction, `src/bfft.cpp`, `src/bodft.cpp` | index tables and permutations copied; every twiddle recomputed with STXlib's `sincospi`/`cospi` |

The kernel text was carried by a mechanical transform (kept outside the
repository with the port's notes): fused multiply-adds spelled as the
multiply and the add or subtract, BFFT's vector macros renamed to
`lanes.inc`'s functions, `BRUUN_LEVEL` to `STXLIB_FFT_LEVEL`. BFFT's
internal names (`N`, `C`, `TW`, `cell_fwd_ip`, ...) are kept inside the
kernels so a diff against BFFT reads; STXlib's names are at the interface
(`include/stxlib/fft.hpp`, `plan_state.hpp`). Not copied: BFFT's sin/cos
tables and code (`generated/bruun_sincos_table256.hpp`,
`bruun_phase_slope_tables.hpp`), its DIT kernel, its native output order,
its schedules, and everything outside the three kernels.

BFFT's licence (its `LICENSE` at `edf5f01`, verbatim):

    MIT License

    Copyright (c) 2026 joshuah.rainstar@gmail.com

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
