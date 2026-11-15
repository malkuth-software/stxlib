// Tests of the process's SIMD level (include/stxlib/simd.hpp). Ported from
// stx_auxlib's tests/simd_tests.cpp at b6e330a: the detected features are
// consistent, the hardware level matches them, a request made before the
// first selection lowers the level and fixes it, and a request after that
// changes nothing. Prints the detection so CI logs show which tier ran.
// The test removes STX_SIMD_LEVEL first, then makes its own request.

#include "stxlib/environment.hpp"
#include "stxlib/simd.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <string>

namespace {

namespace simd = stxlib::simd;

std::string text_of(const stxlib::TextView view) {
    const std::string result(view.data(), view.count());
    return result;
}

void features_are_consistent() {
    const simd::Features features = simd::detected_features();
    CHECK(!features.avx2 || features.avx);
    CHECK(!features.fma || features.avx);
#if defined(__x86_64__)
    CHECK(features.sse2);
    CHECK(!features.neon);
#elif defined(__aarch64__)
    CHECK(features.neon);
    CHECK(!features.avx && !features.avx2 && !features.fma);
#elif defined(__wasm__)
    CHECK(!features.sse2 && !features.neon && !features.avx && !features.avx2 && !features.fma);
#if defined(__wasm_simd128__)
    CHECK(features.simd128);
    CHECK(simd::hardware_level() == simd::Level::vector128);
#else
    CHECK(!features.simd128);
    CHECK(simd::hardware_level() == simd::Level::scalar);
#endif
#endif
#if !defined(__wasm__)
    CHECK(!features.simd128);
#endif
    const simd::Level hardware = simd::hardware_level();
    const bool full = features.avx2 && features.fma;
    CHECK((hardware == simd::Level::avx2_fma) == full);
#if defined(__x86_64__) || defined(__aarch64__)
    CHECK(hardware != simd::Level::scalar);
#endif
    std::printf("simd: sse2=%d neon=%d avx=%d avx2=%d fma=%d simd128=%d hardware=%s\n", features.sse2 ? 1 : 0,
                features.neon ? 1 : 0, features.avx ? 1 : 0, features.avx2 ? 1 : 0, features.fma ? 1 : 0,
                features.simd128 ? 1 : 0, text_of(simd::level_name(hardware)).c_str());
}

void names() {
    CHECK(simd::level_name(simd::Level::scalar) == stxlib::TextView("scalar"));
    CHECK(simd::level_name(simd::Level::avx2_fma) == stxlib::TextView("avx2-fma-256"));
    const stxlib::TextView vector = simd::level_name(simd::Level::vector128);
#if defined(__wasm__)
    CHECK(vector == stxlib::TextView("wasm-simd-128"));
#else
    CHECK(vector == stxlib::TextView("sse2-128") || vector == stxlib::TextView("neon-128"));
#endif
}

void request_fixes_the_level() {
    CHECK(stxlib::remove_variable("STX_SIMD_LEVEL").succeeded());
    CHECK(simd::request_level(simd::Level::scalar));
    const simd::Level first = simd::selected_level();
    CHECK(first == simd::Level::scalar);
    CHECK(!simd::request_level(simd::Level::avx2_fma));
    CHECK(simd::selected_level() == first);
    CHECK(!simd::request_level(static_cast<simd::Level>(7)));
}

} // namespace

int main() {
    features_are_consistent();
    names();
    request_fixes_the_level();
    return stxlib_tests::finish("stxlib simd");
}
