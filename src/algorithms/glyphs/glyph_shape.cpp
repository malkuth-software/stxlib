// stxlib::glyphs: shaping (include/stxlib/glyph_shape.hpp): the plans kept
// for a typeface, and a run through shape_text.hpp.

#include "stxlib/glyph_shape.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "algorithms/glyphs/shape_text.hpp"
#include "algorithms/glyphs/shape_unicode.hpp"

namespace stxlib::glyphs {

namespace detail {

// A plan kept, with what it was made for.
struct KeptPlan final {
    ShapePlan plan{};
    Vector<PlanRequest> requests{};
    usize last_used{0};
};

// A glyph's box remembered for a run: read once from the typeface, or
// refused once.
struct BoxEntry final {
    u32 glyph{0};
    bool used{false};
    bool found{false};
    GlyphBox box{};
};
inline constexpr usize box_memo_size = 64;
// What reading a box from the typeface is charged to the run.
inline constexpr usize box_charge = 64;

struct ShaperState final {
    Shared<Typeface> typeface{};
    const Font* font{nullptr};
    ShaperLimits limits{};
    Vector<KeptPlan> plans{};
    Vector<PlanRequest> requests{};
    ShapeBuffer buffer{};
    ApplyWork work{};
    usize uses{0};
    usize made{0};
    // The size boxes are asked at, and what they are divided by.
    i64 box_size{0};
    i32 box_divisor{1};
    // The boxes of this run (emptied as each run begins).
    BoxEntry boxes[box_memo_size]{};
};

} // namespace detail

namespace {

using detail::refuse_font;
using detail::ShaperState;

inline constexpr i64 largest_size = static_cast<i64>(1) << 24;

// A glyph's box from the typeface, in the units of the shaping: once a run
// for each glyph that the run's table of boxes holds, each reading charged
// to the run's operations.
[[nodiscard]] bool box_of(void* const context, const u32 glyph, detail::GlyphBox& box) noexcept {
    ShaperState& state = *static_cast<ShaperState*>(context);
    // Fibonacci hashing: the top six bits of the low 32 of glyph x 2^32/phi,
    // taken in 64 bits so that nothing wraps.
    const u64 mixed = (static_cast<u64>(glyph) * 0x9E3779B1U) & 0xFFFFFFFFU;
    detail::BoxEntry& entry = state.boxes[static_cast<usize>(mixed >> 26)];
    if (entry.used && entry.glyph == glyph) {
        box = entry.box;
        return entry.found;
    }
    if (!state.buffer.charge(detail::box_charge)) return false;
    InkBox ink{};
    const bool found = (*state.typeface).ink_box(glyph, state.box_size, ink).succeeded();
    detail::GlyphBox made{};
    if (found) {
        // Sides of 32 bits, a width or height of 33: taken in 64 and
        // saturated.
        const i64 divisor = state.box_divisor;
        made.x_bearing = detail::saturate(static_cast<i64>(ink.left) / divisor);
        made.y_bearing = detail::saturate(static_cast<i64>(ink.top) / divisor);
        made.width = detail::saturate((static_cast<i64>(ink.right) - ink.left) / divisor);
        made.height = detail::saturate((static_cast<i64>(ink.bottom) - ink.top) / divisor);
    }
    entry = detail::BoxEntry{.glyph = glyph, .used = true, .found = found, .box = made};
    box = made;
    return found;
}

// A run that made the buffer far larger than its text gives the room back,
// so that what a shaper keeps between runs is of the order of its runs.
void give_back(ShaperState& state, const usize characters) noexcept {
    const usize room = state.buffer.info.count();
    const usize most_kept = static_cast<usize>(1) << 16;
    if (room > most_kept && room / 4 > characters) state.buffer.release();
}

[[nodiscard]] bool same_requests(const Vector<detail::PlanRequest>& first, const Vector<detail::PlanRequest>& second) noexcept {
    bool same = first.count() == second.count();
    for (usize index = 0; same && index < first.count(); ++index) {
        same = first[index].tag == second[index].tag && first[index].value == second[index].value && first[index].start == second[index].start &&
               first[index].end == second[index].end;
    }
    return same;
}

// The plan for the run's direction, script, language and features: one
// kept, or made now in the place of the one longest unused.
[[nodiscard]] OperationStatus plan_for(ShaperState& state, const detail::ShapeDirection direction, const u32 script, const u32 language,
                              const detail::ShapePlan*& plan) noexcept {
    const Operation operation("stxlib.glyphs.shaper");
    plan = nullptr;
    state.uses = state.uses + 1;
    usize oldest = 0;
    for (usize index = 0; index < state.plans.count(); ++index) {
        detail::KeptPlan& kept = state.plans[index];
        if (kept.plan.direction == direction && kept.plan.script == script && kept.plan.language == language && same_requests(kept.requests, state.requests)) {
            kept.last_used = state.uses;
            plan = &kept.plan;
            return OperationStatus::success();
        }
        if (kept.last_used < state.plans[oldest].last_used) oldest = index;
    }
    const usize most = state.limits.plans == 0 ? 1 : state.limits.plans;
    usize place = oldest;
    if (state.plans.count() < most) {
        if (state.plans.emplace().failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        place = state.plans.count() - 1;
    }
    detail::KeptPlan& kept = state.plans[place];
    kept.last_used = state.uses;
    kept.requests.clear();
    kept.plan = detail::ShapePlan{};
    for (usize index = 0; index < state.requests.count(); ++index) {
        if (kept.requests.push_copy(state.requests[index]).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    }
    const OperationStatus compiled = detail::compile_plan(*state.font, direction, script, language, state.requests.view(), kept.plan);
    if (compiled.failed()) {
        // A plan half made is no plan: its place is for the next.
        kept.plan = detail::ShapePlan{};
        kept.requests.clear();
        kept.last_used = 0;
        return compiled;
    }
    state.made = state.made + 1;
    kept.plan.serial = state.made;
    plan = &kept.plan;
    return OperationStatus::success();
}

} // namespace

Shaper::Shaper() noexcept = default;
Shaper::Shaper(Shaper&&) noexcept = default;
Shaper& Shaper::operator=(Shaper&&) noexcept = default;
Shaper::~Shaper() noexcept = default;

OperationStatus Shaper::open(const Shared<Typeface>& typeface, const ShaperLimits& limits) noexcept {
    const Operation operation("stxlib.glyphs.shaper");
    state_.reset();
    if (typeface.empty() || (*typeface).font() == nullptr) return refuse_font(ErrorKind::invalid_argument, operation);
    OperationResult<Unique<ShaperState>> made = Unique<ShaperState>::make();
    if (made.failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    state_ = static_cast<OperationResult<Unique<ShaperState>>&&>(made).take_value();
    ShaperState& state = *state_;
    state.typeface = typeface;
    state.font = (*typeface).font();
    state.limits = limits;
    return OperationStatus::success();
}

OperationStatus Shaper::shape(const TextRun& run, Vector<ShapedGlyph>& glyphs) noexcept {
    const OperationStatus status = shape_into(run, glyphs);
    if (status.failed()) glyphs.clear();
    return status;
}

OperationStatus Shaper::shape_into(const TextRun& run, Vector<ShapedGlyph>& glyphs) noexcept {
    const Operation operation("stxlib.glyphs.shaper");
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    ShaperState& state = *state_;
    const usize count = run.characters.count();
    if ((run.clusters.count() != 0 && run.clusters.count() != count) || run.size < 0 || run.size > largest_size) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    if (count > state.limits.characters || count > 0x3FFFFFFFU || run.features.count() > state.limits.features) {
        return refuse_font(ErrorKind::limit_exceeded, operation);
    }
    if (count == 0) {
        glyphs.clear();
        return OperationStatus::success();
    }

    detail::ShapeBuffer& buffer = state.buffer;
    buffer.reset();
    for (usize index = 0; index < count; ++index) {
        buffer.add(run.characters[index], run.clusters.count() != 0 ? run.clusters[index] : static_cast<u32>(index));
    }
    if (!buffer.successful) return refuse_font(ErrorKind::out_of_memory, operation);
    buffer.direction = run.direction == TextDirection::right_to_left ? detail::ShapeDirection::right_to_left : detail::ShapeDirection::left_to_right;
    buffer.script = run.script;
    detail::guess_segment_properties(buffer);

    state.requests.clear();
    for (usize index = 0; index < run.features.count(); ++index) {
        const TextFeature& feature = run.features[index];
        const detail::PlanRequest request{.tag = feature.tag, .value = feature.value, .start = feature.start, .end = feature.end};
        if (state.requests.push_copy(request).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    }
    const detail::ShapePlan* plan = nullptr;
    const OperationStatus planned = plan_for(state, buffer.direction, buffer.script, run.language, plan);
    if (planned.failed()) return planned;

    detail::ShapeFont font = detail::shape_font_of(*state.font, run.size);
    font.boxes = box_of;
    font.context = &state;
    // In font units a box is asked at the size where a unit is a pixel,
    // and comes back in 64ths of one.
    state.box_size = run.size > 0 ? run.size : static_cast<i64>((*state.font).units_per_em) * 64;
    state.box_divisor = run.size > 0 ? 1 : 64;
    for (usize index = 0; index < detail::box_memo_size; ++index) state.boxes[index] = detail::BoxEntry{};
    buffer.most_glyphs = state.limits.glyphs;
    const bool shaped = detail::shape_run(*plan, font, buffer, state.work);
    if (!shaped) {
        const ErrorKind kind = buffer.memory_failed ? ErrorKind::out_of_memory : ErrorKind::limit_exceeded;
        buffer.reset();
        give_back(state, count);
        return refuse_font(kind, operation);
    }
    if (glyphs.resize(buffer.len).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    for (usize index = 0; index < buffer.len; ++index) {
        const detail::ShapeGlyph& item = buffer.info[index];
        const detail::ShapePosition& position = buffer.pos[index];
        glyphs[index] = ShapedGlyph{.glyph = item.glyph,
                                    .cluster = item.cluster,
                                    .x_advance = position.x_advance,
                                    .y_advance = position.y_advance,
                                    .x_offset = position.x_offset,
                                    .y_offset = position.y_offset,
                                    .unsafe_to_break = (item.mask & detail::glyph_unsafe_to_break) != 0};
    }
    give_back(state, count);
    return OperationStatus::success();
}

usize Shaper::plans_made() const noexcept {
    const usize result = state_.empty() ? 0 : (*state_).made;
    return result;
}

usize Shaper::bytes_held() const noexcept {
    if (state_.empty()) return 0;
    const ShaperState& state = *state_;
    usize bytes = sizeof(ShaperState) + state.plans.capacity() * sizeof(detail::KeptPlan) + state.requests.capacity() * sizeof(detail::PlanRequest) +
                  state.buffer.info.capacity() * sizeof(detail::ShapeGlyph) + state.buffer.out.capacity() * sizeof(detail::ShapeGlyph) +
                  state.buffer.pos.capacity() * sizeof(detail::ShapePosition) + state.work.frames.capacity() * sizeof(detail::ApplyFrame) +
                  state.work.memo.capacity() * sizeof(detail::MemoEntry) + state.work.pairs.capacity() * sizeof(detail::PairEntry);
    for (usize index = 0; index < state.plans.count(); ++index) {
        bytes = bytes + detail::bytes_of(state.plans[index].plan) - sizeof(detail::ShapePlan) +
                state.plans[index].requests.capacity() * sizeof(detail::PlanRequest);
    }
    return bytes;
}

void Shaper::release() noexcept { state_.reset(); }

} // namespace stxlib::glyphs
