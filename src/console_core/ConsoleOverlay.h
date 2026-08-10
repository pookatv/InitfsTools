#pragma once

// ConsoleOverlay.h
// 
// Only meaningful when compiled into the injected DLL
// The Qt host app includes this header for the function signatures only,
// but the .cpp is excluded from its build via FB_CONSOLE_OVERLAY_DLL_BUILD

#ifdef FB_CONSOLE_OVERLAY_DLL_BUILD

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <deque>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

namespace ConsoleOverlay
{
    // Call after FrostbiteConsole::isReady() succeeds in the worker thread
    void initialize();
    // Call on DLL_PROCESS_DETACH or worker thread exit
    void shutdown();
    // Called by the DXGI proxy (dxgi.cpp) right BEFORE the game creates a
    // brand-new swap chain (not a resize) on the same HWND/output. Drops any
    // D3D12 back-buffer / RTV references we are still holding on the
    // CURRENT swap chain. DXGI returns E_ACCESSDENIED from the real
    // CreateSwapChain/CreateSwapChainForHwnd call if any outstanding
    // reference to the swap chain being replaced (ours or anyone else's)
    // still exists at the moment of creation. Safe to call at any time,
    // including before initialize() has ever run (no-op then).
    void releaseSwapChainResourcesForRecreate();

    // Registered with FrostbiteConsole::addOutputHandler (4-arg form)
    // Signature must match OutputHandlerFn exactly
    void __fastcall outputHandler(void* thisDiscarded,
        const char* tag,
        const char* buf,
        unsigned int size);

    // Returns true if the real IngameConsoleImpl is present in this build
    bool probeIngameConsolePresent();

    // Returns true if initialize() completed successfully
    bool isInitialized();

    // Set by DLLmain after registering the pipe output handler, so the
    // overlay can chain-call it and keep ConsoleWindow receiving output
    using PipeHandlerFn = void(__fastcall*)(void*, const char*, const char*, unsigned int);
    extern PipeHandlerFn g_pipeOutputHandler;

}

#endif // FB_CONSOLE_OVERLAY_DLL_BUILD