// Hooks DXGI Present/ResizeBuffers and ID3D12CommandQueue::ExecuteCommandLists.
//
// Function addresses are taken from the vtables of throwaway objects created
// on the WARP adapter (so the game's hardware device singleton is never
// touched), then patched with MinHook. Every swap chain in the process goes
// through the hooks; the mod only draws on the one attached to the game's
// window.
#pragma once

#ifdef _WIN32

#include <string>

#include "d3d.h"

namespace sm2m::d3d12hook {

struct Callbacks {
    void (*present)(IDXGISwapChain* sc) = nullptr;
    void (*resizeBegin)(IDXGISwapChain* sc) = nullptr;
    void (*colorSpace)(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs) = nullptr;
};

bool Install(const Callbacks& cb, std::string& error);
// The DIRECT queue the game most recently submitted work on for `device`.
// Returned with an added reference (caller releases), or nullptr.
ID3D12CommandQueue* LastDirectQueue(ID3D12Device* device);
// Marks the calling thread as "inside our own ExecuteCommandLists" so our
// submissions are not recorded as the game's queue.
void SetInternalSubmit(bool v);

} // namespace sm2m::d3d12hook

#endif
