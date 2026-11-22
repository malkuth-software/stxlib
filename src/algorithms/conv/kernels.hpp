#pragma once

// stxlib::conv's kernel table: the three repeated kernels of the separated CONV field
// (work/conv/SIMD_DESIGN.md), compiled once per SIMD tier through variant.inc
// (kernels_scalar.cpp, kernels_v128.cpp, kernels_avx2_fma.cpp) and chosen once from
// simd::selected_level(). Every tier gives the scalar tier's bits.
//
//   K1, the contest: raw currents and the one-signed test for a batch of windows (pass A),
//       then the unsigned box, preclip, capacity threshold and shift for the listed windows
//       (pass B), in the operation order of stx_conv's compile_cell.
//   K2, preview rows: a row's interior cells (anchor plus raw currents), a tile's vertical
//       windows, and the vertical values.
//   K3, canon cells: the G coefficients, node windows, strip primitives Q, Q at a phase,
//       edge flux points and the moment contraction.
//
// Layouts are channel innermost (4 channels); windows are structure of arrays
// (window[k][lane], k = 0..7 for samples y[i-3..i+4]); inputs and outputs never overlap.

#include "stxlib/simd.hpp"
#include "stxlib/types.hpp"

namespace stxlib::conv::detail {

struct Kernels final {
    // K1
    usize (*pass_a)(const double* const window[8], double* const raw[5], u32* list, usize n);
    void (*pass_b)(const double* const window[8], const double* const raw[5], double* const admitted[5], u8* idle, usize m);
    // K2
    void (*row_cells)(const double* decoded, usize count, double* local);
    void (*tile_windows)(usize queries, const double* const* rows, const double* tau_u, double* const window[8]);
    void (*vertical_linear)(usize queries, const double* const window[8], const double* sv, double* value);
    void (*vertical_admitted)(const double* w3, const double* tau, const double* const admitted[5], double* value, usize m);
    // K3
    void (*g_coefficients)(const double* raw_columns, const double* anchor_row, double* g);
    void (*node_windows)(const double* controls, usize nodes, const double* b5, double* const window[8]);
    void (*strip_q)(const double* g, const double* e, const double* basis, const double* anchor, double t0, double* q);
    void (*q_at)(const double* q, double t, double* out);
    void (*flux_points)(const double* q, usize points, const double* tu, const double* tv, const double* weight, double* sum4);
    void (*moments_contract)(const double* q, const double* m, double weight, double* sum4);
};

namespace scalar {
const Kernels& kernels() noexcept;
}
namespace v128 {
const Kernels& kernels() noexcept;
}
namespace avx2_fma {
const Kernels& kernels() noexcept;
}

// The table for a level, falling back to the highest tier this build compiled.
[[nodiscard]] const Kernels& kernels_for(simd::Level level) noexcept;

} // namespace stxlib::conv::detail
