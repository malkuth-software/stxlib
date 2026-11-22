// The speed of STXlib's glyph rasterizer (include/stxlib/glyph_mask.hpp) on
// a published same-task benchmark, so that the numbers can be set beside
// another rasterizer's: one glyph of one font drawn to 8-bit grey coverage
// at sizes from 10 to 100 pixels to the em, in microseconds a glyph. Only
// numbers are compared; nothing of the other rasterizer is run or carried
// over. Built only with STXLIB_GLYPH_RASTER_BENCH; never run by ctest.
//
// Usage: stxlib_glyph_raster_bench --font <path> [--glyph g] [--face N]
//        [--machine "<CPU model>"] [--skip-hinted] [--skip-large]
//
// The published method, followed here:
// - The glyph's outline is read, decoded and scaled to the size before the
//   timed loop (Face::fitted), and its mask's box measured
//   (glyphs::measure), and the mask's bytes allocated. Nothing in the loop
//   allocates: the Rasterizer keeps its working storage from the warm-up.
// - Timed, each iteration: Rasterizer::render, which clears the coverage
//   cells, flattens the outline's curves to chords, accumulates each
//   chord's signed area into the cells and turns the cells into the bytes
//   of the mask (every byte of the mask is written).
// - 5000 untimed iterations first, then 101 samples of 1000 iterations,
//   each sample's time divided by 1000; min, median, p90 and max of the
//   101, in microseconds a glyph.
// Where it differs from the published timed region (stated so in the
// queue entry and printed in the header): the other rasterizer flattens
// its outline to edges once, outside the loop, and transforms the edges in
// the loop; here the outline is scaled outside the loop (to 26.6 fixed
// point pixels, as the unhinted scaler gives it) and the curves are
// flattened inside it, because render takes an outline of curves and has
// no entry point for chords made beforehand. The loop therefore does more
// than the published one, not less.
//
// Tables: unhinted, 10 to 100 by 5 (the published sizes); light hinting at
// the same sizes ("ours only": the outline hinted along y before the
// loop, the loop as above); unhinted, 150 to 1000 by 50.

#include "stxlib/font_file.hpp"
#include "stxlib/glyph_face.hpp"
#include "stxlib/glyph_mask.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#elif defined(__linux__) || defined(__unix__)
#include <sys/utsname.h>
#endif

#ifndef STXLIB_BENCH_FLAGS
#define STXLIB_BENCH_FLAGS "(not given by the build)"
#endif

namespace {

namespace glyphs = stxlib::glyphs;
using stxlib::i32;
using stxlib::i64;
using stxlib::u32;
using stxlib::u64;
using stxlib::u8;
using stxlib::usize;

constexpr int warmup_iterations = 5000;
constexpr int sample_count = 101;
constexpr int iterations_per_sample = 1000;

u64 sink = 0;

struct Summary final {
    double min{0.0};
    double median{0.0};
    double p90{0.0};
    double max{0.0};
};

struct Options final {
    std::string font_path{};
    u32 character{'g'};
    u32 face_index{0};
    std::string machine{};
    bool hinted{true};
    bool large{true};
};

bool read_file(const std::string& path, std::vector<u8>& bytes) {
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    u8 block[65536];
    usize got = sizeof block;
    while (got == sizeof block) {
        got = std::fread(block, 1, sizeof block, file);
        bytes.insert(bytes.end(), block, block + got);
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    return !failed && !bytes.empty();
}

// The first character of `text`, UTF-8. False for text that is empty or
// does not begin with a whole, well-formed character.
bool first_character(const char* const text, u32& character) {
    const u8 lead = static_cast<u8>(text[0]);
    if (lead == 0) return false;
    if (lead < 0x80) {
        character = lead;
        return true;
    }
    int more = 0;
    u32 value = 0;
    if ((lead & 0xE0) == 0xC0) {
        more = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        more = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        more = 3;
        value = lead & 0x07;
    } else {
        return false;
    }
    for (int index = 1; index <= more; ++index) {
        const u8 next = static_cast<u8>(text[index]);
        if ((next & 0xC0) != 0x80) return false;
        value = (value << 6) | (next & 0x3F);
    }
    character = value;
    return value <= 0x10FFFF;
}

std::string cpu_model() {
#if defined(__APPLE__)
    char brand[256];
    size_t length = sizeof brand;
    if (sysctlbyname("machdep.cpu.brand_string", brand, &length, nullptr, 0) == 0 && length > 0) {
        brand[sizeof brand - 1] = '\0';
        return std::string(brand);
    }
#elif defined(__linux__)
    std::FILE* const file = std::fopen("/proc/cpuinfo", "r");
    if (file != nullptr) {
        char line[512];
        std::string found;
        while (found.empty() && std::fgets(line, sizeof line, file) != nullptr) {
            const bool named = std::strncmp(line, "model name", 10) == 0 || std::strncmp(line, "Hardware", 8) == 0;
            const char* const colon = std::strchr(line, ':');
            if (named && colon != nullptr) {
                std::string value(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
                while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) value.pop_back();
                found = value;
            }
        }
        std::fclose(file);
        if (!found.empty()) return found;
    }
#endif
    return std::string("unknown (give it with --machine)");
}

std::string os_name() {
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    struct utsname names;
    if (uname(&names) == 0) {
        const std::string result = std::string(names.sysname) + " " + names.release + " " + names.machine;
        return result;
    }
    return std::string("unknown POSIX");
#elif defined(_WIN32)
    return std::string("Windows");
#else
    return std::string("unknown");
#endif
}

std::string compiler_name() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return std::string("MSVC ") + std::to_string(_MSC_VER);
#else
    return std::string("unknown");
#endif
}

// Times the timed region for one outline: 5000 iterations untimed, then
// 101 samples of 1000. False if a render failed.
bool time_outline(glyphs::Rasterizer& rasterizer, const glyphs::Outline& outline, const glyphs::Bounds& bounds,
                  const glyphs::Limits& limits, stxlib::MutableView<u8> mask, Summary& summary) {
    const glyphs::Point origin{};
    bool rendered = true;
    for (int index = 0; index < warmup_iterations; ++index) {
        const stxlib::OperationStatus status = rasterizer.render(outline, origin, bounds, limits, mask);
        rendered = rendered && status.succeeded();
    }
    if (!rendered) return false;
    std::vector<double> samples(static_cast<usize>(sample_count), 0.0);
    for (int sample = 0; sample < sample_count; ++sample) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (int index = 0; index < iterations_per_sample; ++index) {
            const stxlib::OperationStatus status = rasterizer.render(outline, origin, bounds, limits, mask);
            rendered = rendered && status.succeeded();
        }
        const std::chrono::steady_clock::time_point stop = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::micro> elapsed = stop - start;
        samples[static_cast<usize>(sample)] = elapsed.count() / static_cast<double>(iterations_per_sample);
        if (mask.count() > 0) sink += mask[mask.count() / 2];
    }
    std::sort(samples.begin(), samples.end());
    // Nearest rank: the median is the 51st of 101, p90 the 91st.
    summary.min = samples[0];
    summary.median = samples[static_cast<usize>(sample_count / 2)];
    summary.p90 = samples[static_cast<usize>((sample_count * 9 + 9) / 10 - 1)];
    summary.max = samples[static_cast<usize>(sample_count - 1)];
    return rendered;
}

// One table: each size from `first` to `last` by `step` pixels to the em.
bool run_table(glyphs::Face& face, const u32 glyph, const glyphs::Fit fit, const int first, const int last,
               const int step, const char* const title) {
    const glyphs::Limits limits{};
    glyphs::Rasterizer rasterizer;
    std::vector<u8> pixels;
    std::printf("\n%s\n", title);
    std::printf("%6s %11s %10s %10s %10s %10s\n", "px/em", "mask", "min", "median", "p90", "max");
    for (int size = first; size <= last; size += step) {
        glyphs::FittedGlyph fitted;
        // Outside the timed region: the outline read, decoded and scaled
        // (and, for Fit::light, hinted), its box measured, the mask
        // allocated.
        const stxlib::OperationStatus had = face.fitted(glyph, static_cast<i64>(size) * 64, fit, fitted);
        if (had.failed()) {
            std::printf("%6d  the glyph could not be fitted\n", size);
            return false;
        }
        glyphs::Bounds bounds;
        const stxlib::OperationStatus measured = glyphs::measure(fitted.outline, glyphs::Point{}, limits, bounds);
        if (measured.failed()) {
            std::printf("%6d  the mask could not be measured\n", size);
            return false;
        }
        pixels.assign(static_cast<usize>(bounds.width) * bounds.height, 0);
        const stxlib::MutableView<u8> mask(pixels.data(), pixels.size());
        Summary summary;
        const bool timed = time_outline(rasterizer, fitted.outline, bounds, limits, mask, summary);
        if (!timed) {
            std::printf("%6d  render failed\n", size);
            return false;
        }
        char box[32];
        std::snprintf(box, sizeof box, "%ux%u", static_cast<unsigned>(bounds.width), static_cast<unsigned>(bounds.height));
        std::printf("%6d %11s %10.4f %10.4f %10.4f %10.4f\n", size, box, summary.min, summary.median, summary.p90,
                    summary.max);
    }
    return true;
}

void print_usage() {
    std::printf("usage: stxlib_glyph_raster_bench --font <path> [--glyph g] [--face N] [--machine \"<CPU>\"] "
                "[--skip-hinted] [--skip-large]\n");
}

bool parse_options(const int argc, char** const argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const char* const argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (std::strcmp(argument, "--font") == 0 && has_value) {
            options.font_path = argv[++index];
        } else if (std::strcmp(argument, "--glyph") == 0 && has_value) {
            if (!first_character(argv[++index], options.character)) return false;
        } else if (std::strcmp(argument, "--face") == 0 && has_value) {
            options.face_index = static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(argument, "--machine") == 0 && has_value) {
            options.machine = argv[++index];
        } else if (std::strcmp(argument, "--skip-hinted") == 0) {
            options.hinted = false;
        } else if (std::strcmp(argument, "--skip-large") == 0) {
            options.large = false;
        } else {
            return false;
        }
    }
    return !options.font_path.empty();
}

} // namespace

int main(const int argc, char** const argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }
    std::vector<u8> bytes;
    if (!read_file(options.font_path, bytes)) {
        std::printf("cannot read %s\n", options.font_path.c_str());
        return 1;
    }
    glyphs::Face face;
    const stxlib::OperationStatus opened =
        face.open(stxlib::View<u8>(bytes.data(), bytes.size()), options.face_index);
    if (opened.failed() || face.font() == nullptr) {
        std::printf("cannot open %s as a font\n", options.font_path.c_str());
        return 1;
    }
    const u32 glyph = glyphs::glyph_of(*face.font(), options.character);
    if (glyph == 0) {
        std::printf("the font has no glyph for U+%04X\n", static_cast<unsigned>(options.character));
        return 1;
    }
    const std::string machine = options.machine.empty() ? cpu_model() : options.machine;

    std::printf("stxlib_glyph_raster_bench: STXlib glyph rasterizer (glyph_mask.hpp, Rasterizer::render)\n");
    std::printf("machine   %s\n", machine.c_str());
    std::printf("os        %s\n", os_name().c_str());
    std::printf("compiler  %s\n", compiler_name().c_str());
    std::printf("flags     %s\n", STXLIB_BENCH_FLAGS);
    std::printf("threads   1\n");
    std::printf("font      %s (face %u), U+%04X, glyph %u, %u units/em\n", options.font_path.c_str(),
                static_cast<unsigned>(options.face_index), static_cast<unsigned>(options.character),
                static_cast<unsigned>(glyph), static_cast<unsigned>((*face.font()).units_per_em));
    std::printf("method    %d untimed iterations, then %d samples of %d iterations; microseconds a glyph\n",
                warmup_iterations, sample_count, iterations_per_sample);
    std::printf("timed     clear the coverage cells, flatten the curves to chords, accumulate the chords' areas,\n"
                "          write the 8-bit mask (Rasterizer::render, every byte written)\n");
    std::printf("untimed   read and parse the font, scale the outline to 26.6 pixels (hint it for Fit::light),\n"
                "          measure the mask's box, allocate the mask\n");
    std::printf("differs   the published loop flattens once outside the loop; this one flattens in it\n");
    std::printf("published font-zy median 0.5088 us at 10 px/em, 2.9586 at 100; fontdue 0.534 and 3.773\n"
                "          (AMD Zen 3 Cezanne, 3.2 GHz, Zig -Doptimize=fast, one thread)\n");

    bool good = run_table(face, glyph, glyphs::Fit::none, 10, 100, 5,
                          "unhinted grey coverage, 10 to 100 px/em (the published sizes), microseconds a glyph");
    if (good && options.hinted) {
        good = run_table(face, glyph, glyphs::Fit::light, 10, 100, 5,
                         "light hinting (ours only: hinted before the loop), 10 to 100 px/em, microseconds a glyph");
    }
    if (good && options.large) {
        good = run_table(face, glyph, glyphs::Fit::none, 150, 1000, 50,
                         "unhinted grey coverage, 150 to 1000 px/em, microseconds a glyph");
    }
    std::printf("\nsink %llu\n", static_cast<unsigned long long>(sink));
    return good ? 0 : 1;
}
