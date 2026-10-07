// The original (un-hooked) ID3D12GraphicsCommandList / ID3D12Device entry
// points. The frame tracker fills these in when it installs its hooks; the
// injector records its commands into the game's lists through them, so its
// own calls never re-enter the hooks.
#pragma once

#ifdef _WIN32

#include "d3d.h"

namespace sm2m {

using GCL = ID3D12GraphicsCommandList;

struct ListFns {
    HRESULT(STDMETHODCALLTYPE* Close)(GCL*) = nullptr;
    HRESULT(STDMETHODCALLTYPE* Reset)(GCL*, ID3D12CommandAllocator*, ID3D12PipelineState*) = nullptr;
    void(STDMETHODCALLTYPE* ClearState)(GCL*, ID3D12PipelineState*) = nullptr;
    void(STDMETHODCALLTYPE* DrawInstanced)(GCL*, UINT, UINT, UINT, UINT) = nullptr;
    void(STDMETHODCALLTYPE* DrawIndexedInstanced)(GCL*, UINT, UINT, UINT, INT, UINT) = nullptr;
    void(STDMETHODCALLTYPE* Dispatch)(GCL*, UINT, UINT, UINT) = nullptr;
    void(STDMETHODCALLTYPE* CopyBufferRegion)(GCL*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) = nullptr;
    void(STDMETHODCALLTYPE* CopyTextureRegion)(GCL*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                               const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*) = nullptr;
    void(STDMETHODCALLTYPE* IASetPrimitiveTopology)(GCL*, D3D12_PRIMITIVE_TOPOLOGY) = nullptr;
    void(STDMETHODCALLTYPE* RSSetViewports)(GCL*, UINT, const D3D12_VIEWPORT*) = nullptr;
    void(STDMETHODCALLTYPE* RSSetScissorRects)(GCL*, UINT, const D3D12_RECT*) = nullptr;
    void(STDMETHODCALLTYPE* OMSetStencilRef)(GCL*, UINT) = nullptr;
    void(STDMETHODCALLTYPE* SetPipelineState)(GCL*, ID3D12PipelineState*) = nullptr;
    void(STDMETHODCALLTYPE* ResourceBarrier)(GCL*, UINT, const D3D12_RESOURCE_BARRIER*) = nullptr;
    void(STDMETHODCALLTYPE* SetDescriptorHeaps)(GCL*, UINT, ID3D12DescriptorHeap* const*) = nullptr;
    void(STDMETHODCALLTYPE* SetComputeRootSignature)(GCL*, ID3D12RootSignature*) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRootSignature)(GCL*, ID3D12RootSignature*) = nullptr;
    void(STDMETHODCALLTYPE* SetComputeRootDescriptorTable)(GCL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRootDescriptorTable)(GCL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRoot32BitConstant)(GCL*, UINT, UINT, UINT) = nullptr;
    void(STDMETHODCALLTYPE* SetComputeRoot32BitConstants)(GCL*, UINT, UINT, const void*, UINT) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRoot32BitConstants)(GCL*, UINT, UINT, const void*, UINT) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRootConstantBufferView)(GCL*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) = nullptr;
    void(STDMETHODCALLTYPE* SetComputeRootShaderResourceView)(GCL*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRootShaderResourceView)(GCL*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) = nullptr;
    void(STDMETHODCALLTYPE* SetComputeRootUnorderedAccessView)(GCL*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) = nullptr;
    void(STDMETHODCALLTYPE* SetGraphicsRootUnorderedAccessView)(GCL*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) = nullptr;
    void(STDMETHODCALLTYPE* IASetVertexBuffers)(GCL*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*) = nullptr;
    void(STDMETHODCALLTYPE* OMSetRenderTargets)(GCL*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                const D3D12_CPU_DESCRIPTOR_HANDLE*) = nullptr;
    void(STDMETHODCALLTYPE* ClearDepthStencilView)(GCL*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8,
                                                   UINT, const D3D12_RECT*) = nullptr;
    void(STDMETHODCALLTYPE* ClearRenderTargetView)(GCL*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT,
                                                   const D3D12_RECT*) = nullptr;
    void(STDMETHODCALLTYPE* SetPredication)(GCL*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) = nullptr;
    void(STDMETHODCALLTYPE* BeginEvent)(GCL*, UINT, const void*, UINT) = nullptr;
    void(STDMETHODCALLTYPE* EndEvent)(GCL*) = nullptr;
    void(STDMETHODCALLTYPE* ExecuteIndirect)(GCL*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                             ID3D12Resource*, UINT64) = nullptr;
    void(STDMETHODCALLTYPE* ExecuteBundle)(GCL*, ID3D12GraphicsCommandList*) = nullptr;
    // ID3D12GraphicsCommandList4 (render pass descs are passed through untouched)
    void(STDMETHODCALLTYPE* BeginRenderPass)(GCL*, UINT, const void*, const void*, UINT) = nullptr;
    void(STDMETHODCALLTYPE* EndRenderPass)(GCL*) = nullptr;
    void(STDMETHODCALLTYPE* SetPipelineState1)(GCL*, void*) = nullptr; // ID3D12StateObject*
    // ID3D12GraphicsCommandList7
    void(STDMETHODCALLTYPE* Barrier)(GCL*, UINT32, const void*) = nullptr;
    // ID3D12GraphicsCommandList8
    void(STDMETHODCALLTYPE* OMSetFrontAndBackStencilRef)(GCL*, UINT, UINT) = nullptr;
};

// D3D12_RENDER_PASS_RENDER_TARGET_DESC / _DEPTH_STENCIL_DESC sizes (x64);
// cpuDescriptor is the first member of both.
constexpr size_t kRenderPassRtDescSize = 88;
constexpr size_t kRenderPassDsDepthAccessOffset = 8;    // DepthBeginningAccess.Type
constexpr size_t kRenderPassDsStencilAccessOffset = 32; // StencilBeginningAccess.Type (after 24 bytes of depth's)

const ListFns& OriginalListFns();

} // namespace sm2m

#endif
