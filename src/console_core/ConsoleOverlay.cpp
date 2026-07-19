// ConsoleOverlay.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Only compiled into the injected DLL (FB_CONSOLE_OVERLAY_DLL_BUILD).
// Excluded from the Qt host build entirely.
//
// Rendering pipeline:
//   VTable hook on IDXGISwapChain::Present (no MinHook needed)
//   GDI bitmap font atlas baked at init time, uploaded as D3D11 R8 texture
//   One dynamic vertex buffer, one VS+PS pair (flat color + textured glyphs)
//   WndProc subclass for keyboard capture
//
// Detection:
//   probeIngameConsolePresent() scans the game module for the "> \0...\0"
//   string signature that only exists when IngameConsoleImpl is compiled in
//   (FB_RETAIL_DEBUG_RENDERER). If found, we skip injecting our overlay.
//
// Hardcoded addresses (Garden Warfare, confirm first):
//   g_dxRendererInstance = 0x141FEB230
//   HWND static          = 0x141C28E08
// ─────────────────────────────────────────────────────────────────────────────

#define FB_CONSOLE_OVERLAY_DLL_BUILD
#include "ConsoleOverlay.h"
#include "FrostbiteConsole.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>

// Declared in DLLmain.cpp — signals that the overlay is mid-execute so the
// shared output handler does not gate its output behind g_expectingOutput.
extern volatile bool g_overlayExecuting;

// ─────────────────────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────────────────────
static void overlayLog(const char* msg)
{
    // OutputDebugString removed: when loaded by the tool process under VS the
    // debugger output window would receive all overlay log traffic, which is
    // completely unrelated to what the DLL is doing inside the actual game process.
    // All overlay logging goes to the disk log only.

    char exePath[MAX_PATH] = {};
    HMODULE mods[1] = {};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) && mods[0])
    {
        if (GetModuleFileNameExA(GetCurrentProcess(), mods[0], exePath, MAX_PATH))
        {
            char* slash = strrchr(exePath, '\\');
            if (slash)
            {
                slash[0] = '\0';
                char logPath[MAX_PATH];
                lstrcpyA(logPath, exePath);
                lstrcatA(logPath, "\\FBConsoleBridge_log.txt");
                HANDLE hFile = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    DWORD w = 0;
                    const char prefix[] = "[Overlay] ";
                    WriteFile(hFile, prefix, (DWORD)strlen(prefix), &w, nullptr);
                    WriteFile(hFile, msg, (DWORD)strlen(msg), &w, nullptr);
                    WriteFile(hFile, "\r\n", 2, &w, nullptr);
                    CloseHandle(hFile);
                }
            }
        }
    }
}

static void overlayLogFmt(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    overlayLog(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic address resolution
//
// k_dxRendererInstance:
//   Scan the main module's executable pages for the Frostbite DxRenderer
//   singleton null-check pattern:
//     48 83 3D ?? ?? ?? ?? 00   cmp qword ptr [RIP+rel], 0
//     0F 85 ?? ?? ?? ??         jne ...
//   The displacement in the cmp resolves to the singleton storage address.
//   This pattern is the renderer init guard and is consistent across Frostbite
//   titles (GW1, GW2, BF4, NFS Heat, MEA, etc.).
//
// k_hwndStatic:
//   Scan the main module's executable pages for the game window HWND store,
//   which always follows CreateWindowExA in the window creation function:
//     FF 15 ?? ?? ?? ??         call [CreateWindowExA]   (IAT indirect)
//     48 8B F0                  mov rsi, rax
//     48 85 C0                  test rax, rax
//     0F 84 ?? ?? ?? ??         je ...  (bail on null)
//   Then scan forward from that point for:
//     48 89 35 ?? ?? ?? ??      mov [RIP+rel], rsi
//   That store's target is the HWND static address.
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t** g_dxRendererInstanceAddr = nullptr;
static HWND* g_hwndStaticAddr = nullptr;

// Resolved once at initialize() time from DXGI directly — no struct offsets needed
static IDXGISwapChain* g_resolvedSwapChain = nullptr;
static ID3D11Device* g_resolvedDevice = nullptr;
static ID3D11DeviceContext* g_resolvedContext = nullptr;

// D3D12 mode — populated when the game uses D3D12 exclusively
static bool                    g_isD3D12Mode = false;
static ID3D12Device* g_d3d12Device = nullptr;
static ID3D12CommandQueue* g_d3d12Queue = nullptr;  // game's queue — execute on this
static ID3D11On12Device* g_11on12Device = nullptr;
static ID3D11Resource* g_11on12WrappedRT = nullptr;

// Pure D3D12 overlay render resources (no D3D11On12)
static ID3D12CommandAllocator* g_d3d12CmdAlloc = nullptr;
static ID3D12GraphicsCommandList* g_d3d12CmdList = nullptr;
static ID3D12RootSignature* g_d3d12RootSig = nullptr;
static ID3D12PipelineState* g_d3d12PSO = nullptr;
static ID3D12Resource* g_d3d12VB = nullptr;       // upload heap vertex buffer
static ID3D12DescriptorHeap* g_d3d12RTVHeap = nullptr;
static ID3D12DescriptorHeap* g_d3d12SRVHeap = nullptr;
static ID3D12Resource* g_d3d12FontTex = nullptr;
static ID3D12Resource* g_d3d12FontUpload = nullptr;
static UINT                       g_d3d12RTVDescSize = 0;
// One RTV per swap-chain buffer (flip model has up to 4)
static const UINT k_maxSwapBuffers = 4;
static ID3D12Resource* g_d3d12BackBuffers[k_maxSwapBuffers] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_d3d12RTVHandles[k_maxSwapBuffers] = {};
static UINT                       g_d3d12BufferCount = 0;
static bool                       g_d3d12ResourcesReady = false;

static uint8_t** g_dxRendererBaseSlot = nullptr;
static uint32_t   g_dxRendererFieldOffset = 0;

// Scan one memory page for a RIP-relative LEA targeting needle.
// Separated from resolveAddresses so __try is legal (no object unwinding).
static bool scanPageForLEA(uint8_t* pageStart, uint8_t* pageEnd,
    uint8_t* needle, uint8_t** outInsn)
{
    __try
    {
        for (uint8_t* p = pageStart; p + 7 <= pageEnd; ++p)
        {
            if (p[0] < 0x48 || p[0] > 0x4F) continue;
            if (p[1] != 0x8D) continue;
            if ((p[2] & 0xC7) != 0x05) continue;
            int32_t rel = 0;
            memcpy(&rel, p + 3, 4);
            if (p + 7 + rel == needle)
            {
                *outInsn = p;
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void resolveAddresses()
{
    HANDLE hProc = GetCurrentProcess();
    MODULEINFO mi = {};
    {
        HMODULE mods[1] = {};
        DWORD needed = 0;
        if (!EnumProcessModules(hProc, mods, sizeof(mods), &needed) || !mods[0])
        {
            overlayLog("resolveAddresses: EnumProcessModules failed");
            return;
        }
        if (!GetModuleInformation(hProc, mods[0], &mi, sizeof(mi)))
        {
            overlayLog("resolveAddresses: GetModuleInformation failed");
            return;
        }
    }

    uint8_t* modBase = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    size_t   modSize = mi.SizeOfImage;
    uint8_t* modEnd = modBase + modSize;

    // ── Helper: scan [base, base+size) for a literal string ──────────────────
    auto findStr = [](uint8_t* base, size_t size, const char* needle) -> uint8_t*
        {
            size_t nlen = strlen(needle);
            if (!nlen || nlen > size) return nullptr;
            for (size_t i = 0; i + nlen <= size; ++i)
                if (memcmp(base + i, needle, nlen) == 0)
                    return base + i;
            return nullptr;
        };

    // ── Helper: find LEA Rxx,[RIP+rel] whose target == needle anywhere in
    //   [searchBase, searchBase+searchSize) executable pages ──────────────────
    // Scan a single 4KB page with SEH; returns true if the 7-byte LEA pattern
    // targeting needle was found and *outInsn is set. Used by findLEAtoTarget.
    auto scanPageForLEA = [](uint8_t* pageStart, uint8_t* pageEnd,
        uint8_t* needle, uint8_t** outInsn) -> bool
        {
            __try
            {
                for (uint8_t* p = pageStart; p + 7 <= pageEnd; ++p)
                {
                    uint8_t b0 = p[0];
                    if (b0 < 0x48 || b0 > 0x4F) continue;
                    if (p[1] != 0x8D) continue;
                    if ((p[2] & 0xC7) != 0x05) continue;
                    int32_t rel = 0;
                    memcpy(&rel, p + 3, 4);
                    if (p + 7 + rel == needle)
                    {
                        *outInsn = p;
                        return true;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return false;
        };

    auto findLEAtoTarget = [&](uint8_t* searchBase, size_t searchSize,
        uint8_t* targetAddr) -> uint8_t*
        {
            if (searchSize < 7) return nullptr;
            uint8_t* end = searchBase + searchSize;

            // Walk VQ regions rather than 4KB pages — one VirtualQuery call
            // per committed region (typically 64KB–2MB) instead of one per page.
            uint8_t* cursor = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(searchBase) & ~(uintptr_t)0xFFF);
            while (cursor < end)
            {
                MEMORY_BASIC_INFORMATION mbi = {};
                if (!VirtualQuery(cursor, &mbi, sizeof(mbi))) break;

                uint8_t* rBase = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                uint8_t* rEnd = rBase + mbi.RegionSize;

                // Clamp to our search window
                uint8_t* pStart = (rBase < searchBase) ? searchBase : rBase;
                uint8_t* pEnd = (rEnd > end) ? end : rEnd;

                // Only scan committed readable regions — skip guard/noaccess/free.
                // EA-AC marks code pages PAGE_EXECUTE_READ or PAGE_READONLY; both
                // are readable so the LEA scan works on either.
                if (mbi.State == MEM_COMMIT &&
                    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    pEnd > pStart)
                {
                    // Still scan in 4KB sub-pages under __try so a faulting
                    // GPU-mapped or DXGI-internal page doesn't abort the whole region.
                    for (uint8_t* page = reinterpret_cast<uint8_t*>(
                        reinterpret_cast<uintptr_t>(pStart) & ~(uintptr_t)0xFFF);
                        page < pEnd; page += 0x1000)
                    {
                        uint8_t* sStart = (page < pStart) ? pStart : page;
                        uint8_t* sEnd = (page + 0x1000 < pEnd) ? page + 0x1000 : pEnd;
                        uint8_t* found = nullptr;
                        if (scanPageForLEA(sStart, sEnd, targetAddr, &found))
                            return found;
                    }
                }

                uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= reinterpret_cast<uintptr_t>(cursor)) break;
                cursor = reinterpret_cast<uint8_t*>(next);
            }
            return nullptr;
        };

    // ── Helper: walk backward from insideFunc to find the function prologue ──
    auto walkBack = [&](uint8_t* insideFunc) -> uint8_t*
        {
            for (int back = 1; back < 128 * 1024; ++back)
            {
                uint8_t* cand = insideFunc - back;
                if (cand < modBase) break;
                bool preceded = (cand > modBase &&
                    (cand[-1] == 0xCC || cand[-1] == 0x90));
                if (!preceded) continue;
                uint8_t b0 = cand[0], b1 = cand[1], b2 = cand[2];
                bool ok = false;
                if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x89 &&
                    (b2 == 0x4C || b2 == 0x54 || b2 == 0x5C ||
                        b2 == 0x44 || b2 == 0x74 || b2 == 0x7C)) ok = true;
                if (b0 == 0x55 || b0 == 0x53 || b0 == 0x56 || b0 == 0x57) ok = true;
                if ((b0 & 0xF0) == 0x40 &&
                    (b1 == 0x55 || b1 == 0x53 || b1 == 0x56 || b1 == 0x57)) ok = true;
                if (b0 == 0x41 && b1 >= 0x54 && b1 <= 0x57) ok = true;
                if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC) ok = true;
                if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC) ok = true;
                if (b0 == 0x48 && b1 == 0x8B && b2 == 0xC4) ok = true;
                if (!ok) continue;
                // EA-AC strips execute bits from code pages; accept any committed
                                // readable page — the SEH-based scan already handled fault safety.
                MEMORY_BASIC_INFORMATION mbi = {};
                if (!VirtualQuery(cand, &mbi, sizeof(mbi))) continue;
                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
                return cand;
            }
            return nullptr;
        };

    // ── Scan 1: DxRenderer singleton ─────────────────────────────────────────
        //
        // Anchor: "RenderDevice" string in .rdata
        // Method: collect ALL LEA Rxx,[RIP+rel] instructions targeting the string,
        //         walk each back to its function prologue, scan forward for a
        //         singleton store whose slot contains a non-null heap pointer.
        //         Multiple constructors may register under the same "RenderDevice"
        //         key (e.g. NFS Payback has two: one that populates 0x14382C260
        //         and one that populates 0x14382C5B8 — only the latter is live).
        //         We try every LEA until we find a non-null slot.
    {
        uint8_t* strAddr = findStr(modBase, modSize, "RenderDevice");
        uint8_t* leaInsn = nullptr;
        if (!strAddr)
        {
            overlayLog("resolveAddresses: 'RenderDevice' string not found");
        }
        else
        {
            overlayLogFmt("resolveAddresses: 'RenderDevice' string at %p", (void*)strAddr);

            // Collect all LEAs targeting the string — different constructors
            // each have their own LEA. We try them all in order.
            std::vector<uint8_t*> allLeaInsns;
            {
                uint8_t* search = modBase;
                while (search < modEnd)
                {
                    uint8_t* found = findLEAtoTarget(search, modEnd - search, strAddr);
                    if (!found) break;
                    allLeaInsns.push_back(found);
                    search = found + 7;
                }
            }
            overlayLogFmt("resolveAddresses: found %zu LEA(s) to 'RenderDevice'",
                allLeaInsns.size());

            // Use the first one as leaInsn for fallback paths below
            leaInsn = allLeaInsns.empty() ? nullptr : allLeaInsns[0];

            for (uint8_t* thisLea : allLeaInsns)
            {
                if (g_dxRendererInstanceAddr) break;

                overlayLogFmt("resolveAddresses: LEA to 'RenderDevice' at %p", (void*)thisLea);

                uint8_t* fnStart = walkBack(thisLea);
                if (!fnStart)
                {
                    overlayLog("resolveAddresses: could not walk back to prologue");
                    continue;
                }

                overlayLogFmt("resolveAddresses: DxRenderer init fn at %p", (void*)fnStart);

                // Scan forward up to 4KB for a singleton store pattern.
                // Accept: 4C 89 3D (mov [RIP+rel32], r15)
                //         48 89 35 (mov [RIP+rel32], rsi)
                //         48 89 3D (mov [RIP+rel32], rdi)
                // Do not accept 48 89 05 (mov [RIP+rel32], rax) which writes
                // temporary intermediate values earlier in the same function.
                uint8_t* scan = fnStart;
                uint8_t* scanEnd = fnStart + 4096;
                if (scanEnd > modEnd) scanEnd = modEnd;

                for (; scan + 7 <= scanEnd; ++scan)
                {
                    bool isSingletonStore =
                        (scan[0] == 0x4C && scan[1] == 0x89 && scan[2] == 0x3D) ||
                        (scan[0] == 0x48 && scan[1] == 0x89 && scan[2] == 0x35) ||
                        (scan[0] == 0x48 && scan[1] == 0x89 && scan[2] == 0x3D);
                    if (!isSingletonStore) continue;

                    int32_t rel = 0;
                    memcpy(&rel, scan + 3, 4);
                    uint8_t* target = scan + 7 + rel;

                    if (target < modBase || target >= modEnd) continue;
                    MEMORY_BASIC_INFORMATION mbi = {};
                    if (!VirtualQuery(target, &mbi, sizeof(mbi))) continue;
                    if (mbi.State != MEM_COMMIT) continue;
                    // Do NOT reject executable slots — EA-AC/GW2 maps its entire
                    // image (including .data globals) as PAGE_EXECUTE_READ.
                    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;

                    // If the slot is null, skip — a later constructor (e.g. NFS
                    // Payback's 0x141AB3500) may write to a different slot that
                    // IS populated. Continue to the next store pattern.
                    uint8_t** candidate = reinterpret_cast<uint8_t**>(target);
                    if (!*candidate)
                    {
                        overlayLogFmt("resolveAddresses: DxRenderer candidate slot=%p is null — continuing",
                            (void*)target);
                        continue;
                    }

                    // Sanity: the DxRenderer object lives on the heap —
                    // its pointer must land in committed non-executable memory.
                    uint8_t* dr = *candidate;
                    MEMORY_BASIC_INFORMATION drMbi = {};
                    if (!VirtualQuery(dr, &drMbi, sizeof(drMbi))) continue;
                    if (drMbi.State != MEM_COMMIT) continue;
                    if (drMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
                    if (drMbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;

                    g_dxRendererInstanceAddr = candidate;
                    overlayLogFmt("resolveAddresses: DxRenderer instance addr=%p (found at %p)",
                        (void*)target, (void*)scan);
                    break;
                }
            } // end for allLeaInsns
        }

        // Fallback A: some Frostbite titles store the DxRenderer as a field inside a
                // larger manager object. The manager is stored via 48 89 3D (mov [RIP+rel32], rdi)
                // near the "RenderDevice" LEA. Scan forward from the LEA for that pattern, read
                // the manager pointer, then scan manager+[0..0x800] for a COM pointer whose
                // vtable lands in an exec region (the DxRenderer).
        if (!g_dxRendererInstanceAddr && leaInsn)
        {
            uint8_t* scan = leaInsn;
            uint8_t* scanEnd = leaInsn + 4096;
            if (scanEnd > modEnd) scanEnd = modEnd;

            for (; scan + 7 <= scanEnd; ++scan)
            {
                // 48 89 3D ?? ?? ?? ??  mov [RIP+rel32], rdi
                if (scan[0] != 0x48 || scan[1] != 0x89 || scan[2] != 0x3D) continue;

                int32_t rel = 0;
                memcpy(&rel, scan + 3, 4);
                uint8_t* managerSlot = scan + 7 + rel;

                if (managerSlot < modBase || managerSlot >= modEnd) continue;
                MEMORY_BASIC_INFORMATION mbi = {};
                if (!VirtualQuery(managerSlot, &mbi, sizeof(mbi))) continue;
                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
                if (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;

                uint64_t managerPtr = 0;
                if (!FrostbiteConsole::safeRead64(managerSlot, &managerPtr) || !managerPtr) continue;

                uint8_t* manager = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(managerPtr));

                for (size_t off = 0; off + 8 <= 0x800; off += 8)
                {
                    uint64_t candidate = 0;
                    if (!FrostbiteConsole::safeRead64(manager + off, &candidate) || !candidate) continue;

                    uint64_t vtbl = 0;
                    if (!FrostbiteConsole::safeRead64(
                        reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)), &vtbl)) continue;

                    MEMORY_BASIC_INFORMATION vtblMbi = {};
                    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(vtbl)),
                        &vtblMbi, sizeof(vtblMbi))) continue;
                    if (vtblMbi.State != MEM_COMMIT) continue;
                    if (!(vtblMbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;

                    uint8_t* dr = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(candidate));
                    MEMORY_BASIC_INFORMATION drMbi = {};
                    if (!VirtualQuery(dr, &drMbi, sizeof(drMbi))) continue;
                    if (drMbi.State != MEM_COMMIT) continue;
                    if (drMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
                    if (drMbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;

                    g_dxRendererInstanceAddr = reinterpret_cast<uint8_t**>(manager + off);
                    overlayLogFmt("resolveAddresses: DxRenderer (field/rdi) addr=%p "
                        "(manager=%p off=0x%zX found at %p)",
                        (void*)(manager + off), (void*)manager, off, (void*)scan);
                    break;
                }

                if (g_dxRendererInstanceAddr) break;
            }
        }

        // Fallback B: The DxRenderer is returned from the init function and stored
        // as a field inside a larger engine object (GW2/EA-AC pattern).
        // Strategy:
        //   1. Find the LEA of "RenderDevice" string inside any function (the caller).
        //   2. In that caller, find a 4C 89 3D store (mov [RIP+rel32], r15) that
        //      writes the engine object pointer to a global — that global is the base.
        //   3. Also in the caller, after the call that returns the DxRenderer, find a
        //      49 89 [87|8F|97|9F|A7|AF|B7|BF] disp32 (mov [r15+disp32], rXX) store
        //      — that disp32 is the field offset of the DxRenderer inside the object.
        //   4. g_dxRendererInstanceAddr = &(*(base_global) + field_offset)
        //      i.e. a pointer-to-pointer into the engine object's field slot.
        if (!g_dxRendererInstanceAddr && leaInsn)
        {
            // The leaInsn we found is inside the init function. We need the CALLER
            // that contains the "RenderDevice" LEA used to register the sub-object.
            // Scan the entire module for any LEA to the "RenderDevice" string,
            // then find the one that is NOT inside the init function (it's in the caller).
            uint8_t* initFn = walkBack(leaInsn);
            uint8_t* initFnEnd = initFn ? (initFn + 4096) : nullptr;
            if (initFnEnd && initFnEnd > modEnd) initFnEnd = modEnd;

            // Collect ALL LEAs to the RenderDevice string (there may be 3+).
                        // The one we already found (inside initFn) is skipped.
                        // We try each remaining one as a "caller" — the right one is the
                        // function that contains the definitive 4C 89 3D singleton stores
                        // (i.e. mov [RIP+rel], r15 with a non-module heap value in the slot).
                        // GW2/EA-AC has the real init at a completely different address from
                        // the registration function, so we must try all candidates.
// Diagnostic: probe why findLEAtoTarget misses the LEA at modBase+0x88DA79.
            {
                // VQ the region containing that page
                uint8_t* testPage = modBase + 0x88D000;
                MEMORY_BASIC_INFORMATION tMbi = {};
                bool tVQ = VirtualQuery(testPage, &tMbi, sizeof(tMbi)) != 0;
                overlayLogFmt("resolveAddresses: FallbackB diag — page %p VQ ok=%d "
                    "state=%08X prot=%08X regionBase=%p regionSize=%zX",
                    (void*)testPage, (int)tVQ,
                    tVQ ? tMbi.State : 0,
                    tVQ ? tMbi.Protect : 0,
                    tVQ ? tMbi.BaseAddress : nullptr,
                    tVQ ? tMbi.RegionSize : (size_t)0);

                // Read the 8 bytes at the expected LEA using safeRead64
                // (two overlapping reads to get all 7 bytes without SEH)
                uint8_t* insn = modBase + 0x88DA79;
                overlayLogFmt("resolveAddresses: FallbackB diag — insn addr=%p strAddr=%p",
                    (void*)insn, (void*)strAddr);

                uint64_t w0 = 0, w1 = 0;
                bool r0 = FrostbiteConsole::safeRead64(insn, &w0);
                bool r1 = FrostbiteConsole::safeRead64(insn + 4, &w1);
                overlayLogFmt("resolveAddresses: FallbackB diag — safeRead64[0] ok=%d val=%016llX",
                    (int)r0, (unsigned long long)w0);
                overlayLogFmt("resolveAddresses: FallbackB diag — safeRead64[4] ok=%d val=%016llX",
                    (int)r1, (unsigned long long)w1);

                if (r0)
                {
                    // Extract bytes from the little-endian 64-bit read
                    uint8_t b0 = (uint8_t)(w0 & 0xFF);
                    uint8_t b1 = (uint8_t)((w0 >> 8) & 0xFF);
                    uint8_t b2 = (uint8_t)((w0 >> 16) & 0xFF);
                    uint8_t b3 = (uint8_t)((w0 >> 24) & 0xFF);
                    uint8_t b4 = (uint8_t)((w0 >> 32) & 0xFF);
                    uint8_t b5 = (uint8_t)((w0 >> 40) & 0xFF);
                    uint8_t b6 = (uint8_t)((w0 >> 48) & 0xFF);
                    int32_t rel = (int32_t)(b3 | (b4 << 8) | (b5 << 16) | (b6 << 24));
                    uint8_t* computed = insn + 7 + rel;
                    overlayLogFmt("resolveAddresses: FallbackB diag — "
                        "bytes=%02X %02X %02X %02X %02X %02X %02X "
                        "b0ok=%d b1ok=%d (b2&C7)=%02X rel=%08X computed=%p match=%d",
                        b0, b1, b2, b3, b4, b5, b6,
                        (int)(b0 >= 0x48 && b0 <= 0x4F),
                        (int)(b1 == 0x8D),
                        b2 & 0xC7,
                        (unsigned)rel,
                        (void*)computed,
                        (int)(computed == strAddr));
                }
            }

            std::vector<uint8_t*> callerLeaCandidates;
            {
                uint8_t* search = modBase;
                while (search < modEnd)
                {
                    uint8_t* found = findLEAtoTarget(search, modEnd - search, strAddr);
                    if (!found) break;
                    if (initFn && found >= initFn && found < initFnEnd)
                    {
                        search = found + 7;
                        continue;
                    }
                    callerLeaCandidates.push_back(found);
                    search = found + 7;
                }
            }
            // For GW2/EA-AC the init function itself contains both the engine-object
            // 4C 89 3D store and the 49 89 BF DxRenderer field store, so it must be
            // tried as a candidate. Sort any LEA inside initFn to the front so it is
            // tried first — for other titles no LEA falls inside initFn, no-op.
            if (initFn && initFnEnd)
            {
                std::stable_partition(callerLeaCandidates.begin(), callerLeaCandidates.end(),
                    [&](uint8_t* p) { return p >= initFn && p < initFnEnd; });
            }

            if (callerLeaCandidates.empty())
            {
                overlayLog("resolveAddresses: DxRenderer fallback B — "
                    "no caller LEA to 'RenderDevice' found");
            }
            else
            {
                bool foundEngineField = false;
                uint8_t** tentativeDirectSlot = nullptr;
                for (uint8_t* callerLeaInsn : callerLeaCandidates)
                {
                    if (foundEngineField) break;

                    overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                        "trying caller LEA at %p", (void*)callerLeaInsn);

                    uint8_t* callerFn = walkBack(callerLeaInsn);
                    if (!callerFn)
                    {
                        overlayLog("resolveAddresses: DxRenderer fallback B — "
                            "could not walk back to caller prologue, skipping");
                        continue;
                    }

                    overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                        "caller fn at %p", (void*)callerFn);

                    uint8_t* scanStart = (callerLeaInsn > callerFn + 4096)
                        ? callerLeaInsn - 4096 : callerFn;
                    uint8_t* scanEnd = callerLeaInsn + 262144;
                    if (scanEnd > modEnd) scanEnd = modEnd;

                    overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                        "scan range [%p, %p) callerLEA=%p callerFn=%p",
                        (void*)scanStart, (void*)scanEnd,
                        (void*)callerLeaInsn, (void*)callerFn);

                    uint8_t* baseSlot = nullptr;
                    for (int storePass = 0; storePass < 2 && !baseSlot; ++storePass)
                    {
                        for (uint8_t* s = scanStart; s + 7 <= scanEnd; ++s)
                        {
                            bool isStore = false;
                            if (storePass == 0)
                                isStore = (s[0] == 0x4C && s[1] == 0x89 && s[2] == 0x3D);
                            else
                                isStore =
                                (s[0] == 0x48 && s[1] == 0x89 && s[2] == 0x35) ||
                                (s[0] == 0x48 && s[1] == 0x89 && s[2] == 0x3D);
                            if (!isStore) continue;

                            int32_t rel = 0;
                            memcpy(&rel, s + 3, 4);
                            uint8_t* slot = s + 7 + rel;
                            if (slot < modBase || slot >= modEnd) continue;

                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "candidate insn=%p slot=%p",
                                (void*)s, (void*)slot);

                            MEMORY_BASIC_INFORMATION mbi = {};
                            bool slotVQ = VirtualQuery(slot, &mbi, sizeof(mbi)) != 0;
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "  slot VQ ok=%d state=%08X prot=%08X",
                                (int)slotVQ,
                                slotVQ ? mbi.State : 0,
                                slotVQ ? mbi.Protect : 0);
                            if (!slotVQ) continue;
                            if (mbi.State != MEM_COMMIT) { overlayLog("  rejected: not committed"); continue; }
                            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) { overlayLog("  rejected: noaccess/guard"); continue; }

                            uint64_t val2 = 0;
                            bool readOk = FrostbiteConsole::safeRead64(slot, &val2);
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "  safeRead64 ok=%d val=%p", (int)readOk, (void*)(uintptr_t)val2);
                            if (!readOk || !val2) { overlayLog("  rejected: slot value null/unreadable"); continue; }

                            if (val2 >= reinterpret_cast<uintptr_t>(modBase) &&
                                val2 < reinterpret_cast<uintptr_t>(modEnd))
                            {
                                overlayLog("  rejected: slot value is inside game module image");
                                continue;
                            }

                            uint8_t* dr = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(val2));
                            MEMORY_BASIC_INFORMATION drMbi = {};
                            bool drOk = VirtualQuery(dr, &drMbi, sizeof(drMbi)) != 0;
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "  dr=%p VQ ok=%d state=%08X prot=%08X",
                                (void*)dr, (int)drOk,
                                drOk ? drMbi.State : 0,
                                drOk ? drMbi.Protect : 0);
                            if (!drOk || drMbi.State != MEM_COMMIT) { overlayLog("  rejected: dr not committed"); continue; }
                            if (drMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) { overlayLog("  rejected: dr noaccess/guard"); continue; }

                            baseSlot = slot;
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "candidate base slot=%p (value=%p)",
                                (void*)slot, (void*)dr);
                            break;
                        }
                    }

                    uint32_t fieldOffset = 0;
                    bool foundField = false;
                    bool fieldIsPostCall = false;
                    if (baseSlot)
                    {
                        // Read the base object pointer so we can validate each candidate
                        // field by dereferencing it and checking for a DXGI/D3D11 vtable.
                        uint64_t baseVal = 0;
                        FrostbiteConsole::safeRead64(baseSlot, &baseVal);
                        uint8_t* baseObj = baseVal
                            ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(baseVal))
                            : nullptr;

                        HMODULE hD3D11chk = GetModuleHandleA("d3d11.dll");
                        HMODULE hDXGIchk = GetModuleHandleA("dxgi.dll");

                        int step2Candidates = 0;
                        uint8_t* bestFs = nullptr;
                        uint32_t bestDisp = 0;
                        bool bestIsPostCall = false;
                        bool bestValidated = false;

                        uint8_t* step2Start = callerLeaInsn;
                        for (uint8_t* fs = step2Start; fs + 7 <= scanEnd; ++fs)
                        {
                            if (fs[0] != 0x49 || fs[1] != 0x89) continue;
                            uint8_t modrm = fs[2];
                            if ((modrm & 0xC7) != 0x87) continue;

                            ++step2Candidates;

                            uint32_t disp = 0;
                            memcpy(&disp, fs + 3, 4);
                            if (disp < 0x100 || disp > 0x40000) continue;

                            bool postCall = false;
                            for (int back = 1; back <= 32 && fs - back >= scanStart; ++back)
                            {
                                uint8_t* p = fs - back;
                                if (p[0] == 0xE8 && back >= 5) { postCall = true; break; }
                                if (p[0] == 0xFF && p[1] == 0x15 && back >= 6) { postCall = true; break; }
                                if (p[0] == 0xFF && (p[1] & 0xF0) == 0xD0 && back >= 2) { postCall = true; break; }
                                if (p[0] == 0x90 || p[0] == 0xCC) break;
                            }

                            // Validate: dereference base[disp] and check if the
                            // resulting pointer's first qword is a vtable in dxgi or d3d11.
                            // This is the strongest signal that we have the right field.
                            bool validated = false;
                            if (baseObj && postCall)
                            {
                                uint64_t fieldPtr = 0;
                                if (FrostbiteConsole::safeRead64(baseObj + disp, &fieldPtr) && fieldPtr)
                                {
                                    // fieldPtr must be a committed non-executable heap pointer
                                    MEMORY_BASIC_INFORMATION fmbi = {};
                                    if (VirtualQuery(reinterpret_cast<void*>(
                                        static_cast<uintptr_t>(fieldPtr)), &fmbi, sizeof(fmbi)) &&
                                        fmbi.State == MEM_COMMIT &&
                                        !(fmbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                                        !(fmbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                                    {
                                        // Scan first 512 bytes of the sub-object for a DXGI/D3D11 vtable
                                        uint8_t* sub = reinterpret_cast<uint8_t*>(
                                            static_cast<uintptr_t>(fieldPtr));
                                        for (size_t so = 0; so + 8 <= 512 && !validated; so += 8)
                                        {
                                            uint64_t candidate = 0;
                                            if (!FrostbiteConsole::safeRead64(sub + so, &candidate)
                                                || !candidate) continue;
                                            uint64_t vtbl = 0;
                                            if (!FrostbiteConsole::safeRead64(
                                                reinterpret_cast<void*>(
                                                    static_cast<uintptr_t>(candidate)),
                                                &vtbl) || !vtbl) continue;
                                            HMODULE hMod = nullptr;
                                            if (GetModuleHandleExA(
                                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                reinterpret_cast<LPCSTR>(
                                                    static_cast<uintptr_t>(vtbl)),
                                                &hMod) && hMod &&
                                                (hMod == hD3D11chk || hMod == hDXGIchk ||
                                                    hMod == GetModuleHandleA("d3d12.dll")))
                                            {
                                                validated = true;
                                            }
                                        }
                                    }
                                }
                            }

                            // Priority: validated postCall > unvalidated postCall > non-postCall.
                            // Among equal quality, prefer smaller disp.
                            bool better = false;
                            if (!bestFs)
                                better = true;
                            else if (validated && !bestValidated)
                                better = true;
                            else if (validated == bestValidated && postCall && !bestIsPostCall)
                                better = true;
                            else if (validated == bestValidated &&
                                postCall == bestIsPostCall && disp < bestDisp)
                                better = true;

                            if (better)
                            {
                                bestDisp = disp;
                                bestFs = fs;
                                bestIsPostCall = postCall;
                                bestValidated = validated;
                            }

                            // Early exit: validated postCall with small disp is definitive
                            if (validated && postCall && disp < 0x10000)
                                break;
                        }
                        if (bestFs)
                        {
                            fieldOffset = bestDisp;
                            foundField = true;
                            fieldIsPostCall = bestIsPostCall;
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "field offset=0x%X (postCall=%d validated=%d, found at %p)",
                                bestDisp, (int)bestIsPostCall, (int)bestValidated, (void*)bestFs);
                        }
                        overlayLogFmt("resolveAddresses: FallbackB Step2 — "
                            "done: total candidates=%d foundField=%d",
                            step2Candidates, (int)foundField);
                    }

                    if (baseSlot && foundField)
                    {
                        // Reject false-positive engine-field results: if the field offset
                        // was found without a post-call anchor AND a tentative direct
                        // singleton exists whose value differs from this base slot's value,
                        // this is likely a different object — skip it and keep searching.
                        bool rejectFalsePositive = false;
                        if (!fieldIsPostCall && tentativeDirectSlot && *tentativeDirectSlot)
                        {
                            uint64_t tentativeVal = reinterpret_cast<uintptr_t>(*tentativeDirectSlot);
                            uint64_t baseVal = 0;
                            FrostbiteConsole::safeRead64(baseSlot, &baseVal);
                            if (baseVal != tentativeVal)
                            {
                                overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                    "rejecting non-postCall engine-field (baseVal=%p != tentativeVal=%p)",
                                    (void*)(uintptr_t)baseVal, (void*)(uintptr_t)tentativeVal);
                                rejectFalsePositive = true;
                            }
                        }

                        if (!rejectFalsePositive)
                        {
                            g_dxRendererBaseSlot = reinterpret_cast<uint8_t**>(baseSlot);
                            g_dxRendererFieldOffset = fieldOffset;
                            foundEngineField = true;
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "engine-field: baseSlot=%p fieldOffset=0x%X (deferred)",
                                (void*)baseSlot, fieldOffset);
                        }
                    }
                    else if (baseSlot && !foundField)
                    {
                        if (!tentativeDirectSlot)
                        {
                            tentativeDirectSlot = reinterpret_cast<uint8_t**>(baseSlot);
                            overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                                "direct singleton addr=%p (tentative, continuing)",
                                (void*)baseSlot);
                        }
                    }
                } // end for callerLeaCandidates

                // If no engine-field candidate was found, promote the tentative direct singleton.
                if (!foundEngineField && !g_dxRendererBaseSlot && tentativeDirectSlot)
                {
                    g_dxRendererInstanceAddr = tentativeDirectSlot;
                    overlayLogFmt("resolveAddresses: DxRenderer fallback B — "
                        "promoting tentative direct singleton addr=%p",
                        (void*)tentativeDirectSlot);
                }
            } // end else (callerLeaCandidates not empty)
        } // end if (!g_dxRendererInstanceAddr && leaInsn)  [FallbackB]

        if (!g_dxRendererInstanceAddr && !g_dxRendererBaseSlot)
            overlayLog("resolveAddresses: WARNING — DxRenderer instance not found");
    } // end Scan 1 block

    // ── Scan 2: HWND static ───────────────────────────────────────────────────
    //
    // Anchor: "parentHwnd" string in .rdata
    // Method: find LEA Rxx,[RIP+rel] → walk back to prologue → scan forward
    //         for FF 15 (call [CreateWindowExA]) followed by 48 8B F0
    //         (mov rsi, rax) → then scan forward up to 256 bytes for
    //         48 89 35 ?? ?? ?? ?? (mov [RIP+rel32], rsi) — the HWND store.
    //         This sequence is consistent across Frostbite titles.
    {
        uint8_t* strAddr = findStr(modBase, modSize, "parentHwnd");
        if (!strAddr)
        {
            overlayLog("resolveAddresses: 'parentHwnd' string not found");
        }
        else
        {
            overlayLogFmt("resolveAddresses: 'parentHwnd' string at %p", (void*)strAddr);

            uint8_t* leaInsn = findLEAtoTarget(modBase, modSize, strAddr);
            if (!leaInsn)
            {
                overlayLog("resolveAddresses: no LEA to 'parentHwnd' found");
            }
            else
            {
                overlayLogFmt("resolveAddresses: LEA to 'parentHwnd' at %p", (void*)leaInsn);

                uint8_t* fnStart = walkBack(leaInsn);
                if (!fnStart)
                {
                    overlayLog("resolveAddresses: could not walk back to prologue");
                }
                else
                {
                    overlayLogFmt("resolveAddresses: window creation fn at %p", (void*)fnStart);

                    // Scan forward up to 8KB for the CreateWindowExA call
                    // followed immediately by mov rsi, rax
                    uint8_t* scan = fnStart;
                    uint8_t* scanEnd = fnStart + 8192;
                    if (scanEnd > modEnd) scanEnd = modEnd;

                    // Cache user32 lookups once
                    HMODULE hUser32 = GetModuleHandleA("user32.dll");
                    void* cwexA = hUser32 ? GetProcAddress(hUser32, "CreateWindowExA") : nullptr;
                    void* cwexW = hUser32 ? GetProcAddress(hUser32, "CreateWindowExW") : nullptr;

                    bool found = false;
                    for (; !found && scan + 9 <= scanEnd; ++scan)
                    {
                        // Must be an indirect call: FF 15 ?? ?? ?? ??
                        if (scan[0] != 0xFF || scan[1] != 0x15) continue;

                        // After the 6-byte call, accept any of:
                                                //   48 8B F0  (mov rsi, rax) — load form — GW1/Rivals/BF4
                                                //   48 8B F8  (mov rdi, rax) — load form — GW2/EA-AC
                                                //   48 89 C6  (mov rsi, rax) — store form — NFS 2016
                                                //   48 89 C7  (mov rdi, rax) — store form — alternate
                                                //   49 89 C6  (mov r14, rax) — REX.B store form
                        bool movRsi = (scan[6] == 0x48 && scan[7] == 0x8B && scan[8] == 0xF0)
                            || (scan[6] == 0x48 && scan[7] == 0x89 && scan[8] == 0xC6);
                        bool movRdi = (scan[6] == 0x48 && scan[7] == 0x8B && scan[8] == 0xF8)
                            || (scan[6] == 0x48 && scan[7] == 0x89 && scan[8] == 0xC7);
                        bool movR14 = (scan[6] == 0x49 && scan[7] == 0x89 && scan[8] == 0xC6);
                        if (!movRsi && !movRdi && !movR14) continue;

                        // Verify IAT slot points to CreateWindowExA or CreateWindowExW
                        int32_t iatRel = 0;
                        memcpy(&iatRel, scan + 2, 4);
                        uint8_t* iatSlot = scan + 6 + iatRel;
                        uint64_t fnPtr = 0;
                        if (!FrostbiteConsole::safeRead64(iatSlot, &fnPtr)) continue;
                        void* resolvedFn = reinterpret_cast<void*>(static_cast<uintptr_t>(fnPtr));
                        if (resolvedFn != cwexA && resolvedFn != cwexW) continue;

                        overlayLogFmt("resolveAddresses: found CreateWindowEx%s call at %p (mov r%s)",
                            (resolvedFn == cwexW ? "W" : "A"), (void*)scan,
                            movRdi ? "di" : movR14 ? "14" : "si");

                        // Scan forward up to 256 bytes for the HWND store.
                        // Accept all three RIP-relative mov-register-to-memory forms:
                        //   48 89 35 ?? ?? ?? ??  mov [RIP+rel32], rsi  — GW1/Rivals
                        //   48 89 3D ?? ?? ?? ??  mov [RIP+rel32], rdi  — GW2/EA-AC
                        //   4C 89 35 ?? ?? ?? ??  mov [RIP+rel32], r14  — alternate
                        uint8_t* inner = scan + 9;
                        uint8_t* innerEnd = inner + 256;
                        if (innerEnd > modEnd) innerEnd = modEnd;

                        for (; inner + 7 <= innerEnd; ++inner)
                        {
                            bool isRsi = (inner[0] == 0x48 && inner[1] == 0x89 && inner[2] == 0x35);
                            bool isRdi = (inner[0] == 0x48 && inner[1] == 0x89 && inner[2] == 0x3D);
                            bool isR14 = (inner[0] == 0x4C && inner[1] == 0x89 && inner[2] == 0x35);
                            if (!isRsi && !isRdi && !isR14) continue;

                            int32_t rel = 0;
                            memcpy(&rel, inner + 3, 4);
                            uint8_t* target = inner + 7 + rel;

                            if (target < modBase || target >= modEnd) continue;
                            MEMORY_BASIC_INFORMATION mbi = {};
                            if (!VirtualQuery(target, &mbi, sizeof(mbi))) continue;
                            if (mbi.State != MEM_COMMIT) continue;
                            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;

                            g_hwndStaticAddr = reinterpret_cast<HWND*>(target);
                            overlayLogFmt("resolveAddresses: HWND static addr=%p (found at %p, enc=%s)",
                                (void*)target, (void*)inner,
                                isRdi ? "48 89 3D" : isR14 ? "4C 89 35" : "48 89 35");
                            found = true;
                            break;
                        }
                    }
                }
            }
        }

        // Fallback: scan all non-executable committed pages in the module for
                // a pointer-sized value that IsWindow() confirms is a valid HWND.
                // This handles games (e.g. NFS Payback) where the HWND is stored as a
                // field inside a manager object rather than in a dedicated RIP-relative
                // static, so no "mov [RIP+rel32], rsi" pattern exists to find.
        if (!g_hwndStaticAddr)
        {
            overlayLog("resolveAddresses: HWND static not found via pattern — trying IsWindow scan");
            MEMORY_BASIC_INFORMATION mbi = {};
            for (uint8_t* cursor = modBase; cursor < modEnd && !g_hwndStaticAddr; )
            {
                if (!VirtualQuery(cursor, &mbi, sizeof(mbi))) break;
                uint8_t* rBase = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                uint8_t* rEnd = rBase + mbi.RegionSize;
                if (rBase < modBase) rBase = modBase;
                if (rEnd > modEnd)  rEnd = modEnd;

                bool isData = (mbi.State == MEM_COMMIT) &&
                    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));

                if (isData && rEnd > rBase)
                {
                    for (uint8_t* p = rBase; p + 8 <= rEnd && !g_hwndStaticAddr; p += 8)
                    {
                        uint64_t val = 0;
                        if (!FrostbiteConsole::safeRead64(p, &val) || !val) continue;
                        // HWND values on 64-bit Windows are 32-bit handles
                        // zero-extended — the upper 32 bits are always 0.
                        if (val >> 32) continue;
                        HWND candidate = reinterpret_cast<HWND>(
                            static_cast<uintptr_t>(val));
                        if (!IsWindow(candidate)) continue;
                        // Confirm it's a top-level window (no parent) with a
                        // non-zero client area — rules out child controls and
                        // message-only windows which also pass IsWindow().
                        if (GetParent(candidate)) continue;
                        RECT rc = {};
                        if (!GetClientRect(candidate, &rc)) continue;
                        if (rc.right < 320 || rc.bottom < 240) continue;
                        g_hwndStaticAddr = reinterpret_cast<HWND*>(p);
                        overlayLogFmt("resolveAddresses: HWND found via IsWindow scan at %p = %p",
                            (void*)p, (void*)candidate);
                    }
                }

                uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= reinterpret_cast<uintptr_t>(cursor)) break;
                cursor = reinterpret_cast<uint8_t*>(next);
            }
        }

        if (!g_hwndStaticAddr)
            overlayLog("resolveAddresses: WARNING — HWND static not found");
    }
}

// Walk all DXGI adapters/outputs to find the swap chain whose output
// device matches our HWND, then derive device+context from it.
// This is offset-free and works on any Frostbite title.
static bool resolveD3DFromDXGI(HWND hwnd)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory))) || !factory)
    {
        overlayLog("resolveD3DFromDXGI: CreateDXGIFactory1 failed");
        return false;
    }

    factory->Release();

    uint8_t* dr = nullptr;
    // Engine-field (deferred two-level dereference) takes priority — it is the result
    // of a validated post-call store pattern and is more reliable than the tentative
    // direct singleton which may be a false-positive candidate from an earlier caller.
    if (g_dxRendererBaseSlot && *g_dxRendererBaseSlot)
    {
        uint8_t* base = *g_dxRendererBaseSlot;
        uint64_t fieldVal = 0;
        if (FrostbiteConsole::safeRead64(base + g_dxRendererFieldOffset, &fieldVal) && fieldVal)
        {
            dr = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(fieldVal));
            overlayLogFmt("resolveD3DFromDXGI: deferred engine-field dr=%p "
                "(base=%p off=0x%X)", (void*)dr, (void*)base, g_dxRendererFieldOffset);
        }
        else
        {
            overlayLogFmt("resolveD3DFromDXGI: deferred engine-field null/unreadable "
                "(base=%p off=0x%X fieldVal=%p) — falling back to direct slot",
                (void*)base, g_dxRendererFieldOffset, (void*)(uintptr_t)fieldVal);
            // Fall through to direct slot below
            if (g_dxRendererInstanceAddr && *g_dxRendererInstanceAddr)
                dr = *g_dxRendererInstanceAddr;
        }
    }
    else if (g_dxRendererInstanceAddr && *g_dxRendererInstanceAddr)
    {
        dr = *g_dxRendererInstanceAddr;
    }
    // Module bounds — needed for the .data scan in the SwapChain fallback
        // and for the D3D12 null-dr scan below.
    uint8_t* modBase = nullptr;
    uint8_t* modEnd = nullptr;
    {
        HMODULE mods[1] = {};
        DWORD needed = 0;
        MODULEINFO mi = {};
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) &&
            mods[0] &&
            GetModuleInformation(GetCurrentProcess(), mods[0], &mi, sizeof(mi)))
        {
            modBase = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
            modEnd = modBase + mi.SizeOfImage;
        }
        overlayLogFmt("resolveD3DFromDXGI: modBase=%p modEnd=%p", (void*)modBase, (void*)modEnd);
    }

    if (!dr)
    {
        overlayLog("resolveD3DFromDXGI: DxRenderer pointer is null — attempting D3D12 swap-chain scan");

        // ── D3D12 fallback ────────────────────────────────────────────────────
        // Dragon Age: The Veilguard (and other D3D12-only Frostbite titles) do
        // not expose a DxRenderer singleton findable by the "RenderDevice" LEA
        // heuristic. Instead, scan the game module's non-executable data pages
        // for an IDXGISwapChain whose vtable lives in dxgi.dll, then use
        // D3D11On12CreateDevice to stand up a D3D11 wrapper device/context on
        // top of the game's D3D12 command queue. The rest of the overlay (shaders,
        // font atlas, present hook) is unchanged.

        HMODULE hDXGIscan = GetModuleHandleA("dxgi.dll");
        HMODULE hD3D12scan = GetModuleHandleA("d3d12.dll");

        IDXGISwapChain* foundSC = nullptr;

        // D3D12 flip-model swap chains are allocated by DXGI internally —
        // the pointer is NOT stored in the game module's data pages. Scan the
        // entire committed process address space instead.
        // Guard: vtable must be in dxgi.dll, object must QI as IDXGISwapChain3
        // (flip-model interface always supported by D3D12 swap chains), and the
        // swap chain must have a back-buffer count >= 2 (rules out dummy objects).
        {
            // Prefer IDXGISwapChain3 — all D3D12 flip-model swap chains support it.
                // We fall back to IDXGISwapChain if QI for 3 fails.
            MEMORY_BASIC_INFORMATION mbi2 = {};
            uint8_t* cursor = reinterpret_cast<uint8_t*>(0x10000);
            uint8_t* limit = reinterpret_cast<uint8_t*>(0x00007FFFFFFFFFFFllu);

            // Cache dxgi.dll address range so the vtable check is a fast
            // range compare instead of a GetModuleHandleEx call per pointer.
            uint8_t* dxgiBase = nullptr;
            uint8_t* dxgiEnd = nullptr;
            if (hDXGIscan)
            {
                MODULEINFO dxgiMI = {};
                if (GetModuleInformation(GetCurrentProcess(), hDXGIscan, &dxgiMI, sizeof(dxgiMI)))
                {
                    dxgiBase = reinterpret_cast<uint8_t*>(dxgiMI.lpBaseOfDll);
                    dxgiEnd = dxgiBase + dxgiMI.SizeOfImage;
                }
            }

            while (cursor < limit && !foundSC)
            {
                if (!VirtualQuery(cursor, &mbi2, sizeof(mbi2))) break;

                uint8_t* rBase2 = reinterpret_cast<uint8_t*>(mbi2.BaseAddress);
                uint8_t* rEnd2 = rBase2 + mbi2.RegionSize;

                // Only scan committed, non-executable, readable private or
                // mapped pages. Skip image-backed (MEM_IMAGE), guard, noaccess.
                // MEM_IMAGE pages are DLL/EXE mappings — swap chains are never
                // stored there, and skipping them cuts scan time significantly.
                bool candidate =
                    (mbi2.State == MEM_COMMIT) &&
                    !(mbi2.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    !(mbi2.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                    (mbi2.Type == MEM_PRIVATE || mbi2.Type == MEM_MAPPED);

                if (candidate && rEnd2 > rBase2)
                {
                    // Scan page-by-page under __try — GPU-mapped and certain
                    // DXGI-internal regions report as readable via VirtualQuery
                    // but fault on access.
                    for (uint8_t* pageBase = rBase2;
                        pageBase < rEnd2 && !foundSC;
                        pageBase += 0x1000)
                    {
                        uint8_t* pageEnd = pageBase + 0x1000;
                        if (pageEnd > rEnd2) pageEnd = rEnd2;

                        __try
                        {
                            for (uint8_t* p2 = pageBase;
                                p2 + 8 <= pageEnd && !foundSC;
                                p2 += 8)
                            {
                                uint64_t val2 = 0;
                                if (!FrostbiteConsole::safeRead64(p2, &val2) || !val2) continue;
                                if (val2 < 0x10000ULL || val2 > 0x00007FFFFFFFFFFFllu) continue;

                                // Vtable pointer check — fast range compare
                                // against cached dxgi.dll bounds; falls back to
                                // GetModuleHandleEx only if bounds are unavailable.
                                uint64_t vtbl2 = 0;
                                if (!FrostbiteConsole::safeRead64(
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(val2)),
                                    &vtbl2) || !vtbl2) continue;

                                uint8_t* vtblPtr = reinterpret_cast<uint8_t*>(
                                    static_cast<uintptr_t>(vtbl2));
                                bool inDxgi = false;
                                if (dxgiBase && vtblPtr >= dxgiBase && vtblPtr < dxgiEnd)
                                {
                                    inDxgi = true;
                                }
                                else if (!dxgiBase)
                                {
                                    HMODULE hModVtbl = nullptr;
                                    inDxgi = GetModuleHandleExA(
                                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCSTR>(vtblPtr),
                                        &hModVtbl) && hModVtbl == hDXGIscan;
                                }
                                if (!inDxgi) continue;

                                IUnknown* unk2 = reinterpret_cast<IUnknown*>(
                                    static_cast<uintptr_t>(val2));

                                // Try IDXGISwapChain3 first (D3D12 flip-model).
                                IDXGISwapChain3* sc3 = nullptr;
                                if (SUCCEEDED(unk2->QueryInterface(
                                    __uuidof(IDXGISwapChain3),
                                    reinterpret_cast<void**>(&sc3))) && sc3)
                                {
                                    DXGI_SWAP_CHAIN_DESC scd = {};
                                    if (SUCCEEDED(sc3->GetDesc(&scd)) &&
                                        scd.BufferCount >= 2 &&
                                        scd.OutputWindow != nullptr &&
                                        IsWindow(scd.OutputWindow))
                                    {
                                        overlayLogFmt("resolveD3DFromDXGI: D3D12 scan"
                                            " found IDXGISwapChain3 at [%p] = %p"
                                            " bufs=%u hwnd=%p",
                                            (void*)p2, (void*)sc3,
                                            scd.BufferCount,
                                            (void*)scd.OutputWindow);
                                        foundSC = sc3;
                                        sc3->Release();
                                    }
                                    else
                                    {
                                        sc3->Release();
                                    }
                                }
                                else
                                {
                                    // Fallback: plain IDXGISwapChain
                                    IDXGISwapChain* sc2 = nullptr;
                                    if (SUCCEEDED(unk2->QueryInterface(
                                        __uuidof(IDXGISwapChain),
                                        reinterpret_cast<void**>(&sc2))) && sc2)
                                    {
                                        DXGI_SWAP_CHAIN_DESC scd = {};
                                        if (SUCCEEDED(sc2->GetDesc(&scd)) &&
                                            scd.BufferCount >= 2 &&
                                            scd.OutputWindow != nullptr &&
                                            IsWindow(scd.OutputWindow))
                                        {
                                            overlayLogFmt("resolveD3DFromDXGI: D3D12 scan"
                                                " found IDXGISwapChain at [%p] = %p"
                                                " bufs=%u hwnd=%p",
                                                (void*)p2, (void*)sc2,
                                                scd.BufferCount,
                                                (void*)scd.OutputWindow);
                                            foundSC = sc2;
                                            sc2->Release();
                                        }
                                        else
                                        {
                                            sc2->Release();
                                        }
                                    }
                                }
                            }
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }

                uintptr_t next2 = reinterpret_cast<uintptr_t>(mbi2.BaseAddress) + mbi2.RegionSize;
                if (next2 <= reinterpret_cast<uintptr_t>(cursor)) break;
                cursor = reinterpret_cast<uint8_t*>(next2);
            }
        }

        if (!foundSC)
        {
            overlayLog("resolveD3DFromDXGI: D3D12 scan — no IDXGISwapChain found, aborting");
            return false;
        }

        // Recover the D3D12 device from the swap chain.
        ID3D12Device* d12dev = nullptr;
        if (FAILED(foundSC->GetDevice(__uuidof(ID3D12Device),
            reinterpret_cast<void**>(&d12dev))) || !d12dev)
        {
            overlayLog("resolveD3DFromDXGI: D3D12 scan — GetDevice(ID3D12Device) failed");
            return false;
        }
        overlayLogFmt("resolveD3DFromDXGI: D3D12 device=%p", (void*)d12dev);

        // Scan the game's data pages for the DIRECT command queue the swap
        // chain was created with. We identify it by vtable in d3d12.dll and
        // D3D12_COMMAND_LIST_TYPE_DIRECT reported by GetDesc().
        ID3D12CommandQueue* d12queue = nullptr;
        {
            MEMORY_BASIC_INFORMATION mqmbi = {};
            // Dragon Age: The Veilguard stores the command queue pointer inside a
            // heap-allocated engine object, not inside the game module's .data
            // section. Scan the full process address space (same strategy used above
            // for the swap chain) so D3D12-only titles are covered. Other games that
            // reach this path via the DxRenderer-null branch already have no queue
            // in the module image anyway, so widening the scan is always correct.
            uint8_t* cursor = reinterpret_cast<uint8_t*>(0x10000);
            uint8_t* limit = reinterpret_cast<uint8_t*>(0x00007FFFFFFFFFFFllu);
            for (; cursor < limit && !d12queue; )
            {
                if (!VirtualQuery(cursor, &mqmbi, sizeof(mqmbi))) break;
                uint8_t* rBase3 = reinterpret_cast<uint8_t*>(mqmbi.BaseAddress);
                uint8_t* rEnd3 = rBase3 + mqmbi.RegionSize;

                bool isData3 = (mqmbi.State == MEM_COMMIT) &&
                    !(mqmbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    !(mqmbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                    (mqmbi.Type == MEM_PRIVATE || mqmbi.Type == MEM_MAPPED);

                if (isData3 && rEnd3 > rBase3)
                {
                    for (uint8_t* pageBase3 = rBase3;
                        pageBase3 < rEnd3 && !d12queue;
                        pageBase3 += 0x1000)
                    {
                        uint8_t* pageEnd3 = pageBase3 + 0x1000;
                        if (pageEnd3 > rEnd3) pageEnd3 = rEnd3;
                        __try
                        {
                            for (uint8_t* p3 = pageBase3; p3 + 8 <= pageEnd3 && !d12queue; p3 += 8)
                            {
                                uint64_t val3 = 0;
                                if (!FrostbiteConsole::safeRead64(p3, &val3) || !val3) continue;
                                if (val3 < 0x10000ULL || val3 > 0x00007FFFFFFFFFFFllu) continue;
                                if (val3 >= reinterpret_cast<uintptr_t>(modBase) &&
                                    val3 < reinterpret_cast<uintptr_t>(modEnd)) continue;

                                // Gate on vtable module membership before any COM call.
                                // Both dereferences and the QI are inside this __try frame.
                                uint64_t vtbl3 = 0;
                                if (!FrostbiteConsole::safeRead64(
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(val3)),
                                    &vtbl3) || !vtbl3) continue;

                                // Fast range check against cached d3d12.dll bounds.
                                {
                                    static uint8_t* s_d3d12Base = nullptr;
                                    static uint8_t* s_d3d12End = nullptr;
                                    if (!s_d3d12Base && hD3D12scan)
                                    {
                                        MODULEINFO d12MI = {};
                                        if (GetModuleInformation(GetCurrentProcess(),
                                            hD3D12scan, &d12MI, sizeof(d12MI)))
                                        {
                                            s_d3d12Base = reinterpret_cast<uint8_t*>(d12MI.lpBaseOfDll);
                                            s_d3d12End = s_d3d12Base + d12MI.SizeOfImage;
                                        }
                                    }
                                    uint8_t* vtbl3Ptr = reinterpret_cast<uint8_t*>(
                                        static_cast<uintptr_t>(vtbl3));
                                    bool inD3D12 = false;
                                    if (s_d3d12Base && vtbl3Ptr >= s_d3d12Base && vtbl3Ptr < s_d3d12End)
                                        inD3D12 = true;
                                    else if (!s_d3d12Base)
                                    {
                                        HMODULE hModQ = nullptr;
                                        inD3D12 = GetModuleHandleExA(
                                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            reinterpret_cast<LPCSTR>(vtbl3Ptr),
                                            &hModQ) && hModQ == hD3D12scan;
                                    }
                                    if (!inD3D12) continue;
                                }

                                HMODULE hModQ = nullptr;
                                if (!GetModuleHandleExA(
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(vtbl3)),
                                    &hModQ) || hModQ != hD3D12scan) continue;

                                // Verify the vtable's first slot is itself a committed
                                // executable address — catches objects whose vtable pointer
                                // resolved to d3d12.dll by coincidence but is malformed.
                                uint64_t vtblSlot0 = 0;
                                if (!FrostbiteConsole::safeRead64(
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(vtbl3)),
                                    &vtblSlot0) || !vtblSlot0) continue;
                                {
                                    MEMORY_BASIC_INFORMATION vtslMbi = {};
                                    if (!VirtualQuery(reinterpret_cast<void*>(
                                        static_cast<uintptr_t>(vtblSlot0)), &vtslMbi, sizeof(vtslMbi)))
                                        continue;
                                    if (!(vtslMbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                                        continue;
                                }

                                IUnknown* unkQ = reinterpret_cast<IUnknown*>(
                                    static_cast<uintptr_t>(val3));
                                ID3D12CommandQueue* cq = nullptr;
                                if (SUCCEEDED(unkQ->QueryInterface(
                                    __uuidof(ID3D12CommandQueue),
                                    reinterpret_cast<void**>(&cq))) && cq)
                                {
                                    D3D12_COMMAND_QUEUE_DESC qd = cq->GetDesc();
                                    if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
                                    {
                                        overlayLogFmt("resolveD3DFromDXGI: D3D12 DIRECT queue"
                                            " at [%p] = %p", (void*)p3, (void*)cq);
                                        d12queue = cq;
                                        // keep this ref — we store it in g_d3d12Queue
                                    }
                                    else
                                    {
                                        cq->Release();
                                    }
                                }
                            }
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }

                uintptr_t next3 = reinterpret_cast<uintptr_t>(mqmbi.BaseAddress) + mqmbi.RegionSize;
                if (next3 <= reinterpret_cast<uintptr_t>(cursor)) break;
                cursor = reinterpret_cast<uint8_t*>(next3);
            }
        }

        if (!d12queue)
        {
            overlayLog("resolveD3DFromDXGI: D3D12 scan — no DIRECT command queue found, aborting");
            d12dev->Release();
            return false;
        }

        overlayLogFmt("resolveD3DFromDXGI: D3D12 mode — using game queue=%p device=%p",
            (void*)d12queue, (void*)d12dev);

        // Store the game's queue directly. We will inject our command list
        // via ExecuteCommandLists on this queue — no D3D11On12, no second device,
        // no resource ownership conflict.
        g_isD3D12Mode = true;
        g_d3d12Device = d12dev;   // keep ref
        g_d3d12Queue = d12queue;  // keep ref — game's queue, we submit on it

        // For the D3D11 overlay path (shaders, font, vb) we create a plain
        // D3D11 device on the same adapter — completely independent, never
        // touches the swap chain or D3D12 resources.
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(d12dev->QueryInterface(__uuidof(IDXGIDevice),
            reinterpret_cast<void**>(&dxgiDev))) && dxgiDev)
        {
            dxgiDev->GetAdapter(&adapter);
            dxgiDev->Release();
        }
        // D3D12Device doesn't implement IDXGIDevice — use CreateDXGIFactory instead
        if (!adapter)
        {
            IDXGIFactory4* fac = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                reinterpret_cast<void**>(&fac))) && fac)
            {
                DXGI_ADAPTER_DESC adDesc = {};
                for (UINT i = 0; ; ++i)
                {
                    IDXGIAdapter1* a = nullptr;
                    if (FAILED(fac->EnumAdapters1(i, &a))) break;
                    // Pick the adapter that owns our device's LUID
                    DXGI_ADAPTER_DESC1 ad1 = {};
                    a->GetDesc1(&ad1);
                    if (ad1.AdapterLuid.LowPart == d12dev->GetAdapterLuid().LowPart &&
                        ad1.AdapterLuid.HighPart == d12dev->GetAdapterLuid().HighPart)
                    {
                        adapter = a;
                        break;
                    }
                    a->Release();
                }
                fac->Release();
            }
        }

        ID3D11Device* d11dev = nullptr;
        ID3D11DeviceContext* d11ctx = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr11 = D3D11CreateDevice(
            adapter,
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0,
            D3D11_SDK_VERSION,
            &d11dev, &fl, &d11ctx);
        if (adapter) adapter->Release();

        if (FAILED(hr11) || !d11dev || !d11ctx)
        {
            overlayLogFmt("resolveD3DFromDXGI: D3D11CreateDevice failed hr=0x%08X", (unsigned)hr11);
            if (d11ctx) d11ctx->Release();
            if (d11dev) d11dev->Release();
            d12queue->Release();
            d12dev->Release();
            return false;
        }

        overlayLogFmt("resolveD3DFromDXGI: D3D12 mode OK — d11dev=%p d11ctx=%p (independent, no 11on12)",
            (void*)d11dev, (void*)d11ctx);

        g_resolvedSwapChain = foundSC;
        g_resolvedDevice = d11dev;
        g_resolvedContext = d11ctx;
        return true;
    }

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");

    // GetModuleHandleA("dxgi.dll") returns our own proxy when loaded as dxgi.dll
    // from the game directory. Load the real system dxgi by full path instead.
    HMODULE hDXGI = nullptr;
    {
        char sysDir[MAX_PATH] = {};
        char realDxgiPath[MAX_PATH] = {};
        GetSystemDirectoryA(sysDir, MAX_PATH);
        lstrcpyA(realDxgiPath, sysDir);
        lstrcatA(realDxgiPath, "\\dxgi.dll");
        hDXGI = LoadLibraryExA(realDxgiPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!hDXGI)
            hDXGI = GetModuleHandleA("dxgi.dll"); // fallback for normal inject path
    }

    auto getModuleForPtr = [](void* p, HMODULE* outMod) -> bool {
        return GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(p), outMod) && *outMod;
        };

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // Helper: try to identify a COM pointer as SwapChain/Device/Context
    auto tryIdentifyCOMPtr = [&](uint64_t val, const char* location) -> bool
        {
            if (!val) return false;
            uint64_t vtbl = 0;
            if (!FrostbiteConsole::safeRead64(
                reinterpret_cast<void*>(static_cast<uintptr_t>(val)), &vtbl) || !vtbl) return false;
            void* vtblPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(vtbl));
            HMODULE hMod = nullptr;
            if (!getModuleForPtr(vtblPtr, &hMod)) return false;

            if (!swapChain && hMod == hDXGI)
            {
                IUnknown* unk = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(val));
                IDXGISwapChain* sc = nullptr;
                if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                    reinterpret_cast<void**>(&sc))) && sc)
                {
                    swapChain = sc;
                    sc->Release();
                    overlayLogFmt("resolveD3DFromDXGI: SwapChain at %s = %p", location, (void*)sc);
                }
            }

            if (hMod == hD3D11)
            {
                IUnknown* unk = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(val));
                if (!device)
                {
                    ID3D11Device* dev = nullptr;
                    if (SUCCEEDED(unk->QueryInterface(__uuidof(ID3D11Device),
                        reinterpret_cast<void**>(&dev))) && dev)
                    {
                        device = dev;
                        dev->Release();
                        overlayLogFmt("resolveD3DFromDXGI: Device at %s = %p", location, (void*)dev);
                    }
                }
                if (!context)
                {
                    ID3D11DeviceContext* ctx = nullptr;
                    if (SUCCEEDED(unk->QueryInterface(__uuidof(ID3D11DeviceContext),
                        reinterpret_cast<void**>(&ctx))) && ctx)
                    {
                        context = ctx;
                        ctx->Release();
                        overlayLogFmt("resolveD3DFromDXGI: Context at %s = %p", location, (void*)ctx);
                    }
                }
            }

            return (swapChain && device && context);
        };

    // Helper: is this a committed readable pointer (heap or mapped, not guard/noaccess)?
    auto isReadablePtr = [](void* p) -> bool {
        if (!p) return false;
        MEMORY_BASIC_INFORMATION m = {};
        if (!VirtualQuery(p, &m, sizeof(m))) return false;
        return m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        };

    // Helper: does this value look like a DXGI COM object (vtable in dxgi.dll)?
    // Used by Pass3 and Pass4 to gate QueryInterface calls safely.
    auto isDXGICOMObject = [&](uint64_t val) -> bool
        {
            if (!val || val < 0x10000ULL || val > 0x00007FFFFFFFFFFFllu) return false;
            uint64_t vtbl = 0;
            if (!FrostbiteConsole::safeRead64(
                reinterpret_cast<void*>(static_cast<uintptr_t>(val)), &vtbl) || !vtbl)
                return false;
            HMODULE hMod = nullptr;
            return GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(vtbl)),
                &hMod) && hMod && hMod == hDXGI;
        };

    // Pass 1: scan first 640 bytes of the DxRenderer object directly.
    overlayLogFmt("resolveD3DFromDXGI: Pass1 scanning dr=%p (range=640B)", (void*)dr);
    for (size_t off = 0; off + 8 <= 640; off += 8)
    {
        uint64_t val = 0;
        if (!FrostbiteConsole::safeRead64(dr + off, &val) || !val) continue;
        char loc[64];
        snprintf(loc, sizeof(loc), "dr+0x%zX", off);
        tryIdentifyCOMPtr(val, loc);
        if (swapChain && device && context) break;
    }

    overlayLogFmt("resolveD3DFromDXGI: after Pass1 sc=%p dev=%p ctx=%p",
        (void*)swapChain, (void*)device, (void*)context);

    // GW2/EA-AC pattern: the primary scan found a slot (g_dxRendererInstanceAddr) but
    // it points to the engine singleton, not the DxRenderer sub-object. If Pass1 found
    // nothing and we have no engine-field path, retry resolution via FallbackB by
    // attempting a two-level dereference: scan the engine object for a sub-object whose
    // first 640 bytes contain a DXGI/D3D11 COM pointer.
    if (!swapChain && !device && !context && g_dxRendererInstanceAddr && !g_dxRendererBaseSlot)
    {
        overlayLog("resolveD3DFromDXGI: Pass1 empty — scanning engine object for D3D objects");
        uint8_t* engineObj = dr;

        // First: scan the engine object itself directly for Device/Context/SwapChain.
        // GW2 stores Device and Context directly on the engine singleton.
        overlayLog("resolveD3DFromDXGI: direct engine object scan (16KB)");
        for (size_t off = 0; off + 8 <= 16384 && !(swapChain && device && context); off += 8)
        {
            uint64_t val = 0;
            if (!FrostbiteConsole::safeRead64(engineObj + off, &val) || !val) continue;
            char loc[64];
            snprintf(loc, sizeof(loc), "engine+0x%zX", off);
            tryIdentifyCOMPtr(val, loc);
        }
        overlayLogFmt("resolveD3DFromDXGI: after direct engine scan sc=%p dev=%p ctx=%p",
            (void*)swapChain, (void*)device, (void*)context);

        // Second: scan every sub-object pointer in the engine object.
        // Call tryIdentifyCOMPtr on each value inside each sub-object —
        // this finds SwapChain, Device, and Context across all sub-objects,
        // not just the first one that happens to have a DXGI pointer.
        for (size_t off = 0; off + 8 <= 16384 && !(swapChain && device && context); off += 8)
        {
            uint64_t subPtr = 0;
            if (!FrostbiteConsole::safeRead64(engineObj + off, &subPtr) || !subPtr) continue;
            if (subPtr < 0x10000ULL || subPtr > 0x00007FFFFFFFFFFFllu) continue;
            if (modBase && subPtr >= reinterpret_cast<uintptr_t>(modBase) &&
                subPtr < reinterpret_cast<uintptr_t>(modEnd)) continue;

            MEMORY_BASIC_INFORMATION smbi = {};
            if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(subPtr)),
                &smbi, sizeof(smbi))) continue;
            if (smbi.State != MEM_COMMIT) continue;
            if (smbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
            if (smbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;
            if (smbi.RegionSize < 4 * 1024) continue;

            uint8_t* sub = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(subPtr));
            __try
            {
                for (size_t so = 0; so + 8 <= 640 && !(swapChain && device && context); so += 8)
                {
                    uint64_t val = 0;
                    if (!FrostbiteConsole::safeRead64(sub + so, &val) || !val) continue;
                    char loc[64];
                    snprintf(loc, sizeof(loc), "engine+0x%zX->+0x%zX", off, so);
                    if (tryIdentifyCOMPtr(val, loc))
                    {
                        overlayLogFmt("resolveD3DFromDXGI: found DxRenderer sub-field at engine+0x%zX = %p",
                            off, (void*)(uintptr_t)subPtr);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        overlayLogFmt("resolveD3DFromDXGI: engine sub-field scan done sc=%p dev=%p ctx=%p",
            (void*)swapChain, (void*)device, (void*)context);
    }

    // Pass2: scan only the first 512 bytes of DxRenderer for a nested Screen
    // pointer, then scan 256 bytes of that sub-object for a DXGI SwapChain.
    // GW1: dr+0x38->+0xF0. Rivals: dr+0x38->+0xB0.
    // The entire inner body is wrapped in __try so any fault is contained.
    // Only sub-objects in non-executable committed memory are entered.
    // Only values whose vtable is in dxgi.dll are QI'd.
    if (!swapChain)
    {
        uintptr_t drBase = reinterpret_cast<uintptr_t>(dr);

        for (size_t off = 0; off + 8 <= 512 && !swapChain; off += 8)
        {
            uint64_t subPtr = 0;
            if (!FrostbiteConsole::safeRead64(dr + off, &subPtr) || !subPtr) continue;
            if (subPtr < 0x10000ULL || subPtr > 0x00007FFFFFFFFFFFllu) continue;
            if (modBase && subPtr >= reinterpret_cast<uintptr_t>(modBase) &&
                subPtr < reinterpret_cast<uintptr_t>(modEnd)) continue;

            MEMORY_BASIC_INFORMATION subMbi = {};
            if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(subPtr)),
                &subMbi, sizeof(subMbi))) continue;
            if (subMbi.State != MEM_COMMIT) continue;
            if (subMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
            if (subMbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;

            overlayLogFmt("resolveD3DFromDXGI: Pass2 checking dr+0x%zX = %p",
                off, (void*)(uintptr_t)subPtr);

            uint8_t* sub = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(subPtr));
            __try
            {
                for (size_t subOff = 0; subOff + 8 <= 256 && !swapChain; subOff += 8)
                {
                    uint64_t val = 0;
                    if (!FrostbiteConsole::safeRead64(sub + subOff, &val) || !val) continue;
                    if (!isDXGICOMObject(val)) continue;
                    IUnknown* unk = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(val));
                    IDXGISwapChain* sc = nullptr;
                    if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                        reinterpret_cast<void**>(&sc))) && sc)
                    {
                        swapChain = sc;
                        sc->Release();
                        overlayLogFmt("resolveD3DFromDXGI: Pass2 SwapChain at dr+0x%zX->+0x%zX = %p",
                            off, subOff, (void*)sc);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (swapChain)
            overlayLogFmt("resolveD3DFromDXGI: Pass2 SwapChain found: %p", (void*)swapChain);
        else
            overlayLog("resolveD3DFromDXGI: Pass2 exhausted");
    }

    // Derive context from device if not found directly.
    if (device && !context)
    {
        device->GetImmediateContext(&context);
        if (context)
        {
            context->Release();
            overlayLogFmt("resolveD3DFromDXGI: Context derived from GetImmediateContext: %p",
                (void*)context);
        }
    }

    // Pass4: scan the vendor screen objects stored at dr+0xAC8/0xAD0/0xAD8.
    // These are populated by the vendor-specific init functions (AMD/NVIDIA/Intel)
    // called from 0x141AB4320. The swap chain is stored inside one of these.
    // Use isDXGICOMObject to gate QI calls safely.
    if (!swapChain && dr)
    {
        overlayLog("resolveD3DFromDXGI: Pass4 scanning vendor screen objects");
        const size_t vendorOffsets[] = { 0xAC8, 0xAD0, 0xAD8, 0xA08, 0xA10, 0xA18, 0xA20 };
        for (size_t voff : vendorOffsets)
        {
            if (swapChain) break;
            uint64_t screenPtr = 0;
            if (!FrostbiteConsole::safeRead64(dr + voff, &screenPtr) || !screenPtr) continue;
            if (screenPtr < 0x10000ULL || screenPtr > 0x00007FFFFFFFFFFFllu) continue;

            MEMORY_BASIC_INFORMATION smbi = {};
            if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(screenPtr)),
                &smbi, sizeof(smbi))) continue;
            if (smbi.State != MEM_COMMIT) continue;
            if (smbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
            if (smbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;
            if (smbi.RegionSize < 8 * 1024) continue;

            overlayLogFmt("resolveD3DFromDXGI: Pass4 scanning dr+0x%zX = %p (region=%zuKB)",
                voff, (void*)(uintptr_t)screenPtr, smbi.RegionSize / 1024);

            uint8_t* screen = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(screenPtr));

            // Direct scan of screen object
            for (size_t so = 0; so + 8 <= 65536 && !swapChain; so += 8)
            {
                uint64_t val = 0;
                if (!FrostbiteConsole::safeRead64(screen + so, &val)) continue;
                if (!isDXGICOMObject(val)) continue;
                IUnknown* unk = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(val));
                IDXGISwapChain* sc = nullptr;
                if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                    reinterpret_cast<void**>(&sc))) && sc)
                {
                    swapChain = sc;
                    sc->Release();
                    overlayLogFmt("resolveD3DFromDXGI: Pass4 SwapChain at dr+0x%zX->+0x%zX = %p",
                        voff, so, (void*)sc);
                }
            }

            // One level deep
            if (!swapChain)
            {
                uintptr_t screenBase = static_cast<uintptr_t>(screenPtr);
                for (size_t so = 0; so + 8 <= 65536 && !swapChain; so += 8)
                {
                    uint64_t subPtr = 0;
                    if (!FrostbiteConsole::safeRead64(screen + so, &subPtr)) continue;
                    if (subPtr < 0x10000ULL || subPtr > 0x00007FFFFFFFFFFFllu) continue;
                    if (subPtr >= screenBase && subPtr < screenBase + 512 * 1024) continue;

                    MEMORY_BASIC_INFORMATION sub2mbi = {};
                    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(subPtr)),
                        &sub2mbi, sizeof(sub2mbi))) continue;
                    if (sub2mbi.State != MEM_COMMIT) continue;
                    if (sub2mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;
                    if (sub2mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) continue;
                    if (sub2mbi.RegionSize < 8 * 1024 ||
                        sub2mbi.RegionSize > 4 * 1024 * 1024) continue;

                    uint8_t* sub2 = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(subPtr));
                    for (size_t so2 = 0; so2 + 8 <= 32768 && !swapChain; so2 += 8)
                    {
                        uint64_t val = 0;
                        if (!FrostbiteConsole::safeRead64(sub2 + so2, &val)) continue;
                        if (!isDXGICOMObject(val)) continue;
                        IUnknown* unk = reinterpret_cast<IUnknown*>(
                            static_cast<uintptr_t>(val));
                        IDXGISwapChain* sc = nullptr;
                        if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                            reinterpret_cast<void**>(&sc))) && sc)
                        {
                            swapChain = sc;
                            sc->Release();
                            overlayLogFmt(
                                "resolveD3DFromDXGI: Pass4 SwapChain at dr+0x%zX->+0x%zX->+0x%zX = %p",
                                voff, so, so2, (void*)sc);
                        }
                    }
                }
            }
        }
        if (!swapChain)
            overlayLog("resolveD3DFromDXGI: Pass4 exhausted");
    }

    // SwapChain not found by direct scan. Use IDXGIDevice->GetAdapter->GetParent
        // to reach the factory, then scan the factory's vtable for CreateSwapChain's
        // internal list. Simpler: scan ALL heap allocations reachable from the
        // IDXGIAdapter for a DXGI swap chain COM object.
        // Most reliable: scan the game module's .data section for any pointer that
        // QI's successfully as IDXGISwapChain using the device we already have.
    if (!swapChain && device)
    {
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory1* fact = nullptr;

        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
            reinterpret_cast<void**>(&dxgiDev))) && dxgiDev)
        {
            dxgiDev->GetAdapter(&adapter);
            dxgiDev->Release();
        }
        if (adapter)
        {
            adapter->GetParent(__uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(&fact));
            adapter->Release();
        }

        // Strategy 1: scan the DxRenderer object in 64KB chunks beyond the
        // first 4KB, up to 256KB total.
        overlayLog("resolveD3DFromDXGI: scanning dr[4KB..256KB] for SwapChain");
        for (size_t off = 4096; off + 8 <= 256 * 1024 && !swapChain; off += 8)
        {
            uint64_t val = 0;
            if (!FrostbiteConsole::safeRead64(dr + off, &val) || !val) continue;
            if (val < 0x10000ULL) continue;
            // Must not be a game module address
            if (val >= reinterpret_cast<uintptr_t>(modBase) &&
                val < reinterpret_cast<uintptr_t>(modEnd)) continue;
            uint64_t vtbl = 0;
            if (!FrostbiteConsole::safeRead64(
                reinterpret_cast<void*>(static_cast<uintptr_t>(val)), &vtbl) || !vtbl) continue;
            HMODULE hMod = nullptr;
            if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(vtbl)), &hMod) || !hMod) continue;
            if (hMod != hDXGI) continue;
            IUnknown* unk = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(val));
            IDXGISwapChain* sc = nullptr;
            if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                reinterpret_cast<void**>(&sc))) && sc)
            {
                swapChain = sc;
                sc->Release();
                overlayLogFmt("resolveD3DFromDXGI: SwapChain at dr+0x%zX = %p", off, (void*)sc);
            }
        }

        // Strategy 2: scan the game module's non-executable data pages for any
        // pointer that QI's as IDXGISwapChain. This catches the case where the
        // swap chain pointer is stored in a global or a separate manager object
        // not reachable from the DxRenderer struct directly.
        if (!swapChain)
        {
            overlayLog("resolveD3DFromDXGI: scanning game .data for SwapChain pointer");
            MEMORY_BASIC_INFORMATION mbi = {};
            for (uint8_t* cursor = modBase; cursor < modEnd && !swapChain; )
            {
                if (!VirtualQuery(cursor, &mbi, sizeof(mbi))) break;
                uint8_t* rBase = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                uint8_t* rEnd = rBase + mbi.RegionSize;
                if (rBase < modBase) rBase = modBase;
                if (rEnd > modEnd)   rEnd = modEnd;

                bool isData = (mbi.State == MEM_COMMIT) &&
                    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));

                if (isData && rEnd > rBase)
                {
                    for (uint8_t* p = rBase; p + 8 <= rEnd && !swapChain; p += 8)
                    {
                        uint64_t val = 0;
                        if (!FrostbiteConsole::safeRead64(p, &val) || !val) continue;
                        if (val < 0x10000ULL) continue;
                        if (val >= reinterpret_cast<uintptr_t>(modBase) &&
                            val < reinterpret_cast<uintptr_t>(modEnd)) continue;
                        uint64_t vtbl = 0;
                        if (!FrostbiteConsole::safeRead64(
                            reinterpret_cast<void*>(static_cast<uintptr_t>(val)),
                            &vtbl) || !vtbl) continue;
                        HMODULE hMod = nullptr;
                        if (!GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(vtbl)),
                            &hMod) || !hMod) continue;
                        if (hMod != hDXGI) continue;
                        IUnknown* unk = reinterpret_cast<IUnknown*>(
                            static_cast<uintptr_t>(val));
                        IDXGISwapChain* sc = nullptr;
                        if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGISwapChain),
                            reinterpret_cast<void**>(&sc))) && sc)
                        {
                            swapChain = sc;
                            sc->Release();
                            overlayLogFmt("resolveD3DFromDXGI: SwapChain in .data at %p = %p",
                                (void*)p, (void*)sc);
                        }
                    }
                }

                uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= reinterpret_cast<uintptr_t>(cursor)) break;
                cursor = reinterpret_cast<uint8_t*>(next);
            }
        }

        if (fact) fact->Release();
    }

    overlayLogFmt("resolveD3DFromDXGI: final sc=%p dev=%p ctx=%p",
        (void*)swapChain, (void*)device, (void*)context);

    if (!swapChain || !device || !context)
    {
        overlayLogFmt("resolveD3DFromDXGI: incomplete — sc=%p dev=%p ctx=%p",
            (void*)swapChain, (void*)device, (void*)context);
        return false;
    }

    g_resolvedSwapChain = swapChain;
    g_resolvedDevice = device;
    g_resolvedContext = context;
    return true;
}

static HWND resolveHwnd()
{
    if (!g_hwndStaticAddr) return nullptr;
    return *g_hwndStaticAddr;
}

// ─────────────────────────────────────────────────────────────────────────────
// IngameConsole detection
//
// Mirrors ConsoleBridge::probeIngameConsole from FrostbiteConsole.cpp.
// Scans the main module's non-executable committed pages for the byte pattern:
//   3E 20 00          "> \0"
//   [0-4 pad bytes]
//   2E 2E 2E 00       "...\0"
//
// Present  → real IngameConsoleImpl compiled in → we skip our overlay.
// Absent   → dummy stub (game shipped without FB_RETAIL_DEBUG_RENDERER) →
//            we inject our own overlay.
// ─────────────────────────────────────────────────────────────────────────────
static bool s_probeResult = false;
static bool s_probeComplete = false;

namespace ConsoleOverlay {
    PipeHandlerFn g_pipeOutputHandler = nullptr;
}

static bool doProbeIngameConsole()
{
    overlayLog("probeIngameConsole: scanning for \"> \\0...\\0\" signature");

    HMODULE mods[1] = {};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) || !mods[0])
    {
        overlayLog("probeIngameConsole: EnumProcessModules failed");
        return false;
    }

    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), mods[0], &mi, sizeof(mi)))
    {
        overlayLog("probeIngameConsole: GetModuleInformation failed");
        return false;
    }

    uint8_t* modBase = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    uint8_t* modEnd = modBase + mi.SizeOfImage;

    overlayLogFmt("probeIngameConsole: module base=%p size=%zu",
        (void*)modBase, (size_t)mi.SizeOfImage);

    MEMORY_BASIC_INFORMATION mbi = {};
    for (uint8_t* cursor = modBase; cursor < modEnd; )
    {
        if (!VirtualQuery(cursor, &mbi, sizeof(mbi))) break;

        uint8_t* rBase = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
        uint8_t* rEnd = rBase + mbi.RegionSize;
        if (rBase < modBase) rBase = modBase;
        if (rEnd > modEnd)  rEnd = modEnd;

        // Only non-executable committed readable pages
        bool readable = (mbi.State == MEM_COMMIT) &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));

        if (readable && rEnd > rBase)
        {
            for (uint8_t* p = rBase; p + 8 <= rEnd; ++p)
            {
                // "> \0"
                if (p[0] != 0x3E || p[1] != 0x20 || p[2] != 0x00) continue;

                // "...\0" within the next 1-4 bytes after the null
                for (int slack = 0; slack <= 4 && p + 3 + slack + 4 <= rEnd; ++slack)
                {
                    if (p[3 + slack] == 0x2E &&
                        p[4 + slack] == 0x2E &&
                        p[5 + slack] == 0x2E &&
                        p[6 + slack] == 0x00)
                    {
                        overlayLogFmt("probeIngameConsole: signature found at %p"
                            " — real IngameConsoleImpl present, overlay NOT needed",
                            (void*)p);
                        return true;
                    }
                }
            }
        }

        uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= reinterpret_cast<uintptr_t>(cursor)) break;
        cursor = reinterpret_cast<uint8_t*>(next);
    }

    overlayLog("probeIngameConsole: signature NOT found — dummy stub, overlay needed");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Vertex / shader
// ─────────────────────────────────────────────────────────────────────────────
struct Vertex
{
    float x, y;          // NDC
    float u, v;          // texture UVs (0,0 = flat color quad)
    float r, g, b, a;    // color
};

static const char* k_shaderSrc = R"hlsl(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = float4(v.pos, 0.0, 1.0);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

Texture2D    g_tex  : register(t0);
SamplerState g_samp : register(s0);

float4 PSMain(VSOut v) : SV_TARGET
{
    // uv == (0,0) means flat color — skip texture sample
    float isTex  = step(0.001, v.uv.x + v.uv.y);
    float alpha  = lerp(v.col.a, g_tex.Sample(g_samp, v.uv).r * v.col.a, isTex);
    return float4(v.col.rgb, alpha);
}
)hlsl";

// ─────────────────────────────────────────────────────────────────────────────
// Font atlas constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int k_firstChar = 32;
static constexpr int k_lastChar = 126;
static constexpr int k_numChars = k_lastChar - k_firstChar + 1;
static constexpr int k_cellW = 10;
static constexpr int k_cellH = 16;
static constexpr int k_atlasW = k_numChars * k_cellW;
static constexpr int k_atlasH = k_cellH;

// ─────────────────────────────────────────────────────────────────────────────
// Console layout constants (mirrors IngameConsoleImpl::Constants)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int   k_responseCount = 25;
static constexpr int   k_lineSpacingPx = 16;
static constexpr float k_marginLeft = 30.0f;
static constexpr float k_marginTop = 48.0f;
static constexpr float k_consoleWidth = 0.0f;  // 0 = use percentage instead
static constexpr float k_consoleWidthPct = 0.70f;  // 70% of screen width
static constexpr float k_bgAlpha = 0.82f;
static constexpr int   k_maxVerts = 16384;

// ─────────────────────────────────────────────────────────────────────────────
// Module-level D3D + overlay state (all in anonymous namespace)
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    // D3D objects
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_ctx = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_rtv = nullptr;
    ID3D11VertexShader* g_vs = nullptr;
    ID3D11PixelShader* g_ps = nullptr;
    ID3D11InputLayout* g_layout = nullptr;
    ID3D11Buffer* g_vb = nullptr;
    ID3D11BlendState* g_blendState = nullptr;
    ID3D11SamplerState* g_sampler = nullptr;
    ID3D11ShaderResourceView* g_fontSRV = nullptr;
    ID3D11RasterizerState* g_rasterState = nullptr;

    // Present hook
    using tPresent = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    tPresent  g_origPresent = nullptr;

    // WndProc
    using tWndProc = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
    tWndProc  g_origWndProc = nullptr;
    HWND      g_hwnd = nullptr;

    // Screen dimensions
    UINT g_screenW = 1920;
    UINT g_screenH = 1080;

    // Console state
    bool                    g_visible = true;   // open on inject for first test
    bool                    g_initialized = false;
    std::mutex              g_mtx;
    std::deque<std::string> g_responses;
    std::string             g_inputLine;
    std::deque<std::string> g_history;
    int                     g_historyIdx = -1;
    static bool g_overlayReady = false; // set to true after first command is run

    // Autocomplete
    std::vector<std::string> g_suggestions;
    std::vector<std::string> g_suggestionDescs; // parallel: description for each suggestion entry
    int                      g_suggestionIdx = -1;      // -1 = none selected
    int                      g_suggestionScrollOffset = 0; // top visible suggestion index
    std::string              g_suggestionDraft;            // typed text before arrow navigation
    static constexpr int     k_maxSuggestions = 1600;       // effectively unlimited
    static constexpr int     k_maxVisibleSuggestions = 8;  // rows shown at once

    // Cursor blink
    DWORD g_lastBlink = 0;
    bool  g_cursorVis = true;

    // Response line color tags (stored as first byte of the string)
    static constexpr char kColorWhite = '\x03';
    static constexpr char kColorGreen = '\x01';
    static constexpr char kColorRed = '\x02';
    static constexpr char kColorGold = '\x04';   // echo lines — matches "> " prompt yellow

    // Scroll state
    int   g_scrollOffset = 0;
    bool  g_scrollDirty = false;

    // Input selection state
    int   g_selStart = -1;   // -1 = no selection
    int   g_selEnd = -1;   // exclusive end index into g_inputLine

    // Per-frame vertex accumulator
    std::vector<Vertex> g_verts;

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Autocomplete helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> g_methodNames; // populated once on first use
static std::vector<std::string> g_methodDescs; // parallel: description string for each entry

static void ensureMethodNames()
{
    if (!g_methodNames.empty()) return;
    int count = 0;
    const FrostbiteConsole::ConsoleMethod* const* methods =
        FrostbiteConsole::getMethods(count);
    if (!methods) return;
    for (int i = 0; i < count; ++i)
    {
        if (!methods[i]) continue;
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(methods[i]);

        uint64_t namePtr = 0, groupPtr = 0;
        if (!FrostbiteConsole::safeRead64(
            const_cast<uint8_t*>(raw + 0x08), &namePtr)) continue;
        if (namePtr < 0x10000ULL) continue;

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(namePtr)),
            &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;

        const char* name = reinterpret_cast<const char*>(
            static_cast<uintptr_t>(namePtr));
        if (!name || name[0] < 'A') continue;

        FrostbiteConsole::safeRead64(
            const_cast<uint8_t*>(raw + 0x10), &groupPtr);

        std::string entry;
        if (groupPtr >= 0x10000ULL)
        {
            MEMORY_BASIC_INFORMATION gmbi{};
            if (VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(groupPtr)),
                &gmbi, sizeof(gmbi)) &&
                gmbi.State == MEM_COMMIT &&
                !(gmbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                const char* grp = reinterpret_cast<const char*>(
                    static_cast<uintptr_t>(groupPtr));
                uint8_t gc0 = static_cast<uint8_t>(grp[0]);
                if (isalpha(gc0) || gc0 == '_')
                {
                    entry = grp;
                    entry += '.';
                }
            }
        }
        entry += name;

        // Read description at +0x18 (may be null for array/list fields)
        std::string desc;
        uint64_t descPtr = 0;
        FrostbiteConsole::safeRead64(
            const_cast<uint8_t*>(raw + 0x18), &descPtr);
        if (descPtr >= 0x10000ULL)
        {
            MEMORY_BASIC_INFORMATION dmbi{};
            if (VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(descPtr)),
                &dmbi, sizeof(dmbi)) &&
                dmbi.State == MEM_COMMIT &&
                !(dmbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                const char* d = reinterpret_cast<const char*>(
                    static_cast<uintptr_t>(descPtr));
                bool ok = true;
                int dlen = 0;
                for (; dlen < 128 && d[dlen]; ++dlen)
                {
                    uint8_t dc = static_cast<uint8_t>(d[dlen]);
                    if (dc < 0x20 || dc > 0x7E) { ok = false; break; }
                }
                if (ok && dlen > 0)
                    desc.assign(d, dlen);
                else
                    desc = "Array";
            }
        }

        g_methodNames.push_back(std::move(entry));
        g_methodDescs.push_back(std::move(desc));
    }
}

static void rebuildSuggestions(const std::string& input)
{
    g_suggestions.clear();
    g_suggestionDescs.clear();
    g_suggestionIdx = -1;
    g_suggestionScrollOffset = 0;
    if (input.empty()) return;

    ensureMethodNames();

    std::string lowerInput = input;
    for (char& c : lowerInput)
        c = (char)tolower((unsigned char)c);

    for (int i = 0; i < (int)g_methodNames.size(); ++i)
    {
        const auto& name = g_methodNames[i];
        if (name.size() < lowerInput.size()) continue;

        std::string lowerName = name;
        for (char& c : lowerName)
            c = (char)tolower((unsigned char)c);

        if (lowerName.find(lowerInput) != std::string::npos)
        {
            g_suggestions.push_back(name);
            g_suggestionDescs.push_back(
                i < (int)g_methodDescs.size() ? g_methodDescs[i] : std::string());
        }
    }
}

// DirectInput hook globals (file scope)
using tGetAsyncKeyState = SHORT(WINAPI*)(int vKey);
static tGetAsyncKeyState g_origGetAsyncKeyState = nullptr;

static void initOrigGetAsyncKeyState()
{
    if (g_origGetAsyncKeyState) return;
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
        g_origGetAsyncKeyState = reinterpret_cast<tGetAsyncKeyState>(
            GetProcAddress(hUser32, "GetAsyncKeyState"));
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D state backup/restore — prevents corrupting the game's renderer state
// ─────────────────────────────────────────────────────────────────────────────
struct D3DStateBackup
{
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT          vp = {};
    UINT                    vpCount = 1;
    ID3D11BlendState* blendState = nullptr;
    FLOAT                   blendFactor[4] = {};
    UINT                    sampleMask = 0;
    ID3D11RasterizerState* rasterState = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    UINT                    vbStride = 0;
    UINT                    vbOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* samp = nullptr;

    void capture(ID3D11DeviceContext* ctx)
    {
        vpCount = 1;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->RSGetViewports(&vpCount, &vp);
        ctx->OMGetBlendState(&blendState, blendFactor, &sampleMask);
        ctx->RSGetState(&rasterState);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->IAGetInputLayout(&layout);
        ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->PSGetShaderResources(0, 1, &srv);
        ctx->PSGetSamplers(0, 1, &samp);
    }

    void restore(ID3D11DeviceContext* ctx)
    {
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        if (vpCount) ctx->RSSetViewports(vpCount, &vp);
        ctx->OMSetBlendState(blendState, blendFactor, sampleMask);
        ctx->RSSetState(rasterState);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->IASetInputLayout(layout);
        ctx->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
        ctx->IASetPrimitiveTopology(topology);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &samp);

        auto safeRelease = [](IUnknown* p) { if (p) p->Release(); };
        safeRelease(rtv); safeRelease(dsv); safeRelease(blendState);
        safeRelease(rasterState); safeRelease(vs); safeRelease(ps);
        safeRelease(layout); safeRelease(vb); safeRelease(srv); safeRelease(samp);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RTV helpers
// ─────────────────────────────────────────────────────────────────────────────

// ── D3D12 overlay resource creation ──────────────────────────────────────────
// Called once from presentHook on the first frame. Creates the command
// allocator, command list, root signature, PSO, vertex buffer, font texture,
// and one RTV per swap-chain back buffer — all on the game's D3D12 device.
// The game's command queue is used to execute our command list each frame.
static bool createD3D12OverlayResources(IDXGISwapChain* sc)
{
    if (g_d3d12ResourcesReady) return true;
    if (!g_d3d12Device || !g_d3d12Queue) return false;

    overlayLog("createD3D12OverlayResources: start");

    // ── Command allocator + list ──────────────────────────────────────────────
    if (FAILED(g_d3d12Device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator),
        reinterpret_cast<void**>(&g_d3d12CmdAlloc))))
    {
        overlayLog("createD3D12OverlayResources: CreateCommandAllocator failed");
        return false;
    }
    if (FAILED(g_d3d12Device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_d3d12CmdAlloc, nullptr,
        __uuidof(ID3D12GraphicsCommandList),
        reinterpret_cast<void**>(&g_d3d12CmdList))))
    {
        overlayLog("createD3D12OverlayResources: CreateCommandList failed");
        return false;
    }
    g_d3d12CmdList->Close();

    // ── RTV descriptor heap ───────────────────────────────────────────────────
    D3D12_DESCRIPTOR_HEAP_DESC rtvHD = {};
    rtvHD.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHD.NumDescriptors = k_maxSwapBuffers;
    rtvHD.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_d3d12Device->CreateDescriptorHeap(&rtvHD,
        __uuidof(ID3D12DescriptorHeap),
        reinterpret_cast<void**>(&g_d3d12RTVHeap))))
    {
        overlayLog("createD3D12OverlayResources: RTV heap failed");
        return false;
    }
    g_d3d12RTVDescSize = g_d3d12Device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // ── SRV heap for font texture ─────────────────────────────────────────────
    D3D12_DESCRIPTOR_HEAP_DESC srvHD = {};
    srvHD.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHD.NumDescriptors = 1;
    srvHD.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_d3d12Device->CreateDescriptorHeap(&srvHD,
        __uuidof(ID3D12DescriptorHeap),
        reinterpret_cast<void**>(&g_d3d12SRVHeap))))
    {
        overlayLog("createD3D12OverlayResources: SRV heap failed");
        return false;
    }

    // ── Back buffers + RTVs ───────────────────────────────────────────────────
    DXGI_SWAP_CHAIN_DESC scd = {};
    sc->GetDesc(&scd);
    g_d3d12BufferCount = scd.BufferCount;
    g_screenW = scd.BufferDesc.Width;
    g_screenH = scd.BufferDesc.Height;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        g_d3d12RTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_d3d12BufferCount && i < k_maxSwapBuffers; ++i)
    {
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&g_d3d12BackBuffers[i]))))
        {
            overlayLogFmt("createD3D12OverlayResources: GetBuffer(%u) failed", i);
            return false;
        }
        g_d3d12Device->CreateRenderTargetView(g_d3d12BackBuffers[i], nullptr, rtvHandle);
        g_d3d12RTVHandles[i] = rtvHandle;
        rtvHandle.ptr += g_d3d12RTVDescSize;
    }

    // ── Root signature: one SRV table + one inline sampler ───────────────────
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER rp = {};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1;
        rp.DescriptorTable.pDescriptorRanges = &srvRange;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.RegisterSpace = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;
        rsd.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* sig = nullptr, * err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
            &sig, &err)) || !sig)
        {
            overlayLogFmt("createD3D12OverlayResources: SerializeRootSig failed: %s",
                err ? (const char*)err->GetBufferPointer() : "");
            if (err) err->Release();
            return false;
        }
        HRESULT hr = g_d3d12Device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            __uuidof(ID3D12RootSignature),
            reinterpret_cast<void**>(&g_d3d12RootSig));
        sig->Release();
        if (err) err->Release();
        if (FAILED(hr)) { overlayLog("createD3D12OverlayResources: CreateRootSignature failed"); return false; }
    }

    // ── Compile shaders and create PSO ────────────────────────────────────────
    // Reuse k_shaderSrc — same HLSL, just compiled for SM5.0 D3D12
    {
        ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * err = nullptr;
        if (FAILED(D3DCompile(k_shaderSrc, strlen(k_shaderSrc), nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vsBlob, &err)) || !vsBlob)
        {
            overlayLogFmt("createD3D12OverlayResources: VS compile failed: %s",
                err ? (const char*)err->GetBufferPointer() : "");
            if (err) err->Release();
            return false;
        }
        if (err) { err->Release(); err = nullptr; }

        if (FAILED(D3DCompile(k_shaderSrc, strlen(k_shaderSrc), nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0", 0, 0, &psBlob, &err)) || !psBlob)
        {
            overlayLogFmt("createD3D12OverlayResources: PS compile failed: %s",
                err ? (const char*)err->GetBufferPointer() : "");
            vsBlob->Release();
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();

        D3D12_INPUT_ELEMENT_DESC elems[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_RENDER_TARGET_BLEND_DESC rtBlend = {};
        rtBlend.BlendEnable = TRUE;
        rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psd = {};
        psd.pRootSignature = g_d3d12RootSig;
        psd.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psd.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psd.InputLayout = { elems, 3 };
        psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psd.NumRenderTargets = 1;
        psd.RTVFormats[0] = scd.BufferDesc.Format;
        psd.SampleDesc.Count = 1;
        psd.BlendState.RenderTarget[0] = rtBlend;
        psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psd.DepthStencilState.DepthEnable = FALSE;
        psd.DepthStencilState.StencilEnable = FALSE;
        psd.SampleMask = UINT_MAX;

        HRESULT hr = g_d3d12Device->CreateGraphicsPipelineState(&psd,
            __uuidof(ID3D12PipelineState),
            reinterpret_cast<void**>(&g_d3d12PSO));
        vsBlob->Release();
        psBlob->Release();
        if (FAILED(hr)) { overlayLog("createD3D12OverlayResources: CreatePSO failed"); return false; }
    }

    // ── Upload-heap vertex buffer (CPU-writable each frame) ───────────────────
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(Vertex) * k_maxVerts;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_d3d12Device->CreateCommittedResource(&hp,
            D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&g_d3d12VB))))
        {
            overlayLog("createD3D12OverlayResources: VB alloc failed");
            return false;
        }
    }

    // ── Font atlas texture (bake via GDI then upload) ─────────────────────────
    {
        // Bake the atlas into a CPU buffer using the same GDI path as D3D11
        HDC hdc = CreateCompatibleDC(nullptr);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = k_atlasW;
        bmi.bmiHeader.biHeight = -k_atlasH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hBmp);
        RECT rc = { 0, 0, k_atlasW, k_atlasH };
        HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, hBlack);
        DeleteObject(hBlack);
        HFONT hFont = CreateFontA(-(k_cellH - 2), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_NATURAL_QUALITY, FIXED_PITCH | FF_MODERN, "Lucida Console");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont ? hFont : (HFONT)GetStockObject(SYSTEM_FIXED_FONT));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        for (int c = k_firstChar; c <= k_lastChar; ++c)
        {
            char ch = (char)c; TextOutA(hdc, (c - k_firstChar) * k_cellW, 1, &ch, 1);
        }
        GdiFlush();
        std::vector<uint8_t> atlas(k_atlasW * k_atlasH);
        uint8_t* src = reinterpret_cast<uint8_t*>(bits);
        for (int i = 0; i < k_atlasW * k_atlasH; ++i)
        {
            // BGRA layout: [0]=B [1]=G [2]=R [3]=A
            uint32_t b = src[i * 4 + 0];
            uint32_t g = src[i * 4 + 1];
            uint32_t r = src[i * 4 + 2];
            // Rec.709 luminance coefficients scaled to integer:
            // Y = (54*R + 183*G + 19*B) / 256  (coefficients sum to 256)
            atlas[i] = static_cast<uint8_t>((54 * r + 183 * g + 19 * b) >> 8);
        }
        SelectObject(hdc, hOldFont); if (hFont) DeleteObject(hFont);
        SelectObject(hdc, hOldBmp); DeleteObject(hBmp); DeleteDC(hdc);

        // Default-heap texture
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = (UINT)k_atlasW;
        td.Height = (UINT)k_atlasH;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(g_d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
            &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&g_d3d12FontTex))))
        {
            overlayLog("createD3D12OverlayResources: font tex alloc failed"); return false;
        }

        // Upload heap
        UINT64 uploadSize = 0;
        g_d3d12Device->GetCopyableFootprints(&td, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
        D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC urd = {};
        urd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        urd.Width = uploadSize; urd.Height = urd.DepthOrArraySize = urd.MipLevels = 1;
        urd.SampleDesc.Count = 1; urd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_d3d12Device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE,
            &urd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&g_d3d12FontUpload))))
        {
            overlayLog("createD3D12OverlayResources: font upload alloc failed"); return false;
        }

        // Copy atlas into upload buffer
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        g_d3d12Device->GetCopyableFootprints(&td, 0, 1, 0, &layout, nullptr, nullptr, nullptr);
        void* mapped = nullptr;
        g_d3d12FontUpload->Map(0, nullptr, &mapped);
        uint8_t* dst = reinterpret_cast<uint8_t*>(mapped);
        for (int row = 0; row < k_atlasH; ++row)
            memcpy(dst + row * layout.Footprint.RowPitch,
                atlas.data() + row * k_atlasW, k_atlasW);
        g_d3d12FontUpload->Unmap(0, nullptr);

        // Record upload command list
        g_d3d12CmdAlloc->Reset();
        g_d3d12CmdList->Reset(g_d3d12CmdAlloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src2 = {}, dst2 = {};
        src2.pResource = g_d3d12FontUpload;
        src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src2.PlacedFootprint = layout;
        dst2.pResource = g_d3d12FontTex;
        dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst2.SubresourceIndex = 0;
        g_d3d12CmdList->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_d3d12FontTex;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_d3d12CmdList->ResourceBarrier(1, &barrier);
        g_d3d12CmdList->Close();
        ID3D12CommandList* lists[] = { g_d3d12CmdList };
        g_d3d12Queue->ExecuteCommandLists(1, lists);

        // GPU-side fence wait so upload is complete before first draw
        ID3D12Fence* fence = nullptr;
        g_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
        HANDLE evt = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_d3d12Queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 5000);
        CloseHandle(evt);
        fence->Release();

        // SRV for the font texture
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = DXGI_FORMAT_R8_UNORM;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        g_d3d12Device->CreateShaderResourceView(
            g_d3d12FontTex, &srvd,
            g_d3d12SRVHeap->GetCPUDescriptorHandleForHeapStart());
    }

    g_d3d12ResourcesReady = true;
    overlayLog("createD3D12OverlayResources: OK");
    return true;
}

static void createRTV()
{
    if (g_rtv || !g_swapChain || !g_device) return;

    // In pure D3D12 mode the RTVs are managed by createD3D12OverlayResources
    // / presentHook directly on ID3D12Resource back buffers.  The D3D11 RTV
    // path is not used and GetBuffer would return a D3D12 resource that the
    // D3D11 device cannot wrap without D3D11On12.
    if (g_isD3D12Mode)
    {
        overlayLog("createRTV: D3D12 mode — skipping D3D11 RTV creation");
        return;
    }

    if (g_isD3D12Mode && g_11on12Device)
    {
        // In D3D12 mode the back buffer is an ID3D12Resource.
        // We wrap it via ID3D11On12Device so our D3D11 render target view
        // refers to it, then we can use the standard D3D11 render path.
        ID3D12Resource* d12back = nullptr;
        if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&d12back))) || !d12back)
        {
            overlayLog("createRTV: D3D12 GetBuffer failed");
            return;
        }

        D3D12_RESOURCE_DESC d12desc = d12back->GetDesc();
        g_screenW = (UINT)d12desc.Width;
        g_screenH = (UINT)d12desc.Height;

        D3D11_RESOURCE_FLAGS d11flags = {};
        d11flags.BindFlags = D3D11_BIND_RENDER_TARGET;

        ID3D11Resource* wrapped = nullptr;
        HRESULT hr = g_11on12Device->CreateWrappedResource(
            d12back,
            &d11flags,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT,
            __uuidof(ID3D11Resource),
            reinterpret_cast<void**>(&wrapped));

        d12back->Release();

        if (FAILED(hr) || !wrapped)
        {
            overlayLogFmt("createRTV: CreateWrappedResource failed hr=0x%08X", (unsigned)hr);
            return;
        }

        hr = g_device->CreateRenderTargetView(wrapped, nullptr, &g_rtv);
        if (FAILED(hr))
        {
            overlayLogFmt("createRTV: CreateRenderTargetView(wrapped) failed hr=0x%08X", (unsigned)hr);
            wrapped->Release();
            return;
        }

        g_11on12WrappedRT = wrapped; // keep ref for Acquire/Release in presentHook
        overlayLogFmt("createRTV: D3D12 mode %ux%u rtv=%p wrappedRT=%p",
            g_screenW, g_screenH, (void*)g_rtv, (void*)wrapped);
        return;
    }

    // D3D11 path (unchanged)
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
    {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);

        D3D11_TEXTURE2D_DESC desc = {};
        back->GetDesc(&desc);
        g_screenW = desc.Width;
        g_screenH = desc.Height;
        back->Release();

        overlayLogFmt("createRTV: %ux%u rtv=%p", g_screenW, g_screenH, (void*)g_rtv);
    }
    else
    {
        overlayLog("createRTV: GetBuffer failed");
    }
}

static void releaseRTV()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_11on12WrappedRT) { g_11on12WrappedRT->Release(); g_11on12WrappedRT = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
// GDI font atlas bake
// ─────────────────────────────────────────────────────────────────────────────
static bool bakeFontAtlas()
{
    overlayLog("bakeFontAtlas: start");

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) { overlayLog("bakeFontAtlas: CreateCompatibleDC failed"); return false; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = k_atlasW;
    bmi.bmiHeader.biHeight = -k_atlasH; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp) { overlayLog("bakeFontAtlas: CreateDIBSection failed"); DeleteDC(hdc); return false; }

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdc, hBmp);

    // Fill black
    RECT rc = { 0, 0, k_atlasW, k_atlasH };
    HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, hBlack);
    DeleteObject(hBlack);

    // Lucida Console pixel-height font
    HFONT hFont = CreateFontA(-(k_cellH - 2), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, FIXED_PITCH | FF_MODERN, "Lucida Console");

    HFONT hOldFont = (HFONT)SelectObject(hdc,
        hFont ? hFont : (HFONT)GetStockObject(SYSTEM_FIXED_FONT));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    for (int c = k_firstChar; c <= k_lastChar; ++c)
    {
        char ch = (char)c;
        TextOutA(hdc, (c - k_firstChar) * k_cellW, 1, &ch, 1);
    }

    GdiFlush();

    // Extract luminance (perceptual average of RGB) into grayscale R8 buffer.
    // Using Rec.709 luminance instead of a single channel preserves the full
    // antialiasing coverage from ClearType without colour fringing artefacts.
    std::vector<uint8_t> atlas(k_atlasW * k_atlasH);
    uint8_t* src = reinterpret_cast<uint8_t*>(bits);
    for (int i = 0; i < k_atlasW * k_atlasH; ++i)
    {
        // BGRA layout: [0]=B [1]=G [2]=R [3]=A
        uint32_t b = src[i * 4 + 0];
        uint32_t g = src[i * 4 + 1];
        uint32_t r = src[i * 4 + 2];
        // Rec.709 luminance coefficients scaled to integer:
        // Y = (54*R + 183*G + 19*B) / 256  (coefficients sum to 256)
        atlas[i] = static_cast<uint8_t>((54 * r + 183 * g + 19 * b) >> 8);
    }

    SelectObject(hdc, hOldFont);
    if (hFont) DeleteObject(hFont);
    SelectObject(hdc, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdc);

    // Upload as R8_UNORM
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)k_atlasW;
    td.Height = (UINT)k_atlasH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = atlas.data();
    sd.SysMemPitch = (UINT)k_atlasW;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_device->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr))
    {
        overlayLogFmt("bakeFontAtlas: CreateTexture2D failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    hr = g_device->CreateShaderResourceView(tex, nullptr, &g_fontSRV);
    tex->Release();
    if (FAILED(hr))
    {
        overlayLogFmt("bakeFontAtlas: CreateSRV failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    overlayLog("bakeFontAtlas: OK");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D object creation/destruction
// ─────────────────────────────────────────────────────────────────────────────
static bool createD3DObjects()
{
    overlayLog("createD3DObjects: compiling shaders");

    ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * err = nullptr;

    HRESULT hr = D3DCompile(k_shaderSrc, strlen(k_shaderSrc),
        nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &err);
    if (FAILED(hr))
    {
        overlayLogFmt("createD3DObjects: VS compile hr=0x%08X | %s",
            (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }

    hr = D3DCompile(k_shaderSrc, strlen(k_shaderSrc),
        nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &err);
    if (FAILED(hr))
    {
        overlayLogFmt("createD3DObjects: PS compile hr=0x%08X | %s",
            (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "");
        vsBlob->Release();
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }

    hr = g_device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    if (FAILED(hr))
    {
        overlayLogFmt("CreateVertexShader failed 0x%08X", (unsigned)hr);
        vsBlob->Release(); psBlob->Release();
        return false;
    }

    hr = g_device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    if (FAILED(hr))
    {
        overlayLogFmt("CreatePixelShader failed 0x%08X", (unsigned)hr);
        vsBlob->Release(); psBlob->Release();
        return false;
    }

    // Input layout: POSITION(float2) TEXCOORD(float2) COLOR(float4) = 32 bytes
    D3D11_INPUT_ELEMENT_DESC elems[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_device->CreateInputLayout(elems, 3,
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_layout);
    vsBlob->Release(); psBlob->Release();
    if (FAILED(hr)) { overlayLogFmt("CreateInputLayout failed 0x%08X", (unsigned)hr); return false; }

    // Dynamic vertex buffer
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(sizeof(Vertex) * k_maxVerts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_device->CreateBuffer(&bd, nullptr, &g_vb);
    if (FAILED(hr)) { overlayLogFmt("CreateBuffer(vb) failed 0x%08X", (unsigned)hr); return false; }

    // Alpha blend
    D3D11_BLEND_DESC blend = {};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_device->CreateBlendState(&blend, &g_blendState);
    if (FAILED(hr)) { overlayLogFmt("CreateBlendState failed 0x%08X", (unsigned)hr); return false; }

    // Point sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_device->CreateSamplerState(&sd, &g_sampler);
    if (FAILED(hr)) { overlayLogFmt("CreateSamplerState failed 0x%08X", (unsigned)hr); return false; }

    // No-cull rasterizer
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    hr = g_device->CreateRasterizerState(&rd, &g_rasterState);
    if (FAILED(hr)) { overlayLogFmt("CreateRasterizerState failed 0x%08X", (unsigned)hr); return false; }

    overlayLog("createD3DObjects: OK");
    return true;
}

static void destroyD3DObjects()
{
    auto R = [](IUnknown*& p) { if (p) { p->Release(); p = nullptr; } };
    R(reinterpret_cast<IUnknown*&>(g_rasterState));
    R(reinterpret_cast<IUnknown*&>(g_sampler));
    R(reinterpret_cast<IUnknown*&>(g_blendState));
    R(reinterpret_cast<IUnknown*&>(g_vb));
    R(reinterpret_cast<IUnknown*&>(g_layout));
    R(reinterpret_cast<IUnknown*&>(g_ps));
    R(reinterpret_cast<IUnknown*&>(g_vs));
    R(reinterpret_cast<IUnknown*&>(g_fontSRV));
    releaseRTV();

    // D3D12 mode cleanup
    if (g_d3d12CmdList) { g_d3d12CmdList->Release();   g_d3d12CmdList = nullptr; }
    if (g_d3d12CmdAlloc) { g_d3d12CmdAlloc->Release();  g_d3d12CmdAlloc = nullptr; }
    if (g_d3d12PSO) { g_d3d12PSO->Release();       g_d3d12PSO = nullptr; }
    if (g_d3d12RootSig) { g_d3d12RootSig->Release();   g_d3d12RootSig = nullptr; }
    if (g_d3d12VB) { g_d3d12VB->Release();        g_d3d12VB = nullptr; }
    if (g_d3d12FontUpload) { g_d3d12FontUpload->Release(); g_d3d12FontUpload = nullptr; }
    if (g_d3d12FontTex) { g_d3d12FontTex->Release();   g_d3d12FontTex = nullptr; }
    if (g_d3d12SRVHeap) { g_d3d12SRVHeap->Release();   g_d3d12SRVHeap = nullptr; }
    if (g_d3d12RTVHeap) { g_d3d12RTVHeap->Release();   g_d3d12RTVHeap = nullptr; }
    for (UINT i = 0; i < k_maxSwapBuffers; ++i)
        if (g_d3d12BackBuffers[i]) { g_d3d12BackBuffers[i]->Release(); g_d3d12BackBuffers[i] = nullptr; }
    if (g_11on12Device) { g_11on12Device->Release(); g_11on12Device = nullptr; }
    if (g_d3d12Queue) { g_d3d12Queue->Release();   g_d3d12Queue = nullptr; }
    if (g_d3d12Device) { g_d3d12Device->Release();  g_d3d12Device = nullptr; }
    g_d3d12ResourcesReady = false;
    g_isD3D12Mode = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry helpers
// ─────────────────────────────────────────────────────────────────────────────
static float ndcX(float px) { return  (px / (float)g_screenW) * 2.0f - 1.0f; }
static float ndcY(float py) { return -(py / (float)g_screenH) * 2.0f + 1.0f; }

static void pushQuad(float x0, float y0, float x1, float y1,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float a)
{
    auto v = [&](float x, float y, float u, float vv) {
        g_verts.push_back({ ndcX(x), ndcY(y), u, vv, r, g, b, a });
        };
    v(x0, y0, u0, v0); v(x1, y0, u1, v0); v(x1, y1, u1, v1);
    v(x0, y0, u0, v0); v(x1, y1, u1, v1); v(x0, y1, u0, v1);
}

// Flat colored quad — u,v = 0,0 so the shader skips texture sample
static void drawRect(float x0, float y0, float x1, float y1,
    float r, float g, float b, float a)
{
    pushQuad(x0, y0, x1, y1, 0.f, 0.f, 0.f, 0.f, r, g, b, a);
}

// Single character glyph
static float drawChar(float px, float py, char c,
    float r, float g, float b, float a)
{
    int idx = (int)(unsigned char)c - k_firstChar;
    if (idx < 0 || idx >= k_numChars) return px + k_cellW;
    float u0 = (float)(idx * k_cellW) / (float)k_atlasW;
    float u1 = (float)(idx * k_cellW + k_cellW) / (float)k_atlasW;
    pushQuad(px, py, px + k_cellW, py + k_cellH, u0, 0.f, u1, 1.f, r, g, b, a);
    return px + k_cellW;
}

// Returns x after the last character.
// If maxX > 0 the string is clipped to that pixel boundary — if it would
// overflow, characters are drawn up to the limit and "..." is appended.
static float drawString(float px, float py, const char* str,
    float r, float g, float b, float a,
    float maxX = 0.f)
{
    if (!str) return px;

    // No clipping requested — draw normally
    if (maxX <= 0.f)
    {
        while (*str)
        {
            char c = *str++;
            if (c == '\n' || c == '\r') continue;
            if (c == ' ') { px += k_cellW; continue; }
            if ((unsigned char)c >= (unsigned char)k_firstChar &&
                (unsigned char)c <= (unsigned char)k_lastChar)
                px = drawChar(px, py, c, r, g, b, a);
            else
                px += k_cellW;
        }
        return px;
    }

    // Clipping path — reserve room for "..." (3 chars) at the right edge
    const float ellipsisW = 3.f * k_cellW;
    const float clipLimit = maxX - ellipsisW;

    bool clipped = false;
    const char* p = str;
    while (*p)
    {
        char c = *p++;
        if (c == '\n' || c == '\r') continue;
        float advance = k_cellW;
        if (px + advance > clipLimit)
        {
            clipped = true;
            break;
        }
        if (c == ' ') { px += k_cellW; continue; }
        if ((unsigned char)c >= (unsigned char)k_firstChar &&
            (unsigned char)c <= (unsigned char)k_lastChar)
            px = drawChar(px, py, c, r, g, b, a);
        else
            px += k_cellW;
    }

    if (clipped)
    {
        // Draw "..."
        for (int i = 0; i < 3; ++i)
            px = drawChar(px, py, '.', r, g, b, a);
    }

    return px;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main render call — invoked from presentHook
// ─────────────────────────────────────────────────────────────────────────────
static void renderConsole()
{
    // Geometry building requires visibility; the D3D11 upload/draw additionally
    // requires g_rtv and g_ctx.  In D3D12 mode g_ctx is a valid independent
    // D3D11 device context but g_rtv is null — we still need to build g_verts
    // so presentHook can upload them via the D3D12 path.
    if (!g_visible) return;

    const float left = k_marginLeft;
    const float top = k_marginTop;
    const float right = (float)g_screenW * k_consoleWidthPct;

    std::lock_guard<std::mutex> lock(g_mtx);

    // Panel grows with content up to k_responseCount lines, then locks
    int displayedCount = (int)g_responses.size();
    if (displayedCount > k_responseCount) displayedCount = k_responseCount;
    float bottom = top + (float)((displayedCount + 2) * k_lineSpacingPx) + 10.f;
    if (bottom > (float)g_screenH - 8.f) bottom = (float)g_screenH - 8.f;

    // Header bar (white background)
    drawRect(left, 30.f, left + 160.f, top, 1.f, 1.f, 1.f, 1.f);
    // Draw "Console" bold-simulated: two passes offset by 1px horizontally
    drawString(left + 8.f, 32.f, "Console", 0.f, 0.f, 0.f, 1.f);
    drawString(left + 8.f + 1.f, 32.f, "Console", 0.f, 0.f, 0.f, 1.f);

    // Body background (dark semi-transparent)
    drawRect(left, top, right, bottom, 0.08f, 0.08f, 0.08f, k_bgAlpha);

    // Thin border line along the top of the body
    drawRect(left, top, right, top + 1.f, 0.5f, 0.5f, 0.5f, 1.f);

    // Response lines — fixed window of k_responseCount lines, scrollable
    const int maxVisible = k_responseCount;
    int total = (int)g_responses.size();

    // Clamp scroll offset: can't scroll past the oldest entry
    int maxScroll = total - maxVisible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scrollOffset > maxScroll) g_scrollOffset = maxScroll;
    if (g_scrollOffset < 0)         g_scrollOffset = 0;

    // offset=0  → live tail (newest k_responseCount lines)
    // offset=N  → window shifted N lines toward older entries
    int windowEnd = total - g_scrollOffset;
    if (windowEnd > total) windowEnd = total;
    if (windowEnd < 0)     windowEnd = 0;
    int windowStart = windowEnd - maxVisible;
    if (windowStart < 0)   windowStart = 0;

    float y = top + 4.f;
    for (int li = windowStart; li < windowEnd; ++li)
    {
        const std::string& entry = g_responses[li];
        if (entry.empty()) { y += k_lineSpacingPx; continue; }

        float r = 1.f, g = 1.f, b = 1.f;
        const char* text = entry.c_str();

        if (entry[0] == kColorGreen) { r = 0.3f; g = 1.f;  b = 0.3f; text++; }
        else if (entry[0] == kColorRed) { r = 1.f;  g = 0.3f; b = 0.3f; text++; }
        else if (entry[0] == kColorGold) { r = 1.f;  g = 1.f;  b = 0.0f; text++; }
        else if (entry[0] == kColorWhite) { text++; }

        drawString(left + 8.f, y, text, r, g, b, 1.f, right - 8.f);
        y += k_lineSpacingPx;
    }

    // Scroll indicator — only shown when not at live tail
    if (g_scrollOffset > 0)
    {
        int hiddenBelow = total - windowEnd;
        char scrollHint[48];
        if (hiddenBelow > 0)
            _snprintf_s(scrollHint, sizeof(scrollHint), _TRUNCATE,
                "^ %d older  v %d newer", windowStart, hiddenBelow);
        else
            _snprintf_s(scrollHint, sizeof(scrollHint), _TRUNCATE,
                "^ %d older lines", windowStart);

        int hintLen = lstrlenA(scrollHint);
        drawString(right - (float)(hintLen * k_cellW) - 8.f,
            top + 4.f, scrollHint, 1.f, 0.85f, 0.3f, 1.f);
    }

    // Blank gap before prompt
    y += 4.f;

    // Prompt line
    float curX = drawString(left + 8.f, y, "> ", 1.f, 1.f, 0.f, 1.f);

    // Draw input line with optional selection highlight
    if (g_selStart != -1 && !g_inputLine.empty())
    {
        int lo = g_selStart < g_selEnd ? g_selStart : g_selEnd;
        int hi = g_selStart < g_selEnd ? g_selEnd : g_selStart;
        lo = lo < 0 ? 0 : lo;
        hi = hi > (int)g_inputLine.size() ? (int)g_inputLine.size() : hi;

        if (lo > 0)
        {
            std::string before = g_inputLine.substr(0, lo);
            curX = drawString(curX, y, before.c_str(), 1.f, 1.f, 1.f, 1.f, right - 8.f);
        }

        float selX0 = curX;
        float selX1 = curX + (float)(hi - lo) * k_cellW;
        drawRect(selX0, y, selX1, y + k_cellH, 0.2f, 0.5f, 1.f, 0.6f);

        if (hi > lo)
        {
            std::string sel = g_inputLine.substr(lo, hi - lo);
            curX = drawString(curX, y, sel.c_str(), 1.f, 1.f, 1.f, 1.f, right - 8.f);
        }

        if (hi < (int)g_inputLine.size())
        {
            std::string after = g_inputLine.substr(hi);
            curX = drawString(curX, y, after.c_str(), 1.f, 1.f, 1.f, 1.f, right - 8.f);
        }
    }
    else
    {
        curX = drawString(curX, y, g_inputLine.c_str(), 1.f, 1.f, 1.f, 1.f, right - 8.f);
    }

    // Blinking cursor (hidden when there is a full selection)
    DWORD now = GetTickCount();
    if (now - g_lastBlink > 500) { g_cursorVis = !g_cursorVis; g_lastBlink = now; }
    if (g_cursorVis && g_selStart == -1)
        drawRect(curX, y + 2.f, curX + 2.f, y + k_cellH - 2.f, 1.f, 1.f, 1.f, 1.f);

    // ── Suggestion dropdown ───────────────────────────────────────────────────
    if (!g_suggestions.empty())
    {
        const int totalSugg = (int)g_suggestions.size();
        const int visibleRows = std::min(totalSugg, k_maxVisibleSuggestions);
        const float kSBW = 4.f; // scrollbar width

        float sy = bottom;
        float dropBottom = sy + (float)visibleRows * k_lineSpacingPx + 4.f;
        dropBottom = std::min(dropBottom, (float)g_screenH - 4.f);

        // Dropdown background
        drawRect(left, sy, right, dropBottom, 0.08f, 0.08f, 0.08f, 0.95f);
        drawRect(left, sy, right, sy + 1.f, 0.3f, 0.3f, 0.3f, 1.f);

        float ey = sy + 2.f;
        for (int i = 0; i < visibleRows; ++i)
        {
            int idx = g_suggestionScrollOffset + i;
            if (idx >= totalSugg) break;
            if (ey + k_cellH > dropBottom) break;

            bool selected = (idx == g_suggestionIdx);

            // Description string (may be empty)
            const char* descStr = (idx < (int)g_suggestionDescs.size() &&
                !g_suggestionDescs[idx].empty())
                ? g_suggestionDescs[idx].c_str() : nullptr;

            // Reserve space on the right for the description so the command
            // name doesn't collide with it.  descCols is in character cells.
            float descX = 0.f;
            float nameMaxX = right - kSBW - 8.f;
            if (descStr)
            {
                int descLen = (int)strlen(descStr);
                descX = right - kSBW - 8.f - (float)(descLen * k_cellW);
                // Give the name at least half the panel width before clipping
                float minNameMaxX = left + (right - left) * 0.5f;
                if (descX < minNameMaxX) descX = minNameMaxX;
                nameMaxX = descX - (float)k_cellW; // one-cell gap
            }

            if (selected)
            {
                drawRect(left + 2.f, ey - 1.f, right - 2.f - kSBW, ey + k_cellH + 1.f,
                    0.15f, 0.35f, 0.15f, 1.f);
                drawString(left + 8.f, ey, g_suggestions[idx].c_str(),
                    0.4f, 1.f, 0.4f, 1.f, nameMaxX);
                if (descStr)
                    drawString(descX, ey, descStr, 0.4f, 0.8f, 0.4f, 0.75f);
            }
            else
            {
                drawString(left + 8.f, ey, g_suggestions[idx].c_str(),
                    0.75f, 0.75f, 0.75f, 1.f, nameMaxX);
                if (descStr)
                    drawString(descX, ey, descStr, 0.45f, 0.45f, 0.55f, 0.85f);
            }
            ey += k_lineSpacingPx;
        }

        // Scrollbar — only when there are more suggestions than visible rows
        if (totalSugg > k_maxVisibleSuggestions)
        {
            const float kBarX = right - kSBW - 2.f;
            const float kBarTop = sy + 2.f;
            const float kBarBot = dropBottom - 2.f;
            const float kBarH = kBarBot - kBarTop;

            // Track
            drawRect(kBarX, kBarTop, kBarX + kSBW, kBarBot,
                0.3f, 0.3f, 0.3f, 0.5f);

            // Thumb
            float thumbH = kBarH * ((float)k_maxVisibleSuggestions / (float)totalSugg);
            if (thumbH < 6.f) thumbH = 6.f;

            int maxSuggScroll = totalSugg - k_maxVisibleSuggestions;
            float scrollFrac = (maxSuggScroll > 0)
                ? (float)g_suggestionScrollOffset / (float)maxSuggScroll
                : 0.f;

            float thumbY = kBarTop + (kBarH - thumbH) * scrollFrac;
            drawRect(kBarX, thumbY, kBarX + kSBW, thumbY + thumbH,
                0.85f, 0.85f, 0.85f, 0.9f);
        }
    }

    // ── Scrollbar ─────────────────────────────────────────────────────────────
    if (total > maxVisible)
    {
        const float kBarW = 4.f;
        const float kBarX = right - kBarW - 2.f;
        const float kBarTop = top + 2.f;
        const float kBarBot = bottom - 2.f;
        const float kBarH = kBarBot - kBarTop;

        drawRect(kBarX, kBarTop, kBarX + kBarW, kBarBot,
            0.3f, 0.3f, 0.3f, 0.5f);

        float thumbH = kBarH * ((float)maxVisible / (float)total);
        if (thumbH < 6.f) thumbH = 6.f;

        float scrollFrac = (maxScroll > 0)
            ? (float)g_scrollOffset / (float)maxScroll
            : 0.f;

        float thumbY = kBarTop + (kBarH - thumbH) * (1.f - scrollFrac);
        float thumbBrightness = (g_scrollOffset > 0) ? 0.85f : 0.5f;
        drawRect(kBarX, thumbY, kBarX + kBarW, thumbY + thumbH,
            thumbBrightness, thumbBrightness, thumbBrightness, 0.9f);
    }

    // ── Upload and draw (D3D11 path only) ─────────────────────────────────────
    // In D3D12 mode g_rtv is null; presentHook reads g_verts directly and
    // uploads them via the D3D12 command list.  Do not touch g_ctx here.
    if (!g_rtv || !g_ctx) return;

    if (g_verts.empty()) return;

    size_t drawCount = std::min(g_verts.size(), (size_t)k_maxVerts);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        g_verts.clear();
        return;
    }
    memcpy(mapped.pData, g_verts.data(), drawCount * sizeof(Vertex));
    g_ctx->Unmap(g_vb, 0);
    g_verts.clear();

    UINT stride = sizeof(Vertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_fontSRV);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    g_ctx->OMSetBlendState(g_blendState, nullptr, 0xFFFFFFFF);
    g_ctx->RSSetState(g_rasterState);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)g_screenW;
    vp.Height = (float)g_screenH;
    vp.MaxDepth = 1.f;
    g_ctx->RSSetViewports(1, &vp);

    g_ctx->Draw((UINT)drawCount, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// GetRawInputData IAT patch
//
// Confirmed via x64dbg: Frostbite calls user32.GetRawInputData directly from
// the main game module (pvz.main_win64_retail). We patch that IAT slot so our
// hook runs instead. While the overlay is visible we zero out the RAWINPUT
// buffer for keyboard packets, making the game see all keys as released.
// Mouse packets are passed through untouched.
// ─────────────────────────────────────────────────────────────────────────────
using tGetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static tGetRawInputData g_origGetRawInputData = nullptr;
static tGetRawInputData g_realGetRawInputData = nullptr;

static UINT WINAPI hookedGetRawInputData(HRAWINPUT hRawInput,
    UINT      uiCommand,
    LPVOID    pData,
    PUINT     pcbSize,
    UINT      cbSizeHeader)
{
    UINT ret = g_realGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    // While overlay is open, suppress key-down (MAKE) events but let
    // key-up (BREAK) events through so the game can clear its held-key state.
    // This means held keys unstick naturally the moment the physical key is
    // released, rather than looping forever.
    if (g_visible && uiCommand == RID_INPUT && pData && ret != (UINT)-1)
    {
        RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(pData);
        if (ri->header.dwType == RIM_TYPEKEYBOARD)
        {
            bool isBreak = (ri->data.keyboard.Flags & RI_KEY_BREAK) != 0;
            if (!isBreak)
            {
                // Key-down: replace with a no-op break on a dummy key (VK 0xFF
                // is reserved and ignored by all known input handlers).
                memset(&ri->data.keyboard, 0, sizeof(ri->data.keyboard));
                ri->data.keyboard.Flags = RI_KEY_BREAK;
                ri->data.keyboard.VKey = 0xFF;
            }
            // Key-up: pass through untouched so game clears its down-state.
        }
    }

    return ret;
}

static bool installDInputHook()
{
    if (g_origGetRawInputData)
    {
        overlayLog("installDInputHook: already installed");
        return true;
    }

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (!hUser32)
    {
        overlayLog("installDInputHook: user32.dll not loaded");
        return false;
    }

    uint8_t* fn = reinterpret_cast<uint8_t*>(
        GetProcAddress(hUser32, "GetRawInputData"));
    if (!fn)
    {
        overlayLog("installDInputHook: GetProcAddress failed");
        return false;
    }

    // Resolve the real implementation via the win32u.dll forward.
    // user32.GetRawInputData is a thin stub that forwards to
    // win32u.NtUserGetRawInputData — call that directly to bypass our patch.
    HMODULE hWin32u = GetModuleHandleA("win32u.dll");
    if (!hWin32u) hWin32u = LoadLibraryA("win32u.dll");

    g_realGetRawInputData = nullptr;
    if (hWin32u)
    {
        g_realGetRawInputData = reinterpret_cast<tGetRawInputData>(
            GetProcAddress(hWin32u, "NtUserGetRawInputData"));
        overlayLogFmt("installDInputHook: win32u.NtUserGetRawInputData=%p",
            (void*)g_realGetRawInputData);
    }

    // Fallback: if win32u isn't available, store the pre-patch pointer.
    // This is safe because we store it before writing the patch bytes.
    if (!g_realGetRawInputData)
    {
        g_realGetRawInputData = reinterpret_cast<tGetRawInputData>(
            reinterpret_cast<void*>(fn));
        overlayLog("installDInputHook: win32u not found, using pre-patch pointer as fallback");
    }

    g_origGetRawInputData = g_realGetRawInputData;

    // Write FF 25 00000000 <addr> — absolute indirect jmp, 14 bytes
    uint8_t patch[14];
    patch[0] = 0xFF; patch[1] = 0x25;
    *reinterpret_cast<uint32_t*>(&patch[2]) = 0;
    *reinterpret_cast<uintptr_t*>(&patch[6]) =
        reinterpret_cast<uintptr_t>(&hookedGetRawInputData);

    DWORD old = 0;
    VirtualProtect(fn, sizeof(patch), PAGE_EXECUTE_READWRITE, &old);
    memcpy(fn, patch, sizeof(patch));
    VirtualProtect(fn, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof(patch));

    overlayLogFmt("installDInputHook: hotpatched user32.GetRawInputData at %p", (void*)fn);
    return true;
}

static void uninstallDInputHook()
{
    if (!g_origGetRawInputData) return;

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (!hUser32) return;

    uint8_t* fn = reinterpret_cast<uint8_t*>(
        GetProcAddress(hUser32, "GetRawInputData"));
    if (!fn) return;

    // The original 14 bytes are at the start of our trampoline
    uint8_t* trampoline = reinterpret_cast<uint8_t*>(g_origGetRawInputData);
    DWORD old = 0;
    VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old);
    memcpy(fn, trampoline, 14);
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);

    g_origGetRawInputData = nullptr;
    overlayLog("uninstallDInputHook: user32.GetRawInputData restored");
}

// ─────────────────────────────────────────────────────────────────────────────
// VTable hook install/uninstall
// IDXGISwapChain vtable slots:
//   slot  8 = Present
//   slot 13 = ResizeBuffers
// We hook both: Present to render, ResizeBuffers to release/recreate our RTV.
// DXGI requires zero outstanding back-buffer references before ResizeBuffers —
// our RTV holds one, so we must release it before the call and recreate after.
// ─────────────────────────────────────────────────────────────────────────────
static HRESULT __stdcall presentHook(IDXGISwapChain* sc, UINT sync, UINT flags);
static HRESULT __stdcall resizeBuffersHook(IDXGISwapChain* sc,
    UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT newFormat, UINT swapChainFlags);

using tResizeBuffers = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT,
    DXGI_FORMAT, UINT);
static tResizeBuffers g_origResizeBuffers = nullptr;

static bool patchVtableSlot(void** vtable, int slot, void* newFn, void** outOrig)
{
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old))
    {
        overlayLogFmt("patchVtableSlot: VirtualProtect failed slot=%d", slot);
        return false;
    }
    if (outOrig) *outOrig = vtable[slot];
    vtable[slot] = newFn;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
    return true;
}

static bool installPresentHook()
{
    IDXGISwapChain* sc = g_resolvedSwapChain;
    if (!sc) { overlayLog("installPresentHook: null swap chain"); return false; }

    void** vtable = *reinterpret_cast<void***>(sc);
    overlayLogFmt("installPresentHook: vtable=%p slot8=%p slot13=%p",
        (void*)vtable, vtable[8], vtable[13]);

    if (!patchVtableSlot(vtable, 8, reinterpret_cast<void*>(&presentHook),
        reinterpret_cast<void**>(&g_origPresent)))
    {
        overlayLog("installPresentHook: failed to patch Present");
        return false;
    }

    if (!patchVtableSlot(vtable, 13, reinterpret_cast<void*>(&resizeBuffersHook),
        reinterpret_cast<void**>(&g_origResizeBuffers)))
    {
        overlayLog("installPresentHook: failed to patch ResizeBuffers");
        // Still usable without it, just log the warning
    }

    overlayLogFmt("installPresentHook: Present=%p ResizeBuffers=%p",
        (void*)g_origPresent, (void*)g_origResizeBuffers);
    return true;
}

static void uninstallPresentHook()
{
    IDXGISwapChain* sc = g_resolvedSwapChain;
    void** vtable = sc ? *reinterpret_cast<void***>(sc) : nullptr;

    if (vtable)
    {
        DWORD old = 0;
        if (g_origPresent && VirtualProtect(&vtable[8], sizeof(void*), PAGE_READWRITE, &old))
        {
            vtable[8] = reinterpret_cast<void*>(g_origPresent);
            VirtualProtect(&vtable[8], sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(), &vtable[8], sizeof(void*));
        }
        if (g_origResizeBuffers && VirtualProtect(&vtable[13], sizeof(void*), PAGE_READWRITE, &old))
        {
            vtable[13] = reinterpret_cast<void*>(g_origResizeBuffers);
            VirtualProtect(&vtable[13], sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(), &vtable[13], sizeof(void*));
        }
    }

    g_origPresent = nullptr;
    g_origResizeBuffers = nullptr;
    overlayLog("uninstallPresentHook: restored");
}

// ─────────────────────────────────────────────────────────────────────────────
// Present hook implementation
// ─────────────────────────────────────────────────────────────────────────────
static HRESULT __stdcall presentHook(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (g_isD3D12Mode)
    {
        // ── Pure D3D12 overlay path ───────────────────────────────────────────
        // First frame: build all resources
        if (!g_d3d12ResourcesReady)
        {
            if (!createD3D12OverlayResources(sc))
            {
                overlayLog("presentHook: createD3D12OverlayResources failed — D3D12 overlay disabled");
                g_isD3D12Mode = false;
                return g_origPresent(sc, sync, flags);
            }
        }

        // Resize handling — only when resources are already built
        if (g_d3d12ResourcesReady)
        {
            DXGI_SWAP_CHAIN_DESC scd = {};
            if (SUCCEEDED(sc->GetDesc(&scd)) &&
                (scd.BufferDesc.Width != g_screenW || scd.BufferDesc.Height != g_screenH))
            {
                overlayLogFmt("presentHook: D3D12 resize %ux%u→%ux%u",
                    g_screenW, g_screenH, scd.BufferDesc.Width, scd.BufferDesc.Height);
                for (UINT i = 0; i < k_maxSwapBuffers; ++i)
                    if (g_d3d12BackBuffers[i]) { g_d3d12BackBuffers[i]->Release(); g_d3d12BackBuffers[i] = nullptr; }
                g_screenW = scd.BufferDesc.Width;
                g_screenH = scd.BufferDesc.Height;
                g_d3d12BufferCount = scd.BufferCount;
                D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_d3d12RTVHeap->GetCPUDescriptorHandleForHeapStart();
                for (UINT i = 0; i < g_d3d12BufferCount && i < k_maxSwapBuffers; ++i)
                {
                    sc->GetBuffer(i, IID_PPV_ARGS(&g_d3d12BackBuffers[i]));
                    if (g_d3d12BackBuffers[i])
                        g_d3d12Device->CreateRenderTargetView(g_d3d12BackBuffers[i], nullptr, rtvH);
                    g_d3d12RTVHandles[i] = rtvH;
                    rtvH.ptr += g_d3d12RTVDescSize;
                }
            }
        }

        // Get current back buffer index
        UINT bufIdx = 0;
        {
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3),
                reinterpret_cast<void**>(&sc3))) && sc3)
            {
                bufIdx = sc3->GetCurrentBackBufferIndex(); sc3->Release();
            }
        }

        if (!g_d3d12BackBuffers[bufIdx] || !g_visible)
            return g_origPresent(sc, sync, flags);

        // Build vertex geometry.  renderConsole() fills g_verts; because g_rtv
        // is null in D3D12 mode the D3D11 upload/draw block at the bottom of
        // renderConsole() is skipped automatically, leaving g_verts populated
        // for us to consume here.
        renderConsole();

        if (!g_verts.empty())
        {
            size_t drawCount = std::min(g_verts.size(), (size_t)k_maxVerts);

            // Upload vertices to upload-heap VB
            void* mapped = nullptr;
            D3D12_RANGE readRange = {};
            g_d3d12VB->Map(0, &readRange, &mapped);
            memcpy(mapped, g_verts.data(), drawCount * sizeof(Vertex));
            D3D12_RANGE writeRange = { 0, drawCount * sizeof(Vertex) };
            g_d3d12VB->Unmap(0, &writeRange);
            g_verts.clear();

            // Record command list
            g_d3d12CmdAlloc->Reset();
            g_d3d12CmdList->Reset(g_d3d12CmdAlloc, g_d3d12PSO);

            // Transition: PRESENT → RENDER_TARGET
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_d3d12BackBuffers[bufIdx];
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_d3d12CmdList->ResourceBarrier(1, &barrier);

            // Set render target
            g_d3d12CmdList->OMSetRenderTargets(1, &g_d3d12RTVHandles[bufIdx], FALSE, nullptr);

            // Viewport + scissor
            D3D12_VIEWPORT vp = { 0, 0, (float)g_screenW, (float)g_screenH, 0, 1 };
            D3D12_RECT scissor = { 0, 0, (LONG)g_screenW, (LONG)g_screenH };
            g_d3d12CmdList->RSSetViewports(1, &vp);
            g_d3d12CmdList->RSSetScissorRects(1, &scissor);

            // Root signature + descriptor heap
            g_d3d12CmdList->SetGraphicsRootSignature(g_d3d12RootSig);
            ID3D12DescriptorHeap* heaps[] = { g_d3d12SRVHeap };
            g_d3d12CmdList->SetDescriptorHeaps(1, heaps);
            g_d3d12CmdList->SetGraphicsRootDescriptorTable(0,
                g_d3d12SRVHeap->GetGPUDescriptorHandleForHeapStart());

            // VB
            D3D12_VERTEX_BUFFER_VIEW vbv = {};
            vbv.BufferLocation = g_d3d12VB->GetGPUVirtualAddress();
            vbv.SizeInBytes = (UINT)(drawCount * sizeof(Vertex));
            vbv.StrideInBytes = sizeof(Vertex);
            g_d3d12CmdList->IASetVertexBuffers(0, 1, &vbv);
            g_d3d12CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            g_d3d12CmdList->DrawInstanced((UINT)drawCount, 1, 0, 0);

            // Transition: RENDER_TARGET → PRESENT
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_d3d12CmdList->ResourceBarrier(1, &barrier);

            g_d3d12CmdList->Close();

            ID3D12CommandList* lists[] = { g_d3d12CmdList };
            g_d3d12Queue->ExecuteCommandLists(1, lists);
        }

        return g_origPresent(sc, sync, flags);
    }

    // ── D3D11 path (unchanged) ────────────────────────────────────────────────
    if (!g_rtv) createRTV();

    if (g_rtv)
    {
        DXGI_SWAP_CHAIN_DESC scd = {};
        if (SUCCEEDED(sc->GetDesc(&scd)))
        {
            if (scd.BufferDesc.Width != g_screenW || scd.BufferDesc.Height != g_screenH)
            {
                overlayLogFmt("presentHook: resize %ux%u→%ux%u",
                    g_screenW, g_screenH, scd.BufferDesc.Width, scd.BufferDesc.Height);
                releaseRTV();
                createRTV();
            }
        }
    }

    D3DStateBackup bk;
    bk.capture(g_ctx);
    renderConsole();
    bk.restore(g_ctx);

    return g_origPresent(sc, sync, flags);
}

static HRESULT __stdcall resizeBuffersHook(IDXGISwapChain* sc,
    UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT newFormat, UINT swapChainFlags)
{
    overlayLogFmt("resizeBuffersHook: %ux%u fmt=%u — releasing RTV", width, height, (unsigned)newFormat);

    // DXGI_ERROR_INVALID_CALL if any reference to the back buffer exists.
    // D3D11 path: our RTV holds one reference.
    // D3D12 path: our g_d3d12BackBuffers[] each hold one reference.
    // Release all of them unconditionally before the call.
    releaseRTV();

    if (g_isD3D12Mode)
    {
        for (UINT i = 0; i < k_maxSwapBuffers; ++i)
        {
            if (g_d3d12BackBuffers[i])
            {
                g_d3d12BackBuffers[i]->Release();
                g_d3d12BackBuffers[i] = nullptr;
            }
        }
        g_d3d12BufferCount = 0;
        overlayLog("resizeBuffersHook: D3D12 back buffers released");
    }

    HRESULT hr = g_origResizeBuffers(sc, bufferCount, width, height, newFormat, swapChainFlags);

    overlayLogFmt("resizeBuffersHook: origResizeBuffers returned 0x%08X", (unsigned)hr);

    if (SUCCEEDED(hr))
    {
        if (g_isD3D12Mode)
        {
            // Only re-acquire back buffers if D3D12 resources have already been
            // created by createD3D12OverlayResources — if presentHook hasn't run
            // its first frame yet the heap/descriptors don't exist and we must
            // not touch them. presentHook will call createD3D12OverlayResources
            // on its next frame which will GetBuffer and build the RTVs fresh.
            if (g_d3d12ResourcesReady)
            {
                DXGI_SWAP_CHAIN_DESC scd = {};
                sc->GetDesc(&scd);
                g_d3d12BufferCount = bufferCount ? bufferCount : scd.BufferCount;
                g_screenW = width ? width : scd.BufferDesc.Width;
                g_screenH = height ? height : scd.BufferDesc.Height;

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                    g_d3d12RTVHeap->GetCPUDescriptorHandleForHeapStart();
                for (UINT i = 0; i < g_d3d12BufferCount && i < k_maxSwapBuffers; ++i)
                {
                    if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&g_d3d12BackBuffers[i]))))
                    {
                        g_d3d12Device->CreateRenderTargetView(
                            g_d3d12BackBuffers[i], nullptr, rtvHandle);
                        g_d3d12RTVHandles[i] = rtvHandle;
                        rtvHandle.ptr += g_d3d12RTVDescSize;
                    }
                }
                overlayLogFmt("resizeBuffersHook: D3D12 back buffers recreated %ux%u bufs=%u",
                    g_screenW, g_screenH, g_d3d12BufferCount);
            }
            else
            {
                // Resources not ready yet — let presentHook's first-frame
                // createD3D12OverlayResources call handle everything.
                overlayLog("resizeBuffersHook: D3D12 resources not ready yet — deferring to presentHook");
            }
        }
        else
        {
            createRTV();
        }
    }

    return hr;
}

// ─────────────────────────────────────────────────────────────────────────────
// WndProc subclass
// ─────────────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ── Tilde toggle — intercepted regardless of current visibility ───────────
        // Must come before the !g_visible early-exit, otherwise the overlay can
        // never be re-opened once closed.
    if (msg == WM_CHAR)
    {
        wchar_t key = (wchar_t)wParam;
        if (key == L'`' || key == L'~')
        {
            g_visible = !g_visible;
            overlayLog(g_visible ? "console opened via tilde" : "console closed via tilde");
            // Snap back to live view on open and close
            g_scrollOffset = 0;
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wParam == VK_OEM_3) // VK_OEM_3 = the ` / ~ key
    {
        // Some games translate before WM_CHAR arrives; eat the KEYDOWN too.
        // (WM_CHAR is still the authoritative toggle above.)
        if (!g_visible)
            return 0; // suppress the keydown so the game doesn't act on it
    }

    // ── Raw mouse input — scroll wheel works even when game has mouse capture ──
        // Delivered only when the game window is foreground (no RIDEV_INPUTSINK),
        // so other apps are never affected when the user has alt-tabbed out.
    if (msg == WM_INPUT)
    {
        // Only scroll when console is visible AND the game window is foreground.
        // The foreground check is the critical guard — without RIDEV_INPUTSINK
        // Windows should not deliver WM_INPUT when we are not foreground anyway,
        // but we check explicitly as a safety net.
        if (g_visible && GetForegroundWindow() == hwnd)
        {
            UINT rawSize = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                RID_INPUT, nullptr, &rawSize, sizeof(RAWINPUTHEADER));
            if (rawSize > 0 && rawSize <= 256)
            {
                BYTE buf[256];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),
                    RID_INPUT, buf, &rawSize, sizeof(RAWINPUTHEADER)) == rawSize)
                {
                    RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf);
                    if (ri->header.dwType == RIM_TYPEMOUSE &&
                        (ri->data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
                    {
                        SHORT delta = static_cast<SHORT>(ri->data.mouse.usButtonData);
                        std::lock_guard<std::mutex> lock(g_mtx);
                        if (delta > 0)
                            g_scrollOffset += 3;
                        else
                            g_scrollOffset -= 3;
                        if (g_scrollOffset < 0) g_scrollOffset = 0;
                    }
                }
            }
        }
        // Always fall through — DefWindowProc must still process WM_INPUT
        // to call CleanupRawInput internally, or you get a handle leak.
        return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
    }

    // Pass through everything when console is not visible
    if (!g_visible)
        return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);

    // ── Eat all mouse input while open (scroll wheel handled separately) ──────
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
        msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
        msg == WM_MOUSEMOVE ||
        msg == WM_MOUSEHWHEEL || msg == WM_XBUTTONDOWN ||
        msg == WM_XBUTTONUP)
        return 0;

    // ── Mouse wheel scrolling (WM_MOUSEWHEEL fallback) ───────────────────────
    // Handled here as a fallback for cases where raw input is unavailable.
    if (msg == WM_MOUSEWHEEL)
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta > 0)
            g_scrollOffset += 3;
        else
            g_scrollOffset -= 3;
        if (g_scrollOffset < 0) g_scrollOffset = 0;
        return 0;
    }

    // ── Character input ───────────────────────────────────────────────────────
    if (msg == WM_CHAR)
    {
        wchar_t key = (wchar_t)wParam;
        // Skip control characters — handled in WM_KEYDOWN
        if (key >= L' ' && key < 127)
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            // If text is selected, delete it first then insert
            if (g_selStart != -1)
            {
                int lo = g_selStart < g_selEnd ? g_selStart : g_selEnd;
                int hi = g_selStart < g_selEnd ? g_selEnd : g_selStart;
                lo = lo < 0 ? 0 : lo;
                hi = hi > (int)g_inputLine.size() ? (int)g_inputLine.size() : hi;
                g_inputLine.erase(lo, hi - lo);
                g_selStart = g_selEnd = -1;
                // insert at lo
                g_inputLine.insert(lo, 1, (char)key);
            }
            else
            {
                g_inputLine += (char)key;
            }
            rebuildSuggestions(g_inputLine);
        }
        return 0;
    }

    // ── Key-down ──────────────────────────────────────────────────────────────
    if (msg == WM_KEYDOWN)
    {
        // Always let F12 through so the DLL can unload
        if (wParam == VK_F12)
            return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);

        std::lock_guard<std::mutex> lock(g_mtx);

        switch (wParam)
        {
        case VK_BACK:
            if (g_selStart != -1)
            {
                int lo = g_selStart < g_selEnd ? g_selStart : g_selEnd;
                int hi = g_selStart < g_selEnd ? g_selEnd : g_selStart;
                lo = lo < 0 ? 0 : lo;
                hi = hi > (int)g_inputLine.size() ? (int)g_inputLine.size() : hi;
                g_inputLine.erase(lo, hi - lo);
                g_selStart = g_selEnd = -1;
                rebuildSuggestions(g_inputLine);
            }
            else if (!g_inputLine.empty())
            {
                g_inputLine.pop_back();
                rebuildSuggestions(g_inputLine);
            }
            break;

        case VK_ESCAPE:
            g_inputLine.clear();
            g_suggestions.clear();
            g_suggestionIdx = -1;
            g_suggestionDraft.clear();
            g_selStart = g_selEnd = -1;
            g_scrollOffset = 0;
            break;

        case 'A':
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                // Select all
                if (!g_inputLine.empty())
                {
                    g_selStart = 0;
                    g_selEnd = (int)g_inputLine.size();
                }
            }
            break;

        case 'C':
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                // Copy selection if any, otherwise copy whole line
                std::string toCopy;
                if (g_selStart != -1)
                {
                    int lo = g_selStart < g_selEnd ? g_selStart : g_selEnd;
                    int hi = g_selStart < g_selEnd ? g_selEnd : g_selStart;
                    lo = lo < 0 ? 0 : lo;
                    hi = hi > (int)g_inputLine.size() ? (int)g_inputLine.size() : hi;
                    toCopy = g_inputLine.substr(lo, hi - lo);
                }
                else
                {
                    toCopy = g_inputLine;
                }
                if (!toCopy.empty() && OpenClipboard(nullptr))
                {
                    EmptyClipboard();
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, toCopy.size() + 1);
                    if (hMem)
                    {
                        char* dst = reinterpret_cast<char*>(GlobalLock(hMem));
                        if (dst)
                        {
                            memcpy(dst, toCopy.c_str(), toCopy.size() + 1);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_TEXT, hMem);
                        }
                    }
                    CloseClipboard();
                }
            }
            break;

        case 'V':
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                // Delete selection first if any
                if (g_selStart != -1)
                {
                    int lo = g_selStart < g_selEnd ? g_selStart : g_selEnd;
                    int hi = g_selStart < g_selEnd ? g_selEnd : g_selStart;
                    lo = lo < 0 ? 0 : lo;
                    hi = hi > (int)g_inputLine.size() ? (int)g_inputLine.size() : hi;
                    g_inputLine.erase(lo, hi - lo);
                    g_selStart = g_selEnd = -1;
                }
                if (OpenClipboard(nullptr))
                {
                    HANDLE hData = GetClipboardData(CF_TEXT);
                    if (hData)
                    {
                        const char* src = reinterpret_cast<const char*>(
                            GlobalLock(hData));
                        if (src)
                        {
                            for (; *src; ++src)
                            {
                                unsigned char c = static_cast<unsigned char>(*src);
                                if (c == '\r' || c == '\n') break;
                                if (c >= 0x20 && c <= 0x7E)
                                    g_inputLine += (char)c;
                            }
                            GlobalUnlock(hData);
                            rebuildSuggestions(g_inputLine);
                        }
                    }
                    CloseClipboard();
                }
            }
            break;

        case VK_UP:
            if (!g_suggestions.empty())
            {
                if (g_suggestionIdx == -1)
                    g_suggestionDraft = g_inputLine;

                if (g_suggestionIdx <= 0)
                    g_suggestionIdx = (int)g_suggestions.size() - 1;
                else
                    --g_suggestionIdx;

                g_inputLine = g_suggestions[(size_t)g_suggestionIdx];

                // Keep selected item visible in the scrolled window
                if (g_suggestionIdx < g_suggestionScrollOffset)
                    g_suggestionScrollOffset = g_suggestionIdx;
                else if (g_suggestionIdx >= g_suggestionScrollOffset + k_maxVisibleSuggestions)
                    g_suggestionScrollOffset = g_suggestionIdx - k_maxVisibleSuggestions + 1;
            }
            else if (!g_history.empty())
            {
                g_historyIdx = std::min(g_historyIdx + 1, (int)g_history.size() - 1);
                g_inputLine = g_history[(size_t)g_historyIdx];
                rebuildSuggestions(g_inputLine);
            }
            break;

        case VK_DOWN:
            if (!g_suggestions.empty())
            {
                if (g_suggestionIdx >= (int)g_suggestions.size() - 1)
                {
                    g_suggestionIdx = -1;
                    g_suggestionScrollOffset = 0;
                    g_inputLine = g_suggestionDraft;
                }
                else
                {
                    if (g_suggestionIdx == -1)
                        g_suggestionDraft = g_inputLine;
                    ++g_suggestionIdx;
                    g_inputLine = g_suggestions[(size_t)g_suggestionIdx];

                    // Keep selected item visible in the scrolled window
                    if (g_suggestionIdx < g_suggestionScrollOffset)
                        g_suggestionScrollOffset = g_suggestionIdx;
                    else if (g_suggestionIdx >= g_suggestionScrollOffset + k_maxVisibleSuggestions)
                        g_suggestionScrollOffset = g_suggestionIdx - k_maxVisibleSuggestions + 1;
                }
            }
            else if (g_historyIdx > 0)
            {
                --g_historyIdx;
                g_inputLine = g_history[(size_t)g_historyIdx];
                rebuildSuggestions(g_inputLine);
            }
            else
            {
                g_historyIdx = -1;
                g_inputLine.clear();
                g_suggestions.clear();
                g_suggestionIdx = -1;
                g_suggestionScrollOffset = 0;
            }
            break;

        case VK_TAB:
            // Accept first suggestion if none highlighted, then close dropdown
            if (!g_suggestions.empty())
            {
                int idx = (g_suggestionIdx >= 0) ? g_suggestionIdx : 0;
                g_inputLine = g_suggestions[idx];
                g_suggestions.clear();
                g_suggestionIdx = -1;
                g_suggestionDraft.clear();
            }
            break;

        case VK_RETURN:
        {
            if (!g_inputLine.empty())
            {
                // If a suggestion is selected, accept it first
                if (g_suggestionIdx >= 0 && g_suggestionIdx < (int)g_suggestions.size())
                    g_inputLine = g_suggestions[g_suggestionIdx];

                g_suggestions.clear();
                g_suggestionIdx = -1;
                g_suggestionDraft.clear();
                g_selStart = g_selEnd = -1;
                g_scrollOffset = 0;

                g_history.push_front(g_inputLine);
                if (g_history.size() > 50) g_history.pop_back();
                g_historyIdx = -1;

                // Echo line — gold, matching the "> " prompt colour
                g_responses.push_back(std::string(1, kColorGold) + "> " + g_inputLine);

                std::string cmd = g_inputLine;
                g_inputLine.clear();

                g_mtx.unlock();
                g_overlayReady = true;
                size_t countBefore = g_responses.size();
                g_overlayExecuting = true;
                std::string result = FrostbiteConsole::executeCommand(cmd.c_str());
                g_overlayExecuting = false;
                g_mtx.lock();

                if (!result.empty() && g_responses.size() == countBefore)
                {
                    char tag = kColorGreen;
                    if (result.find("Unknown console command") != std::string::npos)
                        tag = kColorRed;
                    g_responses.push_back(tag + result);
                }

                // Keep up to 4000 lines of history for scrollback
                while ((int)g_responses.size() > 4000)
                    g_responses.pop_front();
            }
            break;
        }

        default: break;
        }

        return 0; // eat all WM_KEYDOWN while console is open
    }

    // ── Eat key-up too — prevents the game acting on key releases ────────────
    if (msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
    {
        if (wParam == VK_F12)
            return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
        return 0;
    }

    // Pass everything else through
    return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API implementation
// ─────────────────────────────────────────────────────────────────────────────
namespace ConsoleOverlay
{

    bool probeIngameConsolePresent()
    {
        if (!s_probeComplete)
        {
            s_probeResult = doProbeIngameConsole();
            s_probeComplete = true;
        }
        return s_probeResult;
    }

    void initialize()
    {
        if (g_initialized) return;

        overlayLog("=== ConsoleOverlay::initialize ===");

        // ── Run detection first — abort immediately if game has its own console ───
        if (probeIngameConsolePresent())
        {
            overlayLog("initialize: real IngameConsoleImpl detected — "
                "skipping overlay injection (game has its own console)");
            return;
        }
        overlayLog("initialize: no real IngameConsoleImpl — injecting overlay");

        // ── Abort if exec command not yet resolved — overlay is useless without it ─
        if (!FrostbiteConsole::isReady())
        {
            overlayLog("initialize: exec command not resolved — skipping overlay");
            return;
        }

        // ── Resolve dynamic addresses ─────────────────────────────────────────────
        resolveAddresses();

        // ── Grab D3D objects by scanning DxRenderer for COM interfaces ────────────
            // Retry up to 120 times (60 seconds) — mirrors the consoleMethods retry loop
            // in the worker thread. Addresses may not be populated yet if the game's
            // renderer initializes after our DLL attaches.
        {
            bool d3dResolved = false;

            // Phase 1: normal resolve loop — up to 10 attempts (10 seconds).
            for (int attempt = 0; attempt < 10 && !d3dResolved; ++attempt)
            {
                if (attempt > 0)
                {
                    // Re-run address resolution on each retry — the DxRenderer
                    // singleton slot may have been null on the previous attempt.
                    Sleep(1000);
                    g_dxRendererInstanceAddr = nullptr;
                    g_dxRendererBaseSlot = nullptr;
                    g_dxRendererFieldOffset = 0;
                    g_hwndStaticAddr = nullptr;
                    resolveAddresses();
                }

                if (resolveD3DFromDXGI(resolveHwnd()))
                {
                    d3dResolved = true;
                }
                else
                {
                    static char retryMsg[64];
                    wsprintfA(retryMsg, "initialize: resolveD3DFromDXGI attempt %d failed, retrying", attempt + 1);
                    overlayLog(retryMsg);
                }
            }

            // Phase 2: if the normal path failed all 10 attempts, force the
            // DxRenderer pointer to null so resolveD3DFromDXGI takes the D3D12
            // scan branch (which does not depend on the DxRenderer singleton).
            if (!d3dResolved)
            {
                overlayLog("initialize: normal resolve failed after 10 attempts — trying D3D12 scan path once");
                g_dxRendererInstanceAddr = nullptr;
                g_dxRendererBaseSlot = nullptr;
                g_dxRendererFieldOffset = 0;
                if (resolveD3DFromDXGI(resolveHwnd()))
                {
                    d3dResolved = true;
                    overlayLog("initialize: D3D12 scan path succeeded");
                }
                else
                {
                    overlayLog("initialize: D3D12 scan path also failed — aborting");
                }
            }

            if (!d3dResolved)
            {
                overlayLog("initialize: all resolve attempts exhausted — aborting");
                return;
            }
        }

        g_device = g_resolvedDevice;
        g_ctx = g_resolvedContext;
        g_swapChain = g_resolvedSwapChain;

        overlayLogFmt("initialize: device=%p ctx=%p swapChain=%p",
            (void*)g_device, (void*)g_ctx, (void*)g_swapChain);

        if (!createD3DObjects()) { overlayLog("initialize: createD3DObjects failed"); return; }
        if (!bakeFontAtlas()) { overlayLog("initialize: bakeFontAtlas failed");    return; }

        createRTV();

        if (!installPresentHook()) { overlayLog("initialize: installPresentHook failed"); return; }

        // ── Subclass WndProc ──────────────────────────────────────────────────────
        g_hwnd = resolveHwnd();
        if (g_hwnd)
        {
            g_origWndProc = reinterpret_cast<tWndProc>(
                SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(overlayWndProc)));
            overlayLogFmt("initialize: WndProc subclassed hwnd=%p origWndProc=%p",
                (void*)g_hwnd, (void*)g_origWndProc);

            // Register for raw mouse input so scroll wheel is received even when
                        // the game has mouse capture.  No RIDEV_INPUTSINK — we only want
                        // delivery when the game window is the foreground window, so other
                        // apps are not affected when the user alt-tabs.
            RAWINPUTDEVICE rid = {};
            rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
            rid.usUsage = 0x02;  // HID_USAGE_GENERIC_MOUSE
            rid.dwFlags = 0;     // foreground only — no RIDEV_INPUTSINK
            rid.hwndTarget = g_hwnd;
            if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
                overlayLog("initialize: WARNING — RegisterRawInputDevices(mouse) failed");
            else
                overlayLog("initialize: raw mouse input registered");
        }
        else
        {
            overlayLog("initialize: WARNING — could not resolve HWND, "
                "keyboard input will not work");
        }

        // ── Register output handler ───────────────────────────────────────────────
        if (FrostbiteConsole::ConsoleBridge::instance().use3ArgHandler())
        {
            FrostbiteConsole::addOutputHandler3(&ConsoleOverlay::outputHandler);
            overlayLog("initialize: output handler registered (3-arg path)");
        }
        else
        {
            FrostbiteConsole::addOutputHandler(&ConsoleOverlay::outputHandler);
            overlayLog("initialize: output handler registered (4-arg/Skate path)");
        }

        // ── Hook DirectInput keyboard device ─────────────────────────────────────
        if (!installDInputHook())
            overlayLog("initialize: WARNING — DirectInput hook failed, game keys will not be blocked");

        // Show init message — will be visible when player first opens the overlay
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_responses.push_back(std::string(1, kColorWhite) + "Console overlay initialized.");
            g_responses.push_back(std::string(1, kColorWhite) + "Press '~' to show/hide this console.");
        }

        g_visible = true;
        g_initialized = true;

        overlayLog("=== ConsoleOverlay::initialize COMPLETE — press ~ to open ===");
    }

    void shutdown()
    {
        if (!g_initialized) return;
        overlayLog("=== ConsoleOverlay::shutdown ===");

        uninstallDInputHook();

        if (FrostbiteConsole::ConsoleBridge::instance().use3ArgHandler())
            FrostbiteConsole::removeOutputHandler3(&ConsoleOverlay::outputHandler);
        else
            FrostbiteConsole::removeOutputHandler(&ConsoleOverlay::outputHandler);

        // Unregister raw mouse so we stop receiving RIDEV_INPUTSINK messages
        if (g_hwnd)
        {
            RAWINPUTDEVICE rid = {};
            rid.usUsagePage = 0x01;
            rid.usUsage = 0x02;
            rid.dwFlags = RIDEV_REMOVE;
            rid.hwndTarget = nullptr;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
        }

        if (g_hwnd && g_origWndProc)
        {
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_origWndProc));
            g_origWndProc = nullptr;
            overlayLog("shutdown: WndProc restored");
        }

        uninstallPresentHook();
        destroyD3DObjects();

        g_initialized = false;
        overlayLog("=== ConsoleOverlay::shutdown COMPLETE ===");
    }

    void __fastcall outputHandler(void* /*thisDiscarded*/,
        const char* tag,
        const char* buf,
        unsigned int size)
    {
        if (!buf || size == 0) return;

        // Chain to pipe handler so ConsoleWindow still receives all output
        if (g_pipeOutputHandler)
            g_pipeOutputHandler(nullptr, tag, buf, size);

        if (!g_overlayReady) return;

        std::lock_guard<std::mutex> lock(g_mtx);

        const char* it = buf;
        const char* end = buf + size;
        const char* lineStart = buf;

        auto pushLine = [&](const char* a, const char* b)
            {
                if (a >= b) return;
                std::string line(a, b);
                char t = kColorGreen;  // default: result output is green
                if (line.find("Unknown console command") != std::string::npos)
                    t = kColorRed;
                else if (line.compare(0, 2, "> ") == 0)
                    t = kColorGold;    // echoed command lines stay gold
                g_responses.push_back(t + line);
            };

        while (it < end)
        {
            bool isCR = (*it == '\r');
            bool isLF = (*it == '\n');
            if (isCR || isLF)
            {
                pushLine(lineStart, it);
                if (isCR && (it + 1 < end) && *(it + 1) == '\n')
                    ++it;
                ++it;
                lineStart = it;
            }
            else ++it;
        }
        pushLine(lineStart, it);

        while ((int)g_responses.size() > 4000)
            g_responses.pop_front();
    }

} // namespace ConsoleOverlay