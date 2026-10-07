#ifdef _WIN32

#include "d3d12_hook.h"

#include <string>

#include "../common/log.h"
#include "MinHook.h"
#include "frame_tracker.h"

namespace sm2m::d3d12hook {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                     const UINT*, IUnknown* const*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resize = nullptr;
ResizeBuffers1Fn g_resize1 = nullptr;
SetColorSpace1Fn g_colorSpace = nullptr;
ExecuteFn g_execute = nullptr;
Callbacks g_cb;

thread_local int t_inPresent = 0;
thread_local bool t_internal = false;

Mutex g_queueMutex;
ID3D12CommandQueue* g_queue = nullptr; // AddRef'd
ID3D12Device* g_queueDevice = nullptr; // not AddRef'd, identity only

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_inPresent == 0 && !(flags & DXGI_PRESENT_TEST) && g_cb.present) {
        ++t_inPresent;
        g_cb.present(sc);
        --t_inPresent;
    }
    ++t_inPresent;
    HRESULT hr = g_present(sc, sync, flags);
    --t_inPresent;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_inPresent == 0 && !(flags & DXGI_PRESENT_TEST) && g_cb.present) {
        ++t_inPresent;
        g_cb.present(sc);
        --t_inPresent;
    }
    ++t_inPresent;
    HRESULT hr = g_present1(sc, sync, flags, p);
    --t_inPresent;
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    if (g_cb.resizeBegin) g_cb.resizeBegin(sc);
    return g_resize(sc, n, w, h, f, flags);
}

HRESULT STDMETHODCALLTYPE HookResize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                      const UINT* mask, IUnknown* const* queues) {
    if (g_cb.resizeBegin) g_cb.resizeBegin(sc);
    return g_resize1(sc, n, w, h, f, flags, mask, queues);
}

HRESULT STDMETHODCALLTYPE HookColorSpace(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs) {
    HRESULT hr = g_colorSpace(sc, cs);
    if (SUCCEEDED(hr) && g_cb.colorSpace) g_cb.colorSpace(sc, cs);
    return hr;
}

void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!t_internal && q) tracker::OnExecute(q, n, lists);
    if (!t_internal && q) {
        D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            LockGuard lock(g_queueMutex);
            if (g_queue != q) {
                ID3D12Device* dev = nullptr;
                if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev)))) {
                    if (g_queue) g_queue->Release();
                    q->AddRef();
                    g_queue = q;
                    g_queueDevice = dev;
                    dev->Release();
                }
            }
        }
    }
    g_execute(q, n, lists);
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

void* VtableEntry(void* obj, int index) { return (*reinterpret_cast<void***>(obj))[index]; }

bool Hook(void* target, void* detour, void** original, const char* name, std::string& error) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        error = std::string("MH_CreateHook(") + name + ") failed";
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        error = std::string("MH_EnableHook(") + name + ") failed";
        return false;
    }
    return true;
}

} // namespace

void SetInternalSubmit(bool v) { t_internal = v; }

ID3D12CommandQueue* LastDirectQueue(ID3D12Device* device) {
    LockGuard lock(g_queueMutex);
    if (!g_queue || (device && g_queueDevice != device)) return nullptr;
    g_queue->AddRef();
    return g_queue;
}

bool Install(const Callbacks& cb, std::string& error) {
    g_cb = cb;
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d12 || !dxgi) {
        error = "d3d12.dll/dxgi.dll not available";
        return false;
    }
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto createFactory = reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    auto createDevice = reinterpret_cast<CreateDeviceFn>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
    if (!createFactory || !createDevice) {
        error = "missing CreateDXGIFactory1/D3D12CreateDevice";
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sm2mario_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        error = "could not create dummy window";
        return false;
    }

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    bool ok = false;
    do {
        if (FAILED(createFactory(IID_PPV_ARGS(&factory)))) {
            error = "CreateDXGIFactory1 failed";
            break;
        }
        // Prefer WARP so the game's hardware device is never touched; fall back to
        // the default adapter where WARP isn't available (e.g. Wine/Proton). A
        // D3D12 device per adapter is a process singleton, so this is harmless.
        HRESULT hrDev = E_FAIL;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
            hrDev = createDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hrDev)) {
            LOGW("d3d12: WARP unavailable, using the default adapter for vtable discovery");
            hrDev = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        }
        if (FAILED(hrDev)) {
            error = "D3D12CreateDevice failed";
            break;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
            error = "CreateCommandQueue failed";
            break;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1))) {
            error = "CreateSwapChainForHwnd failed";
            break;
        }
        // IDXGISwapChain: 8 Present, 13 ResizeBuffers; IDXGISwapChain1: 22 Present1;
        // IDXGISwapChain3: 38 SetColorSpace1, 39 ResizeBuffers1; ID3D12CommandQueue: 10 ExecuteCommandLists.
        void* pPresent = VtableEntry(sc1, 8);
        void* pResize = VtableEntry(sc1, 13);
        void* pPresent1 = VtableEntry(sc1, 22);
        void* pColor = VtableEntry(sc1, 38);
        void* pResize1 = VtableEntry(sc1, 39);
        void* pExecute = VtableEntry(queue, 10);
        ok = Hook(pExecute, reinterpret_cast<void*>(&HookExecute), reinterpret_cast<void**>(&g_execute), "Execute", error) &&
             Hook(pPresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_present), "Present", error) &&
             Hook(pPresent1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_present1), "Present1", error) &&
             Hook(pResize, reinterpret_cast<void*>(&HookResize), reinterpret_cast<void**>(&g_resize), "ResizeBuffers", error) &&
             Hook(pResize1, reinterpret_cast<void*>(&HookResize1), reinterpret_cast<void**>(&g_resize1), "ResizeBuffers1", error) &&
             Hook(pColor, reinterpret_cast<void*>(&HookColorSpace), reinterpret_cast<void**>(&g_colorSpace), "SetColorSpace1", error);
        if (!ok) break;
        // Device / command list hooks for drawing Mario inside the game's frame.
        // Optional: without them the mod falls back to the overlay renderer.
        std::string terr;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) {
            list->Close();
            if (!tracker::Install(device, list, terr)) LOGE("d3d12: frame hooks failed: %s", terr.c_str());
        } else {
            LOGE("d3d12: couldn't create a command list for the frame hooks");
        }
    } while (false);
    if (list) list->Release();
    if (alloc) alloc->Release();

    if (sc1) sc1->Release();
    if (queue) queue->Release();
    if (device) device->Release();
    if (warp) warp->Release();
    if (factory) factory->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (ok) LOGI("d3d12: Present/ResizeBuffers/ExecuteCommandLists hooked");
    return ok;
}

} // namespace sm2m::d3d12hook

#endif
