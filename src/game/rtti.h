// MSVC x64 run-time type information in the game executable: finds a class's
// virtual function table by its mangled name, so a binding can name "slot 10
// of Camera2::CameraTarget" instead of a byte pattern that breaks on every
// recompile.
//
//   TypeDescriptor           { void* vftable; void* spare; char name[]; }
//   CompleteObjectLocator    { u32 signature (1 on x64), u32 offset, u32 cdOffset,
//                              u32 typeDescriptor, u32 classDescriptor, u32 self } (image RVAs)
//   vtable[-1] = &CompleteObjectLocator, vtable[0..] = the virtual functions
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sm2m {
namespace rtti {

struct Span {
    uintptr_t addr = 0;
    size_t size = 0;
};

// The vtable of `mangled` (e.g. ".?AVCameraTarget@Camera2@@") whose locator
// has `offset` (0: the class's primary vtable), searching the readable data
// of an image loaded at `imageBase`. 0 when not found.
uintptr_t FindVtable(uintptr_t imageBase, const std::vector<Span>& data, const char* mangled, uint32_t offset = 0);

// The readable, non-executable sections of an image mapped at `imageBase`
// (full virtual size; .pdata, .rsrc and .reloc left out).
std::vector<Span> ImageDataSpans(uintptr_t imageBase);

} // namespace rtti
} // namespace sm2m
