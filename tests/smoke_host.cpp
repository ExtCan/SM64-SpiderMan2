// Smoke-test host: mimics Overstrike's scripts proxy. Lays out like the game
// folder (this exe + scripts/sm2mario.dll + scripts/resources/sm2mario/...),
// loads the DLL, calls script_enable(), optionally opens a window and presents
// through D3D12 so the Present hook, renderer and overlay run, then exits.
//
//   smoke_host.exe [seconds] [--d3d12]
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <utility>

// ---- mock engine globals (referenced by tests/mock_game.S) ----
extern "C" {
struct FakeHeroSystem {
    void* vftable;
    uint8_t pad[8];
    int32_t unk;
    uint32_t heroHandle; // +0x14, like SM2ScriptTemplate's HeroSystem
};
FakeHeroSystem g_heroSystem;
uint8_t* g_actorArray = nullptr;
uint32_t g_actorCount = 0;
uint64_t g_scratch = 0;
uint32_t g_renderFrame = 0;
uint32_t g_pedEnterCount = 0; // used by mock_game.S (the e2e test's pedestrian)
void fake_hero_ctor();
void fake_transform_set_position(void*, const float*);
void fake_transform_hide(void*);
void fake_transform_unhide(void*);
uintptr_t fake_get_actor(const uint32_t*);
void fake_set_position(void*, const float*);
}

static bool g_mock = false;
static bool g_script = false;
// Transforms are 0x80 bytes like the game's (flags at +0x5C).
alignas(16) static float g_heroTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100.0f, 5.0f, -40.0f, 1};
alignas(16) static float g_enemyTransform[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100.0f, 5.0f, -39.0f, 1};
static float* g_camera = nullptr;     // heap, like a real camera object
static float* g_projection = nullptr;
static float g_orbit = 0.0f;

// Component layout per SM2ScriptTemplate: ComponentInfo name at +0x60 (the
// types it derives from at +0x80, count +0xE6; +0xE4 bit 6: lookups match
// derived types), Health max at +0xA0 / current at +0xD0, actor list
// {info*, comp*} at +0x68.
struct MockComponentInfo {
    uint8_t pad[0x60];
    const char* name;
    uint8_t pad2[0x18];
    const void* parents[12];
    uint8_t pad3[4];
    uint8_t flags;
    uint8_t pad4;
    uint8_t parentCount;
    uint8_t pad5[9];
};
struct MockEntry {
    void* info;
    void* comp;
};
alignas(16) static MockComponentInfo g_healthInfo{{}, "Health", {}, {}, {}, 0x40, 0, 0, {}};
alignas(16) static MockComponentInfo g_otherInfo{{}, "Locomotion", {}, {}, {}, 0, 0, 0, {}};
alignas(16) static uint8_t g_otherComp[0x100];
alignas(16) static uint8_t g_heroHealth[0x100];
alignas(16) static uint8_t g_enemyHealth[0x100];
static MockEntry g_heroComps[2] = {{&g_otherInfo, g_otherComp}, {&g_healthInfo, g_heroHealth}};
static MockEntry g_enemyComps[1] = {{&g_healthInfo, g_enemyHealth}};

static float& HealthMax(uint8_t* c) { return *reinterpret_cast<float*>(c + 0xA0); }
static float& HealthCur(uint8_t* c) { return *reinterpret_cast<float*>(c + 0xD0); }

static void SetupActor(uint8_t* slot, float* transform, uint16_t serial, const char* name, MockEntry* comps,
                       uint16_t count) {
    *reinterpret_cast<float**>(slot) = transform;
    *reinterpret_cast<uint16_t*>(slot + 0x08) = serial; // the handle's bits 20-30
    *reinterpret_cast<uint32_t*>(slot + 0x0C) = static_cast<uint32_t>((slot - g_actorArray) / 0xC0); // its own index
    *reinterpret_cast<MockEntry**>(slot + 0x68) = comps;
    *reinterpret_cast<uint16_t*>(slot + 0x70) = count;
    slot[0x88] = 0; // component storage mode
    *reinterpret_cast<const char**>(slot + 0xB0) = name;
}

static void SetupMock() {
    g_actorArray = static_cast<uint8_t*>(_aligned_malloc(0xC0 * 8, 16));
    std::memset(g_actorArray, 0, 0xC0 * 8);
    uint8_t* slot = g_actorArray + 3 * 0xC0;
    SetupActor(slot, g_heroTransform, 7, "hero_mock_spiderman", g_heroComps, 2);
    SetupActor(g_actorArray + 5 * 0xC0, g_enemyTransform, 9, "npc_thug_test", g_enemyComps, 1);
    HealthMax(g_heroHealth) = HealthCur(g_heroHealth) = 1000.0f;
    HealthMax(g_enemyHealth) = HealthCur(g_enemyHealth) = 300.0f;
    g_actorCount = 8;
    g_heroSystem.heroHandle = (7u << 20) | 3u;
    // Keep the asm referenced.
    volatile uintptr_t keep = reinterpret_cast<uintptr_t>(&fake_hero_ctor) ^ reinterpret_cast<uintptr_t>(&fake_get_actor) ^
                              reinterpret_cast<uintptr_t>(&fake_set_position) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_set_position) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_hide) ^
                              reinterpret_cast<uintptr_t>(&fake_transform_unhide);
    (void)keep;
    uint32_t h = g_heroSystem.heroHandle;
    std::printf("host: mock hero actor %p (GetActor -> %p)\n", static_cast<void*>(slot),
                reinterpret_cast<void*>(fake_get_actor(&h)));

    g_camera = static_cast<float*>(_aligned_malloc(64 * 4, 16));
    std::memset(g_camera, 0, 64 * 4);
    g_projection = static_cast<float*>(_aligned_malloc(64, 16));
    std::memset(g_projection, 0, 64);
    const float aspect = 1280.0f / 720.0f;
    const float ys = 1.0f / std::tan(0.5f * 55.0f * 3.14159265f / 180.0f);
    g_projection[0] = ys / aspect;
    g_projection[5] = ys;
    g_projection[10] = 0.0f; // reverse-Z, infinite far
    g_projection[11] = 1.0f;
    g_projection[14] = 0.1f;
}

static void Key(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

// Scripted play session (seconds since the first frame).
static void RunScript(double t, HWND hwnd) {
    struct Step {
        double at;
        int what; // 0 key down, 1 key up, 2 damage hero, 3 screenshot marker, 4 report
        WORD vk;
        bool done;
    };
    static Step steps[] = {
        {6.0, 0, 'M', false},  {6.15, 1, 'M', false},  // become Mario
        {8.0, 0, 'E', false},  {8.12, 1, 'E', false},  // punch x3
        {8.7, 0, 'E', false},  {8.82, 1, 'E', false},
        {9.4, 0, 'E', false},  {9.52, 1, 'E', false},
        {10.0, 2, 0, false},                           // an enemy hits Spider-Man
        {10.5, 0, 'W', false}, {11.5, 1, 'W', false},  // walk
        {12.0, 3, 0, false},                           // screenshot
        {13.0, 0, 'M', false}, {13.15, 1, 'M', false}, // back to Spider-Man
        {14.0, 4, 0, false},
    };
    for (auto& s : steps) {
        if (s.done || t < s.at) continue;
        s.done = true;
        SetForegroundWindow(hwnd);
        switch (s.what) {
        case 0: Key(s.vk, true); break;
        case 1: Key(s.vk, false); break;
        case 2:
            HealthCur(g_heroHealth) -= 100.0f;
            std::printf("host: t=%.1f the game hit Spider-Man (health %.0f)\n", t, HealthCur(g_heroHealth));
            break;
        case 3: {
            FILE* f = std::fopen("shoot_now", "w");
            if (f) std::fclose(f);
            break;
        }
        case 4: {
            float len[3];
            for (int r = 0; r < 3; ++r)
                len[r] = std::sqrt(g_heroTransform[r * 4] * g_heroTransform[r * 4] +
                                   g_heroTransform[r * 4 + 1] * g_heroTransform[r * 4 + 1] +
                                   g_heroTransform[r * 4 + 2] * g_heroTransform[r * 4 + 2]);
            std::printf("host: REPORT hero pos (%.2f %.2f %.2f) row lengths %.3f %.3f %.3f | hero health %.0f/%.0f | "
                        "enemy health %.0f/%.0f\n",
                        g_heroTransform[12], g_heroTransform[13], g_heroTransform[14], len[0], len[1], len[2],
                        HealthCur(g_heroHealth), HealthMax(g_heroHealth), HealthCur(g_enemyHealth),
                        HealthMax(g_enemyHealth));
            break;
        }
        }
    }
}

static void UpdateMockCamera() {
    // Third-person camera orbiting the hero, looking at his chest (LH basis rows).
    g_orbit += 0.004f;
    const float hx = g_heroTransform[12], hy = g_heroTransform[13], hz = g_heroTransform[14];
    const float cx = hx + 4.0f * std::sin(g_orbit), cy = hy + 2.2f, cz = hz - 4.0f * std::cos(g_orbit);
    float fx = hx - cx, fy = hy + 1.0f - cy, fz = hz - cz;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    // right = up x fwd, up' = fwd x right
    float rx = fz, ry = 0.0f, rz = -fx;
    float rl = std::sqrt(rx * rx + rz * rz);
    rx /= rl; rz /= rl;
    float ux = fy * rz - fz * ry, uy = fz * rx - fx * rz, uz = fx * ry - fy * rx;
    float* m = g_camera + 16; // somewhere inside the object
    m[0] = rx; m[1] = ry; m[2] = rz; m[3] = 0;
    m[4] = ux; m[5] = uy; m[6] = uz; m[7] = 0;
    m[8] = fx; m[9] = fy; m[10] = fz; m[11] = 0;
    m[12] = cx; m[13] = cy; m[14] = cz; m[15] = 1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static int RunD3D12(int seconds) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"smoke_game";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Smoke Game", WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("host: no window\n");
        return 2;
    }
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    ShowCursor(FALSE); // like a game during gameplay
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("host: no dxgi factory\n");
        return 2;
    }
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        IDXGIAdapter* warp = nullptr;
        factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    }
    if (FAILED(hr)) {
        std::printf("host: D3D12CreateDevice failed 0x%08lx\n", hr);
        return 2;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 1280;
    sd.Height = 720;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1))) {
        std::printf("host: swap chain failed\n");
        return 2;
    }
    IDXGISwapChain3* sc = nullptr;
    sc1->QueryInterface(IID_PPV_ARGS(&sc));
    // Like a game: submit an (empty) command list on the queue each frame so
    // the ExecuteCommandLists hook sees it, then Present.
    ID3D12CommandAllocator* alloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    // RTVs so the host clears its back buffers every frame, like a game.
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 3;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource* buffers[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtvStride;
        device->CreateRenderTargetView(buffers[i], nullptr, h);
    }
    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    const DWORD start = GetTickCount();
    DWORD end = start + DWORD(seconds) * 1000;
    int frames = 0;
    while (GetTickCount() < end) {
        if (g_script) RunScript((GetTickCount() - start) / 1000.0, hwnd);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_mock) UpdateMockCamera();
        alloc->Reset();
        list->Reset(alloc, nullptr);
        {
            const UINT bi = sc->GetCurrentBackBufferIndex();
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = buffers[bi];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &b);
            D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(bi) * rtvStride;
            const float sky[4] = {0.32f, 0.45f, 0.62f, 1.0f};
            list->ClearRenderTargetView(h, sky, 0, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list->ResourceBarrier(1, &b);
        }
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        sc->Present(1, 0);
        queue->Signal(fence, ++fv);
        fence->SetEventOnCompletion(fv, ev);
        WaitForSingleObject(ev, 1000);
        ++frames;
    }
    std::printf("host: presented %d frames\n", frames);
    return 0;
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? std::atoi(argv[1]) : 8;
    bool d3d = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--d3d12") == 0) d3d = true;
        if (std::strcmp(argv[i], "--mock") == 0) g_mock = true;
        if (std::strcmp(argv[i], "--script") == 0) g_script = true;
    }
    if (g_mock) {
        SetupMock();
        UpdateMockCamera();
    }
    SetDllDirectoryA("scripts");
    HMODULE mod = LoadLibraryA("sm2mario.dll");
    if (!mod) {
        std::printf("host: LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }
    using EnableFn = void (*)();
    auto enable = reinterpret_cast<EnableFn>(reinterpret_cast<void*>(GetProcAddress(mod, "script_enable")));
    if (!enable) {
        std::printf("host: script_enable not exported\n");
        return 1;
    }
    enable();
    std::printf("host: script_enable returned\n");
    int rc = 0;
    if (d3d) rc = RunD3D12(seconds);
    else Sleep(DWORD(seconds) * 1000);
    std::printf("host: done\n");
    std::fflush(stdout);
    ExitProcess(UINT(rc));
}
