// RTTI: typeid, dynamic_cast and dynamic_cast<void*> over the descriptors
// clang writes (MicrosoftCXXABI.cpp: emitRTtypeidCall, emitDynamicCastCall,
// emitDynamicCastToVoid, MSRTTIBuilder).
//
// Every polymorphic object's vftable is preceded by a pointer to its
// CompleteObjectLocator (MicrosoftCXXABI.cpp, getAddrOfVTable: the RTTI
// slot before the first virtual function). The locator's self reference
// gives the image base, its offset (and, under a vtordisp, the
// displacement stored before the virtual base) gives the complete object,
// and its hierarchy lists every base in pre-order with its displacement.
//
// clang passes the pointer already moved to the vfptr ("performBaseAdjustment")
// and, for dynamic_cast, the distance moved (VfDelta), so the source
// subobject is inptr - VfDelta.
//
// [expr.dynamic.cast]p8: a downcast succeeds when the source is a public
// base of exactly one T object; otherwise a cross cast succeeds when the
// source is a public base of the complete object and T an unambiguous
// public base of it. A failed reference cast throws std::bad_cast; typeid
// of a null pointer throws std::bad_typeid ([expr.typeid]p3). Both are
// thrown by the C++ library's hooks (cxx_hooks.cpp).

#include "runtime_state.hpp"

namespace stxlib::msvc_runtime {

namespace {

// A hierarchy larger than this is refused as corrupt before it is walked
// (the walk is quadratic in it).
constexpr i32 max_bases = 4096;

struct ObjectView final {
    Byte* complete;
    uptr image_base;
    const CompleteObjectLocator* locator;
    const ImageOffset* bases;
    i32 base_count;
};

const CompleteObjectLocator* locator_of(const void* const holder) noexcept {
    const void* const* const vtable = *static_cast<const void* const* const*>(holder);
    if (vtable == nullptr) return nullptr;
    const CompleteObjectLocator* const locator = static_cast<const CompleteObjectLocator*>(vtable[-1]);
    if (locator == nullptr || (*locator).signature != locator_image_relative) return nullptr;
    return locator;
}

uptr locator_image_base(const CompleteObjectLocator& locator) noexcept {
    const uptr address = reinterpret_cast<uptr>(&locator);
    const uptr result = address - static_cast<uptr>(locator.self.value);
    return result;
}

Byte* complete_object(Byte* const holder, const CompleteObjectLocator& locator) noexcept {
    Byte* result = holder - locator.offset;
    if (locator.constructor_displacement != 0) {
        const i32 vtordisp = *reinterpret_cast<const i32*>(holder - locator.constructor_displacement);
        result = result - vtordisp;
    }
    return result;
}

const TypeDescriptor* locator_type(const CompleteObjectLocator& locator) noexcept {
    const TypeDescriptor* const result =
        reinterpret_cast<const TypeDescriptor*>(resolve(locator_image_base(locator), locator.type));
    return result;
}

bool view_object(void* const holder, ObjectView& view) noexcept {
    const CompleteObjectLocator* const locator = locator_of(holder);
    if (locator == nullptr) return false;
    const uptr image_base = locator_image_base(*locator);
    const ClassHierarchyDescriptor* const hierarchy =
        reinterpret_cast<const ClassHierarchyDescriptor*>(resolve(image_base, (*locator).hierarchy));
    if (hierarchy == nullptr) return false;
    if ((*hierarchy).base_count < 1 || (*hierarchy).base_count > max_bases) return false;
    view.complete = complete_object(static_cast<Byte*>(holder), *locator);
    view.image_base = image_base;
    view.locator = locator;
    view.bases = reinterpret_cast<const ImageOffset*>(resolve(image_base, (*hierarchy).base_array));
    view.base_count = (*hierarchy).base_count;
    const bool result = view.bases != nullptr;
    return result;
}

const BaseClassDescriptor& base_at(const ObjectView& view, const i32 index) noexcept {
    const BaseClassDescriptor* const base = reinterpret_cast<const BaseClassDescriptor*>(resolve(view.image_base, view.bases[index]));
    if (base == nullptr) fail_fast();
    return *base;
}

bool base_is(const ObjectView& view, const i32 index, const TypeDescriptor& type) noexcept {
    const TypeDescriptor* const descriptor = reinterpret_cast<const TypeDescriptor*>(resolve(view.image_base, base_at(view, index).type));
    if (descriptor == nullptr) return false;
    const bool result = same_type(*descriptor, type);
    return result;
}

Byte* base_address(const ObjectView& view, const i32 index) noexcept {
    Byte* const result = displace(view.complete, base_at(view, index).displacement);
    return result;
}

// `ancestor` contains `index` in the pre-order base array.
bool contains(const ObjectView& view, const i32 ancestor, const i32 index) noexcept {
    const i32 last = ancestor + base_at(view, ancestor).contained_bases;
    const bool result = ancestor <= index && index <= last;
    return result;
}

// Every base specifier on the path from `ancestor` down to `index` is public.
bool public_path(const ObjectView& view, const i32 ancestor, const i32 index) noexcept {
    for (i32 step = ancestor + 1; step <= index; ++step) {
        if (!contains(view, step, index)) continue;
        if ((base_at(view, step).attributes & base_private) != 0) return false;
    }
    return true;
}

// The distinct addresses among a candidate set, kept as "none, one, many".
struct UniqueAddress final {
    Byte* address;
    i32 index;
    bool many;
};

void note_address(UniqueAddress& unique, Byte* const address, const i32 index) noexcept {
    if (unique.address == nullptr) {
        unique.address = address;
        unique.index = index;
        return;
    }
    if (unique.address != address) unique.many = true;
}

void* dynamic_cast_in(const ObjectView& view, Byte* const source, const TypeDescriptor& source_type, const TypeDescriptor& target_type) noexcept {
    // The source subobject: an entry of the source type at the source address.
    i32 source_index = -1;
    for (i32 index = 0; index < view.base_count && source_index < 0; ++index) {
        if (base_is(view, index, source_type) && base_address(view, index) == source) source_index = index;
    }
    if (source_index < 0) return nullptr;
    // Downcast: the T objects that contain the source.
    UniqueAddress down{nullptr, -1, false};
    for (i32 index = 0; index <= source_index; ++index) {
        if (!base_is(view, index, target_type) || !contains(view, index, source_index)) continue;
        note_address(down, base_address(view, index), index);
    }
    if (down.address != nullptr && !down.many && public_path(view, down.index, source_index)) return down.address;
    // Cross cast: the source public in the complete object, T unambiguous
    // and public in it.
    if (!public_path(view, 0, source_index)) return nullptr;
    UniqueAddress across{nullptr, -1, false};
    for (i32 index = 0; index < view.base_count; ++index) {
        if (!base_is(view, index, target_type)) continue;
        note_address(across, base_address(view, index), index);
    }
    if (across.address == nullptr || across.many) return nullptr;
    if (!public_path(view, 0, across.index)) return nullptr;
    return across.address;
}

} // namespace

} // namespace stxlib::msvc_runtime

extern "C" {

void* __RTtypeid(void* const inptr) {
    using namespace stxlib::msvc_runtime;
    if (inptr == nullptr) raise_bad_typeid();
    const CompleteObjectLocator* const locator = locator_of(inptr);
    if (locator == nullptr) raise_bad_typeid();
    const TypeDescriptor* const type = locator_type(*locator);
    if (type == nullptr) raise_bad_typeid();
    return const_cast<TypeDescriptor*>(type);
}

void* __RTCastToVoid(void* const inptr) {
    using namespace stxlib::msvc_runtime;
    if (inptr == nullptr) return nullptr;
    const CompleteObjectLocator* const locator = locator_of(inptr);
    if (locator == nullptr) raise_bad_typeid();
    void* const result = complete_object(static_cast<stxlib::Byte*>(inptr), *locator);
    return result;
}

void* __RTDynamicCast(void* const inptr, const long vf_delta, void* const source_type, void* const target_type, const int is_reference) {
    using namespace stxlib::msvc_runtime;
    if (inptr == nullptr) return nullptr;
    ObjectView view{};
    if (!view_object(inptr, view)) raise_bad_typeid();
    stxlib::Byte* const source = static_cast<stxlib::Byte*>(inptr) - vf_delta;
    const TypeDescriptor& source_descriptor = *static_cast<const TypeDescriptor*>(source_type);
    const TypeDescriptor& target_descriptor = *static_cast<const TypeDescriptor*>(target_type);
    void* const result = dynamic_cast_in(view, source, source_descriptor, target_descriptor);
    if (result == nullptr && is_reference != 0) raise_bad_cast();
    return result;
}

} // extern "C"
