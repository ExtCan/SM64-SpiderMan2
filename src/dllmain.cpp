// Entry points. Overstrike's scripts proxy (winmm.dll) LoadLibrary()s this DLL
// from <game>/scripts and calls the exported script_enable(). If the DLL is
// injected some other way (an ASI loader), DllMain starts it after a delay.
#ifdef _WIN32

#include "common/platform.h"
#include "mod/mod.h"

static HMODULE g_self = nullptr;

static DWORD WINAPI DelayedStart(void*) {
    Sleep(5000);
    sm2m::MarioMod::Get().Start(g_self, false);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        HANDLE t = CreateThread(nullptr, 0, DelayedStart, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void script_enable() { sm2m::MarioMod::Get().Start(g_self, true); }

#endif
