#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define FB_CONSOLE_OVERLAY_DLL_BUILD
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <string>
#include "../FrostbiteConsole.h"
#include "../ConsoleOverlay.h"

#pragma comment(lib, "psapi.lib")


// dxgi.dll proxy exports
//
// When renamed to dxgi.dll and placed in the game's install directory, the
// game loads this DLL automatically at startup as a proxy for the real
// dxgi.dll. We forward all DXGI exports to the real system DLL
//
// Detection: if no host pipe is waiting (proxy/standalone mode), the worker
// thread detects this and goes straight to the unlock path without waiting
// for a host connection. This only triggers when loaded by a game process —
// tool processes (InitfsTools.exe) are identified by checking the calling
// executable's name and skip this entirely

static HMODULE g_hRealDxgi = nullptr;

// Cached function pointers — resolved once in loadRealDxgiDll()
static HRESULT(__stdcall* g_fn_CreateDXGIFactory)(REFIID, void**) = nullptr;
static HRESULT(__stdcall* g_fn_CreateDXGIFactory1)(REFIID, void**) = nullptr;
static HRESULT(__stdcall* g_fn_CreateDXGIFactory2)(UINT, REFIID, void**) = nullptr;
static HRESULT(__stdcall* g_fn_DXGIGetDebugInterface1)(UINT, REFIID, void**) = nullptr;
static HRESULT(__stdcall* g_fn_DXGIReportAdapterConfiguration)(DWORD) = nullptr;
static void(__stdcall* g_fn_DXGIDumpJournal)() = nullptr;
static HRESULT(__stdcall* g_fn_ApplyCompatResolutionQuirking)(void*) = nullptr;
static HRESULT(__stdcall* g_fn_CompatString)(void*) = nullptr;
static HRESULT(__stdcall* g_fn_CompatValue)(void*) = nullptr;
static HRESULT(__stdcall* g_fn_DXGIDisableVBlankVirtualization)() = nullptr;
static HRESULT(__stdcall* g_fn_SetAppCompatStringPointer)(void*) = nullptr;

static void loadRealDxgiDll()
{
    if (g_hRealDxgi) return;
    static LONG s_loading = 0;
    if (InterlockedCompareExchange(&s_loading, 1, 0) != 0) return;
    char sysDir[MAX_PATH] = {};
    GetSystemDirectoryA(sysDir, MAX_PATH);
    char realPath[MAX_PATH] = {};
    lstrcpyA(realPath, sysDir);
    lstrcatA(realPath, "\\dxgi.dll");
    g_hRealDxgi = LoadLibraryExA(realPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_hRealDxgi) return;
    // Resolve all exports once
    g_fn_CreateDXGIFactory = reinterpret_cast<HRESULT(__stdcall*)(REFIID, void**)>(GetProcAddress(g_hRealDxgi, "CreateDXGIFactory"));
    g_fn_CreateDXGIFactory1 = reinterpret_cast<HRESULT(__stdcall*)(REFIID, void**)>(GetProcAddress(g_hRealDxgi, "CreateDXGIFactory1"));
    g_fn_CreateDXGIFactory2 = reinterpret_cast<HRESULT(__stdcall*)(UINT, REFIID, void**)>(GetProcAddress(g_hRealDxgi, "CreateDXGIFactory2"));
    g_fn_DXGIGetDebugInterface1 = reinterpret_cast<HRESULT(__stdcall*)(UINT, REFIID, void**)>(GetProcAddress(g_hRealDxgi, "DXGIGetDebugInterface1"));
    g_fn_DXGIReportAdapterConfiguration = reinterpret_cast<HRESULT(__stdcall*)(DWORD)>(GetProcAddress(g_hRealDxgi, "DXGIReportAdapterConfiguration"));
    g_fn_DXGIDumpJournal = reinterpret_cast<void(__stdcall*)()>(GetProcAddress(g_hRealDxgi, "DXGIDumpJournal"));
    g_fn_ApplyCompatResolutionQuirking = reinterpret_cast<HRESULT(__stdcall*)(void*)>(GetProcAddress(g_hRealDxgi, "ApplyCompatResolutionQuirking"));
    g_fn_CompatString = reinterpret_cast<HRESULT(__stdcall*)(void*)>(GetProcAddress(g_hRealDxgi, "CompatString"));
    g_fn_CompatValue = reinterpret_cast<HRESULT(__stdcall*)(void*)>(GetProcAddress(g_hRealDxgi, "CompatValue"));
    g_fn_DXGIDisableVBlankVirtualization = reinterpret_cast<HRESULT(__stdcall*)()>(GetProcAddress(g_hRealDxgi, "DXGIDisableVBlankVirtualization"));
    g_fn_SetAppCompatStringPointer = reinterpret_cast<HRESULT(__stdcall*)(void*)>(GetProcAddress(g_hRealDxgi, "SetAppCompatStringPointer"));
}

extern "C"
{
    HRESULT __stdcall proxyCreateDXGIFactory(REFIID riid, void** ppFactory)
    {
        loadRealDxgiDll(); return g_fn_CreateDXGIFactory ? g_fn_CreateDXGIFactory(riid, ppFactory) : E_FAIL;
    }

    HRESULT __stdcall proxyCreateDXGIFactory1(REFIID riid, void** ppFactory)
    {
        loadRealDxgiDll(); return g_fn_CreateDXGIFactory1 ? g_fn_CreateDXGIFactory1(riid, ppFactory) : E_FAIL;
    }

    HRESULT __stdcall proxyCreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
    {
        loadRealDxgiDll(); return g_fn_CreateDXGIFactory2 ? g_fn_CreateDXGIFactory2(flags, riid, ppFactory) : E_FAIL;
    }

    HRESULT __stdcall proxyDXGIGetDebugInterface1(UINT flags, REFIID riid, void** ppDebug)
    {
        loadRealDxgiDll(); return g_fn_DXGIGetDebugInterface1 ? g_fn_DXGIGetDebugInterface1(flags, riid, ppDebug) : E_FAIL;
    }

    HRESULT __stdcall proxyDXGIReportAdapterConfiguration(DWORD unknown)
    {
        loadRealDxgiDll(); return g_fn_DXGIReportAdapterConfiguration ? g_fn_DXGIReportAdapterConfiguration(unknown) : E_FAIL;
    }

    void __stdcall proxyDXGIDumpJournal()
    {
        loadRealDxgiDll(); if (g_fn_DXGIDumpJournal) g_fn_DXGIDumpJournal();
    }

    HRESULT __stdcall proxyApplyCompatResolutionQuirking(void* pUnknown)
    {
        loadRealDxgiDll(); return g_fn_ApplyCompatResolutionQuirking ? g_fn_ApplyCompatResolutionQuirking(pUnknown) : E_FAIL;
    }

    HRESULT __stdcall proxyCompatString(void* pUnknown)
    {
        loadRealDxgiDll(); return g_fn_CompatString ? g_fn_CompatString(pUnknown) : E_FAIL;
    }

    HRESULT __stdcall proxyCompatValue(void* pUnknown)
    {
        loadRealDxgiDll(); return g_fn_CompatValue ? g_fn_CompatValue(pUnknown) : E_FAIL;
    }

    HRESULT __stdcall proxyDXGIDisableVBlankVirtualization()
    {
        loadRealDxgiDll(); return g_fn_DXGIDisableVBlankVirtualization ? g_fn_DXGIDisableVBlankVirtualization() : E_FAIL;
    }

    HRESULT __stdcall proxySetAppCompatStringPointer(void* pUnknown)
    {
        loadRealDxgiDll(); return g_fn_SetAppCompatStringPointer ? g_fn_SetAppCompatStringPointer(pUnknown) : E_FAIL;
    }
} // extern "C"

// Globals
static HANDLE  g_hPipe = INVALID_HANDLE_VALUE;
static LONG    g_running = 0;
static volatile bool g_expectingOutput = false;
// Set by ConsoleOverlay when it calls executeCommand so the output handler
// does not silently drop game output during overlay-initiated executions
volatile bool g_overlayExecuting = false;

static HANDLE  g_thread = nullptr;
static HMODULE g_hModule = nullptr;
static char    g_gameDir[MAX_PATH] = {};
static char    g_currentCmdText[64 * 1024] = {};
static DWORD   g_execThreadId = 0;
static volatile LONG g_cmdExceptionLogged = 0;

// True while FBConsoleBridge_log.txt disk writes are active
extern bool g_enableDiskLog = false;

static bool    g_suspendedForUnlock = false;
static bool    g_proxyMode = false;
static volatile bool g_unlockPollStable = false;
static int     g_baselineCommandCount = 0;

// Sequence ID protocol
static volatile LONG g_currentCmdSeq = -1;

// Parses "<digits>:<rest>" out of a CMD: packet body (the bytes after the
// "CMD:" prefix). Returns the parsed seq and points outText at the
// remainder after the colon. Falls back to seq=-1, outText=body if the body
// isn't seq-tagged (defensive only — this DLL's paired host always tags)
static int parseCmdSeqAndText(const char* body, const char*& outText)
{
    const char* p = body;
    if (*p < '0' || *p > '9')
    {
        outText = body;
        return -1;
    }
    int seq = 0;
    while (*p >= '0' && *p <= '9')
    {
        seq = seq * 10 + (*p - '0');
        ++p;
    }
    if (*p != ':')
    {
        outText = body;
        return -1;
    }
    outText = p + 1;
    return seq;
}

static int parseIntA(const char* s)
{
    int val = 0;
    bool neg = false;
    if (*s == '-') { neg = true; ++s; }
    while (*s >= '0' && *s <= '9')
    {
        val = val * 10 + (*s - '0');
        ++s;
    }
    return neg ? -val : val;
}

// Static I/O buffers
static char g_readBuf[64 * 1024];
static char g_replyBuf[512 * 1024];

// STL strings — only ever touched from cpp_* functions, never from __try blocks
static std::string s_diagStr;
static std::string s_resultStr;

// Pre-READY log queue
static bool pipeWriteRaw(const char* data, uint32_t len);

// Logging helpers — pure Win32, no STL, safe from any context
static void pipeLogLine(const char* msg)
{
    static const char kHdr[] = "OUTPUT:-1:[Inject]|";
    const int hdrLen = sizeof(kHdr) - 1;
    int msgLen = lstrlenA(msg);
    int total = hdrLen + msgLen;
    if (total >= (int)sizeof(g_replyBuf)) return;

    char* p = g_replyBuf;
    CopyMemory(p, kHdr, hdrLen); p += hdrLen;
    CopyMemory(p, msg, msgLen);
    pipeWriteRaw(g_replyBuf, (uint32_t)total);
}

// Same wire format as pipeLogLine (seq=-1, never scan-scoped) but tagged
// "[Error]" instead of "[Inject]". The host's tag-based classification in
// PipeReaderThread::parsePacket() checks ciContains(tag, "error") BEFORE
// applying the bracket-tag debug rule, so this forces outError=true /
// outDebug=false — the line always shows in the normal (non-debug) log,
// rendered in the error colour, regardless of the Debug Log toggle
static void pipeLogErrorLine(const char* msg)
{
    static const char kHdr[] = "OUTPUT:-1:[Error]|";
    const int hdrLen = sizeof(kHdr) - 1;
    int msgLen = lstrlenA(msg);
    int total = hdrLen + msgLen;
    if (total >= (int)sizeof(g_replyBuf)) return;

    char* p = g_replyBuf;
    CopyMemory(p, kHdr, hdrLen); p += hdrLen;
    CopyMemory(p, msg, msgLen);
    pipeWriteRaw(g_replyBuf, (uint32_t)total);
}

// Sentinel prefix used only by FrostbiteConsole.cpp's shimExecCmd() to mark
// its "executeCommand: EXCEPTION..." line (a command crashing inside the
// game's own executeConsoleCommand) as one that must surface in the normal
// log, in red, with a restart notice appended
static const char kCrashMarker[] = "##CMDCRASH##";

static void logToFile(const char* msg)
{
    const int  markerLen = sizeof(kCrashMarker) - 1;
    const bool isCrashLine = (strncmp(msg, kCrashMarker, markerLen) == 0);
    const char* displayMsg = isCrashLine ? (msg + markerLen) : msg;

    // Append to disk log (only when explicitly re-enabled via g_enableDiskLog)
    if (g_enableDiskLog && g_gameDir[0])
    {
        char path[MAX_PATH];
        lstrcpyA(path, g_gameDir);
        lstrcatA(path, "\\FBConsoleBridge_log.txt");
        HANDLE hFile = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            DWORD w = 0;
            WriteFile(hFile, displayMsg, (DWORD)lstrlenA(displayMsg), &w, nullptr);
            WriteFile(hFile, "\r\n", 2, &w, nullptr);
            CloseHandle(hFile);
        }
    }

    if (g_hPipe == INVALID_HANDLE_VALUE) return;

    if (isCrashLine)
    {
        char buf[1200];
        wsprintfA(buf, "%s — restart may be required", displayMsg);
        pipeLogErrorLine(buf);
    }
    else
    {
        pipeLogLine(msg);
    }
}

// Turns off disk logging for the remainder of this DLL instance
static void stopDiskLoggingOnceConfirmed()
{
    if (!g_enableDiskLog) return; // already off
    g_enableDiskLog = false;
    logToFile("[log] total command count confirmed — disk logging stopped for this session");
}

static void logHex(const char* prefix, DWORD val)
{
    char tmp[128];
    wsprintfA(tmp, "%s0x%08lX", prefix, val);
    logToFile(tmp);
}

static void logDec(const char* prefix, DWORD val)
{
    char tmp[128];
    wsprintfA(tmp, "%s%lu", prefix, val);
    logToFile(tmp);
}

// Logs "EXCEPTION=0x...cmd='...'" exactly once for the command
static void logCmdExceptionOnce(const char* cmd, DWORD exceptionCode)
{
    if (InterlockedCompareExchange(&g_cmdExceptionLogged, 1, 0) != 0)
        return; // already logged for this command

    char safeCmd[201];
    safeCmd[0] = '\0';
    if (cmd) (void)lstrcpynA(safeCmd, cmd, sizeof(safeCmd));

    char msg[320];
    wsprintfA(msg, "[shim] execute EXCEPTION=0x%08lX cmd='%s'", exceptionCode, safeCmd);
    logToFile(msg);
}

// Pipe helpers — raw buffers only
static bool pipeWriteRaw(const char* data, uint32_t len)
{
    if (g_hPipe == INVALID_HANDLE_VALUE) return false;

    // Use overlapped writes with a timeout to prevent the game thread from
    // blocking indefinitely if the host hasn't drained the pipe read buffer
    // This is critical in proxy+pipe mode where the overlay calls executeCommand
    // from the game's render/message thread while the pipe reader is separate
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD w = 0;
    bool ok = true;

    if (!WriteFile(g_hPipe, &len, 4, &w, &ov))
    {
        if (GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0)
            {
                CancelIo(g_hPipe);
                ok = false;
            }
            else
            {
                GetOverlappedResult(g_hPipe, &ov, &w, FALSE);
            }
        }
        else ok = false;
    }

    if (ok)
    {
        ResetEvent(ov.hEvent);
        if (!WriteFile(g_hPipe, data, len, &w, &ov))
        {
            if (GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0)
                {
                    CancelIo(g_hPipe);
                    ok = false;
                }
                else
                {
                    GetOverlappedResult(g_hPipe, &ov, &w, FALSE);
                }
            }
            else ok = false;
        }
    }

    CloseHandle(ov.hEvent);
    return ok;
}

static bool pipeWriteStr(const char* msg)
{
    return pipeWriteRaw(msg, (uint32_t)lstrlenA(msg));
}

// Writes the seq-tagged RESULT: terminator for the command
static void pipeWriteResultTerminator(int seq)
{
    char buf[32];
    int len = wsprintfA(buf, "RESULT:%d:", seq);
    pipeWriteRaw(buf, (uint32_t)len);
}

// Batch buffer — accumulates lines during LIST/LIST_VARS, flushed once at end
static char  g_batchBuf[4 * 1024 * 1024]; // 4 MB
static int   g_batchLen = 0;
static bool  g_batching = false;

static void batchBegin()
{
    g_batchLen = 0;
    g_batching = true;
}

static void batchAppend(const char* data, int len)
{
    if (g_batchLen + len + 1 < (int)sizeof(g_batchBuf))
    {
        CopyMemory(g_batchBuf + g_batchLen, data, len);
        g_batchLen += len;
        g_batchBuf[g_batchLen++] = '\n'; // newline separator between entries
    }
}

static void batchFlush()
{
    g_batching = false;
    if (g_batchLen > 0)
        pipeWriteRaw(g_batchBuf, (uint32_t)g_batchLen);
    g_batchLen = 0;
}

static uint32_t pipeReadRaw(char* buf, uint32_t bufSize)
{
    if (g_hPipe == INVALID_HANDLE_VALUE) return 0;
    uint32_t len = 0;
    DWORD r = 0;
    if (!ReadFile(g_hPipe, &len, 4, &r, nullptr)) return 0;
    if (len == 0 || len >= bufSize) return 0;
    if (!ReadFile(g_hPipe, buf, len, &r, nullptr)) return 0;
    buf[r] = '\0';
    return r;
}

// Output handler core — pure Win32, no STL, safe to call from any thread.
static void __fastcall gameOutputHandler_impl(const char* tag, const char* buf, unsigned int size)
{
    if (!g_expectingOutput && !g_overlayExecuting) return;
    logToFile("[output] handler fired");
    if (!buf || size == 0 || size > 512 * 1024) return;
    if (reinterpret_cast<uintptr_t>(buf) <= 0xFFFF) return;
    {
        MEMORY_BASIC_INFORMATION _bufMbi{};
        if (!VirtualQuery(buf, &_bufMbi, sizeof(_bufMbi)) ||
            _bufMbi.State != MEM_COMMIT ||
            (_bufMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return;
    }
    {
        static char dbg[128];
        wsprintfA(dbg, "[output] tag='%s' size=%u", tag ? tag : "(null)", size);
        logToFile(dbg);
    }
    int n = (int)size;
    if (n > 0 && buf[n - 1] == '\0') --n;
    if (n <= 0) return;

    // Validate tag pointer before dereferencing
    const char* safeTag = "";
    if (tag && reinterpret_cast<uintptr_t>(tag) > 0xFFFF)
    {
        MEMORY_BASIC_INFORMATION _tagMbi{};
        if (VirtualQuery(tag, &_tagMbi, sizeof(_tagMbi)) &&
            _tagMbi.State == MEM_COMMIT &&
            !(_tagMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            safeTag = tag[0] ? tag : "";
        }
    }
    int tagLen = lstrlenA(safeTag);

    static char s_outputBuf[512 * 1024];
    // Build "OUTPUT:<seq>:" dynamically since its digit count varies
    int hdrLen = wsprintfA(s_outputBuf, "OUTPUT:%d:", (int)g_currentCmdSeq);
    int total = hdrLen + tagLen + 1 + n;
    if (total >= (int)sizeof(s_outputBuf)) return;

    char* p = s_outputBuf + hdrLen;
    if (tagLen) { CopyMemory(p, safeTag, tagLen); p += tagLen; }
    *p++ = '|';
    CopyMemory(p, buf, n);
    pipeWriteRaw(s_outputBuf, (uint32_t)total);
}

// Trampoline: 4-arg form
// FastDelegate passes (m_pThis, tag, buf, size) — RCX discarded, RDX/R8/R9 used
static void __fastcall gameOutputHandler(void* /*thisDiscarded*/,
    const char* tag,
    const char* buf,
    unsigned int size)
{
    gameOutputHandler_impl(tag, buf, size);
}

// Trampoline: 3-arg form
//
//   executeConsoleCommand calls each handler slot as:
//   RCX = slot[0x18]  (object ptr / "this", ignored by us)
//   RDX = eastl::string* result  (3-pointer layout: {char* begin, char* end, ...})
//
//   The output is already fully formatted into a single eastl::string:
//   rdx = tag      (const char*)
//   r8  = buf      (const char*)
//   r9d = size     (unsigned int)
static void __fastcall gameOutputHandler3(void*        /*thisIgnored*/,
    const char* tag,
    const char* buf,
    unsigned int size)
{
    gameOutputHandler_impl(tag, buf, size);
}

// cpp_* functions — may use std::string freely, must NEVER contain __try
__declspec(noinline) static void cpp_getDiag()
{
    s_diagStr = FrostbiteConsole::diagnosticInfo();
}

__declspec(noinline) static void cpp_execute(const char* cmd)
{
    s_resultStr = FrostbiteConsole::executeCommand(cmd);
}

__declspec(noinline) static void cpp_addHandler()
{
    logToFile("[cpp] calling addOutputHandler");
    // In proxy mode we skip direct registration only when the overlay
    // successfully initialized — its outputHandler is already in the game's
    // list and will chain to g_pipeOutputHandler
    const bool overlayOwnsSlot = g_proxyMode && ConsoleOverlay::isInitialized();
    if (FrostbiteConsole::ConsoleBridge::instance().use3ArgHandler())
    {
        uintptr_t fnAddr = (uintptr_t)&gameOutputHandler3;
        logHex("[cpp] gameOutputHandler3 hi=", (DWORD)(fnAddr >> 32));
        logHex("[cpp] gameOutputHandler3 lo=", (DWORD)(fnAddr & 0xFFFFFFFF));
        if (!overlayOwnsSlot)
        {
            logToFile("[cpp] registering gameOutputHandler3 directly (overlay not owning slot)");
            FrostbiteConsole::addOutputHandler3(&gameOutputHandler3);
        }
        else
        {
            logToFile("[cpp] proxy+overlay active — skipping direct registration, overlay will chain");
        }
        ConsoleOverlay::g_pipeOutputHandler =
            reinterpret_cast<ConsoleOverlay::PipeHandlerFn>(&gameOutputHandler3);
    }
    else
    {
        uintptr_t fnAddr = (uintptr_t)&gameOutputHandler;
        logHex("[cpp] gameOutputHandler hi=", (DWORD)(fnAddr >> 32));
        logHex("[cpp] gameOutputHandler lo=", (DWORD)(fnAddr & 0xFFFFFFFF));
        if (!overlayOwnsSlot)
        {
            logToFile("[cpp] registering gameOutputHandler directly (overlay not owning slot)");
            FrostbiteConsole::addOutputHandler(&gameOutputHandler);
        }
        else
        {
            logToFile("[cpp] proxy+overlay active — skipping direct registration, overlay will chain");
        }
        ConsoleOverlay::g_pipeOutputHandler =
            reinterpret_cast<ConsoleOverlay::PipeHandlerFn>(&gameOutputHandler);
    }
}

__declspec(noinline) static void cpp_removeHandler()
{
    // In proxy mode, only skip direct removal when the overlay owns the slot
    if (g_proxyMode)
    {
        ConsoleOverlay::g_pipeOutputHandler = nullptr;
        if (!ConsoleOverlay::isInitialized())
        {
            // We registered directly because the overlay did not own the slot —
            // remove the handler we added so re-attach starts clean
            if (FrostbiteConsole::ConsoleBridge::instance().use3ArgHandler())
                FrostbiteConsole::removeOutputHandler3(&gameOutputHandler3);
            else
                FrostbiteConsole::removeOutputHandler(&gameOutputHandler);
        }
        // If the overlay IS initialized it owns the slot — do not touch it,
        // the overlay manages its own registration lifetime via shutdown()
        return;
    }
    if (FrostbiteConsole::ConsoleBridge::instance().use3ArgHandler())
        FrostbiteConsole::removeOutputHandler3(&gameOutputHandler3);
    else
        FrostbiteConsole::removeOutputHandler(&gameOutputHandler);
}

__declspec(noinline) static int cpp_getMethods()
{
    int count = 0;
    FrostbiteConsole::getMethods(count);
    if (count < 0 || count > 100000)
        count = 0;
    return count;
}

__declspec(noinline) static void cpp_listVars()
{
    uint64_t smPtr = 0;
    if (!FrostbiteConsole::safeRead64(reinterpret_cast<void*>(0x145EE4920), &smPtr) || !smPtr)
    {
        logToFile("[vars] g_settingsManager is null");
        return;
    }

    // map header at smObj + 0x90 (144)
    uint64_t map = smPtr + 0x90;
    uint64_t bucketArray = 0;
    uint32_t bucketCount = 0;

    if (!FrostbiteConsole::safeRead64(reinterpret_cast<void*>(map + 0x08), &bucketArray)) return;
    if (!FrostbiteConsole::safeRead32(reinterpret_cast<void*>(map + 0x10), &bucketCount)) return;

    static char dbgBuf[128];
    wsprintfA(dbgBuf, "[vars] bucketArray=%p bucketCount=%u", (void*)bucketArray, bucketCount);
    logToFile(dbgBuf);

    static char lineBuf[512];

    for (uint32_t b = 0; b < bucketCount; b++)
    {
        uint64_t node = 0;
        if (!FrostbiteConsole::safeRead64(
            reinterpret_cast<void*>(bucketArray + 8ULL * b), &node)) continue;

        while (node && node != 0xFFFFFFFFFFFFFFFFULL)
        {
            // key is const char* stored at node+0x00
            uint64_t keyPtr = 0;
            if (!FrostbiteConsole::safeRead64(reinterpret_cast<void*>(node), &keyPtr)) break;

            if (keyPtr >= 0x10000ULL)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(keyPtr), &mbi, sizeof(mbi)) &&
                    mbi.State == MEM_COMMIT &&
                    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                {
                    const char* key = reinterpret_cast<const char*>(keyPtr);

                    // validate key string
                    uint8_t c0 = static_cast<uint8_t>(key[0]);
                    if ((isalpha(c0) || c0 == '_') && key[1] != '\0')
                    {
                        int vlen = _snprintf_s(lineBuf, sizeof(lineBuf), _TRUNCATE, "VAR:%s", key);
                        if (vlen > 0)
                            batchAppend(lineBuf, vlen);
                    }
                }
            }

            // next node at +0x70 (112)
            uint64_t next = 0;
            if (!FrostbiteConsole::safeRead64(
                reinterpret_cast<void*>(node + 0x70), &next)) break;
            node = next;
        }
    }
    logToFile("[vars] enumeration done");
}

// shim_* functions — contain __try/__except
__declspec(noinline) static int shimInit()
{
    __try { return FrostbiteConsole::init() ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logHex("[shim] init EXCEPTION=", GetExceptionCode());
        return -1;
    }
}

__declspec(noinline) static void shimGetDiag(char* outBuf, int outLen)
{
    __try
    {
        cpp_getDiag();
        (void)lstrcpynA(outBuf, s_diagStr.c_str(), outLen);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        (void)lstrcpynA(outBuf, "(exception in getDiag)", outLen);
    }
}

__declspec(noinline) static void shimAddHandler()
{
    __try
    {
        cpp_addHandler();
        logToFile("[shim] addHandler OK");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logHex("[shim] addHandler EXCEPTION=", GetExceptionCode());
        logToFile("[shim] addHandler failed — check delegate layout and fn address");
    }
}

__declspec(noinline) static void shimRemoveHandler()
{
    __try { cpp_removeHandler(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static int shimGetMethods()
{
    // getMethods reads BSS directly, no game call — SEH kept for safety only
    __try { return cpp_getMethods(); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logHex("[shim] getMethods EXCEPTION=", GetExceptionCode());
        return 0;
    }
}

__declspec(noinline) static void shimResetForReinit()
{
    __try { FrostbiteConsole::resetForReinit(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Execute a command and copy the result into g_replyBuf as a "RESULT:<text>"
// packet.  Returns the total packet length written into g_replyBuf
__declspec(noinline) static int shimExecute(const char* cmd)
{
    static char g_resultBuf[256 * 1024];
    g_resultBuf[0] = '\0';
    __try
    {
        cpp_execute(cmd);
        (void)lstrcpynA(g_resultBuf, s_resultStr.c_str(), (int)sizeof(g_resultBuf));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logCmdExceptionOnce(cmd, GetExceptionCode());
        lstrcpyA(g_resultBuf, "(exception)");
    }

    int resultLen = lstrlenA(g_resultBuf);
    int total = 7 + resultLen;
    if (total >= (int)sizeof(g_replyBuf)) total = (int)sizeof(g_replyBuf) - 1;
    CopyMemory(g_replyBuf, "RESULT:", 7);
    CopyMemory(g_replyBuf + 7, g_resultBuf, resultLen);
    g_replyBuf[total] = '\0';
    return total;
}

// Unlock patches
//
// Two patches, both found by byte-pattern scan so they work regardless of ASLR:
//
// Pattern: 66 C1 E8 0D  A8 01  0F 84 xx xx xx xx
//           shr ax,0Dh  test al,1  je <skip>
//
//   Patch 1 (offset +4, 2 bytes): A8 01 -> B0 01  (TEST AL,1 -> MOV AL,1)
//   Forces the isExposed flag to always be true
//
//   Patch 2 (offset +6, 6 bytes): 0F 84 xx xx xx xx -> 90 90 90 90 90 90
//   NOPs the je that skips registration when the flag is false
//
// Both patches together = Full console unlock

static uint8_t  s_patch1OrigBytes[2] = {};   // TEST AL,1 original
static uint8_t* s_patch1Addr = nullptr;
static uint8_t  s_patch2OrigBytes[6] = {};   // je original
static uint8_t* s_patch2Addr = nullptr;

// Patch 3 — Settings-object exposure gate. A separate, second hidden-flag
// check distinct from patch1/patch2's console-command exposure
// NOPing the je forces the "expose to console" branch below it to always
// run instead of being skipped when the Settings object was hidden
static uint8_t  s_patch3OrigBytes[6] = {};   // je original
static uint8_t* s_patch3Addr = nullptr;
static bool     s_unlockApplied = false;

// Finds the start of the function containing `addr`. Functions in this
// binary are padded with a run of INT3 (0xCC) up to their alignment
// boundary. 3+ consecutive real INT3 bytes essentially never occurs in
// actual code, so that's the bar
static uint8_t* findFunctionStart(uint8_t* addr, uint8_t* moduleBase, size_t maxBack)
{
    static const int kMinCCRun = 3;
    uint8_t* limit = (addr - maxBack > moduleBase) ? (addr - maxBack) : moduleBase;
    uint8_t* best = nullptr;
    int ccRun = 0;
    for (uint8_t* p = limit; p < addr; ++p)
    {
        if (*p == 0xCC) ++ccRun;
        else { if (ccRun >= kMinCCRun) best = p; ccRun = 0; }
    }
    return best;
}

// Returns the length of the instruction at `p` if it's either:
//  - an alignment NOP (90 / 66 90 / 0F 1F [+modrm/SIB/disp]) — seen between
//    a function's INT3 padding and its real prologue (Battlefront 1), or
//  - a generic "mov [base+disp], reg" store (88/89, optionally REX-prefixed)
//    — argument/register spills the compiler emits before the pushes that
//    aren't the specific r9b spill we track (PVZ spills rdx/rcx this way)
// We skip over both without caring what they do; returns 0 if neither shape
static int trySkipInstr(uint8_t* p, uint8_t* limit)
{
    if (p < limit && p[0] == 0x90) return 1;
    if (p + 1 < limit && p[0] == 0x66 && p[1] == 0x90) return 2;

    uint8_t* q = p;
    size_t prefixLen = 0;
    if (q < limit && (q[0] == 0x66 || (q[0] & 0xF0) == 0x40)) { prefixLen = 1; ++q; }
    if (q + 1 >= limit) return 0;

    bool isNop = (q[0] == 0x0F && q + 1 < limit && q[1] == 0x1F);
    bool isStore = (q[0] == 0x88 || q[0] == 0x89);
    if (!isNop && !isStore) return 0;

    size_t opLen = isNop ? 2 : 1; // "0F 1F" or single store opcode byte
    if (q + opLen >= limit) return 0;
    uint8_t modrm = q[opLen];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    if (isStore && mod == 3) return 0; // reg-to-reg, not a memory store

    size_t len = prefixLen + opLen + 1; // prefix + opcode(s) + modrm
    bool hasSIB = (rm == 4);
    if (hasSIB) { if (p + len >= limit) return 0; len += 1; }

    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0)
    {
        bool sibBaseIsBP = hasSIB && (p + len - 1 < limit) && ((p[len - 1] & 7) == 5);
        if ((!hasSIB && rm == 5) || sibBaseIsBP) len += 4;
    }

    if (p + len > limit) return 0;
    return (int)len;
}

// Derives the rbp-relative displacement where a function spills its 4th
// arg (r9b) — NOT a fixed constant (confirmed 0x67 vs 0x6F vs 0x7F across
// games, purely from push-count/lea-rbp differences). Simulates the
// prologue to solve: dispFromRbp = argSpillOffsetFromRSP0 - rbpOffsetFromRSP0
static bool computeExpectedGateDisp(uint8_t* funcStart, uint8_t* limit, uint8_t& outDisp)
{
    if (!funcStart) return false;
    uint8_t* p = funcStart;
    int  rspDelta = 0, raxDelta = 0, argSpillOffsetFromRSP0 = 0;
    bool haveRaxMirror = false, haveArgSpill = false;

    for (int guard = 0; guard < 96 && p + 8 < limit; ++guard)
    {
        // mov rax,rsp — either encoding (8B/C4 or 89/E0; Battlefront 1 uses the latter)
        if (p[0] == 0x48 && ((p[1] == 0x8B && p[2] == 0xC4) || (p[1] == 0x89 && p[2] == 0xE0)))
        {
            haveRaxMirror = true; raxDelta = rspDelta; p += 3; continue;
        }
        if (p[0] == 0x44 && p[1] == 0x88 && p[2] == 0x48) // mov [rax+d8], r9b
        {
            if (haveRaxMirror) { haveArgSpill = true; argSpillOffsetFromRSP0 = raxDelta + (int8_t)p[3]; }
            p += 4; continue;
        }
        if (p[0] == 0x44 && p[1] == 0x88 && p[2] == 0x4C && p[3] == 0x24) // mov [rsp+d8], r9b
        {
            haveArgSpill = true; argSpillOffsetFromRSP0 = rspDelta + (int8_t)p[4];
            p += 5; continue;
        }
        if (p[0] >= 0x50 && p[0] <= 0x57) { rspDelta -= 8; p += 1; continue; } // push reg
        if (p[0] == 0x41 && p[1] >= 0x50 && p[1] <= 0x57) { rspDelta -= 8; p += 2; continue; } // push r8-r15
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) { rspDelta -= (int8_t)p[3]; p += 4; continue; } // sub rsp,imm8
        if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) // sub rsp,imm32
        {
            int32_t imm; memcpy(&imm, p + 3, 4);
            rspDelta -= imm; p += 7; continue;
        }
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x68) // lea rbp,[rax+d8]
        {
            if (!haveRaxMirror || !haveArgSpill) return false;
            int disp = argSpillOffsetFromRSP0 - (raxDelta + (int8_t)p[3]);
            if (disp < 0 || disp > 255) return false;
            outDisp = (uint8_t)disp; return true;
        }
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x6C && p[3] == 0x24) // lea rbp,[rsp+d8]
        {
            if (!haveArgSpill) return false;
            int disp = argSpillOffsetFromRSP0 - (rspDelta + (int8_t)p[4]);
            if (disp < 0 || disp > 255) return false;
            outDisp = (uint8_t)disp; return true;
        }

        int skipLen = trySkipInstr(p, limit);
        if (skipLen > 0) { p += skipLen; continue; }

        return false; // unrecognized instruction — bail rather than mis-simulate
    }
    return false;
}

__declspec(noinline) static int shimApplyUnlock()
{
    __try
    {
        if (s_unlockApplied) return 1;

        // Pattern: shr ax,0Dh | test al,1 | je <rel32>
        static const uint8_t kPattern[] = {
            0x66, 0xC1, 0xE8, 0x0D,  // shr ax, 0Dh
            0xA8, 0x01,               // test al, 1
            0x0F, 0x84               // je (rel32 follows)
        };
        static const int kPatternLen = sizeof(kPattern);

        HMODULE mods[1] = {};
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) || !mods[0])
        {
            logToFile("[unlock] EnumProcessModules failed");
            return 0;
        }
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[0], &mi, sizeof(mi)))
        {
            logToFile("[unlock] GetModuleInformation failed");
            return 0;
        }

        uint8_t* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
        uint8_t* modEnd = base + static_cast<size_t>(mi.SizeOfImage);
        uint8_t* found = nullptr;
        int      matchCount = 0;

        MEMORY_BASIC_INFORMATION mbi{};
        for (uint8_t* cursor = base; cursor < modEnd; )
        {
            if (!VirtualQuery(cursor, &mbi, sizeof(mbi))) break;
            uint8_t* rBase = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
            uint8_t* rEnd = rBase + mbi.RegionSize;
            if (rEnd > modEnd) rEnd = modEnd;

            bool isExec = (mbi.State == MEM_COMMIT) &&
                (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
            if (isExec)
            {
                for (uint8_t* p = rBase; p + kPatternLen + 4 <= rEnd; ++p)
                {
                    if (memcmp(p, kPattern, kPatternLen) == 0)
                    {
                        ++matchCount;
                        static char dbg[128];
                        wsprintfA(dbg, "[unlock] pattern match #%d at %p", matchCount, p);
                        logToFile(dbg);
                        if (!found) found = p;
                    }
                }
            }
            if (rEnd <= cursor) break;
            cursor = rEnd;
        }

        if (!found) { logToFile("[unlock] pattern not found"); return -1; }
        if (matchCount > 1) logToFile("[unlock] WARNING: multiple matches, using first");

        // Patch 1: TEST AL,1 -> MOV AL,1  (offset +4, 2 bytes)
        // Forces exposeToConsole flag to always be true
        s_patch1Addr = found + 4;
        {
            DWORD oldProt = 0;
            if (!VirtualProtect(s_patch1Addr, 2, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                logToFile("[unlock] VirtualProtect failed (patch1)");
                return 0;
            }
            memcpy(s_patch1OrigBytes, s_patch1Addr, 2);
            s_patch1Addr[0] = 0xB0; // MOV AL,
            s_patch1Addr[1] = 0x01; //        1
            VirtualProtect(s_patch1Addr, 2, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), s_patch1Addr, 2);
            logToFile("[unlock] patch1 applied: MOV AL,1");
        }

        // Patch 2: NOP the je  (offset +6, 6 bytes)
        s_patch2Addr = found + 6;
        {
            DWORD oldProt = 0;
            if (!VirtualProtect(s_patch2Addr, 6, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                logToFile("[unlock] VirtualProtect failed (patch2)");
                // Revert patch1 so we don't leave a half-applied state
                DWORD p = 0;
                if (VirtualProtect(s_patch1Addr, 2, PAGE_EXECUTE_READWRITE, &p))
                {
                    memcpy(s_patch1Addr, s_patch1OrigBytes, 2);
                    VirtualProtect(s_patch1Addr, 2, p, &p);
                    FlushInstructionCache(GetCurrentProcess(), s_patch1Addr, 2);
                }
                return 0;
            }
            memcpy(s_patch2OrigBytes, s_patch2Addr, 6);
            memset(s_patch2Addr, 0x90, 6);
            VirtualProtect(s_patch2Addr, 6, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), s_patch2Addr, 6);
            logToFile("[unlock] patch2 applied: NOP je");
        }

        // Patch 3 — Settings-object exposure gate
        {
            const ptrdiff_t kRadius = 0x200000;
            uint8_t* scanStart = (found - kRadius > base) ? (found - kRadius) : base;
            uint8_t* scanEnd = (found + kRadius < modEnd) ? (found + kRadius) : modEnd;

            uint8_t* bestAddr = nullptr;
            bool      bestIsRbp = false;
            ptrdiff_t bestDist = 0;
            int       matchCount3 = 0;
            uint8_t* lastFuncStartQueriedFor = nullptr;
            uint8_t* lastFuncStart = nullptr;

            {
                MEMORY_BASIC_INFORMATION mbi3{};
                for (uint8_t* cursor = scanStart; cursor < scanEnd; )
                {
                    if (!VirtualQuery(cursor, &mbi3, sizeof(mbi3))) break;
                    uint8_t* rBase = reinterpret_cast<uint8_t*>(mbi3.BaseAddress);
                    uint8_t* rEnd = rBase + mbi3.RegionSize;
                    if (rBase < scanStart) rBase = scanStart;
                    if (rEnd > scanEnd) rEnd = scanEnd;

                    bool isExec = (mbi3.State == MEM_COMMIT) &&
                        (mbi3.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
                    if (isExec)
                    {
                        for (uint8_t* p = rBase; p + 12 <= rEnd; ++p)
                        {
                            size_t idx = 0;
                            bool hasRexR = false;
                            if (p[idx] == 0x44) { hasRexR = true; idx = 1; }

                            uint8_t baseReg = 0xFF, dispByte = 0;
                            size_t  afterOperand = 0;

                            if (p[idx] == 0x38) // CMP r/m8, r8
                            {
                                uint8_t modrm = p[idx + 1];
                                if (((modrm >> 6) & 3) == 1 && (modrm & 7) != 4)
                                {
                                    afterOperand = idx + 3;
                                    baseReg = modrm & 7;
                                    dispByte = p[idx + 2];
                                }
                            }
                            else if (!hasRexR && p[idx] == 0x80) // CMP r/m8, imm8 (/7)
                            {
                                uint8_t modrm = p[idx + 1];
                                if (((modrm >> 6) & 3) == 1 && ((modrm >> 3) & 7) == 7 && (modrm & 7) != 4)
                                {
                                    afterOperand = idx + 4;
                                    baseReg = modrm & 7;
                                    dispByte = p[idx + 2];
                                }
                            }
                            if (baseReg == 0xFF) continue;

                            uint8_t* funcStart = (p == lastFuncStartQueriedFor)
                                ? lastFuncStart : findFunctionStart(p, base, 0x4000);
                            lastFuncStartQueriedFor = p;
                            lastFuncStart = funcStart;

                            uint8_t predictedDisp = 0;
                            if (!funcStart || !computeExpectedGateDisp(funcStart, rEnd, predictedDisp)) continue;
                            if (dispByte != predictedDisp) continue;
                            if (p[afterOperand] != 0x0F || p[afterOperand + 1] != 0x84) continue;

                            int32_t rel32 = 0;
                            memcpy(&rel32, p + afterOperand + 2, 4);
                            if (rel32 < 0x20 || rel32 > 0x3000) continue;

                            uint8_t* jeAddr = p + afterOperand;
                            uint8_t* skipStart = jeAddr + 6;
                            uint8_t* skipEnd = skipStart + rel32;

                            int callCount = 0;
                            for (uint8_t* q = skipStart; q + 5 <= skipEnd && q + 5 <= rEnd; ++q)
                            {
                                if (q[0] == 0xE8)
                                {
                                    int32_t callRel = 0;
                                    memcpy(&callRel, q + 1, 4);
                                    uint8_t* callTarget = q + 5 + callRel;
                                    if (callTarget >= base && callTarget < modEnd) { ++callCount; q += 4; }
                                }
                            }
                            if (callCount < 2) continue;

                            ++matchCount3;
                            bool isRbp = (baseReg == 5);
                            ptrdiff_t dist = jeAddr > found ? (jeAddr - found) : (found - jeAddr);

                            bool better = !bestAddr
                                || (isRbp && !bestIsRbp)
                                || (isRbp == bestIsRbp && dist < bestDist);
                            if (better) { bestAddr = jeAddr; bestIsRbp = isRbp; bestDist = dist; }
                        }
                    }
                    if (rEnd <= cursor) break;
                    cursor = rEnd;
                }
            }

            if (!bestAddr)
            {
                logToFile("[unlock] patch3: no gate found - some settings will not be useable");
            }
            else
            {
                static char pickMsg[128];
                wsprintfA(pickMsg, "[unlock] patch3: %d gate(s) seen, selected %p (rbp=%d, dist=%Id from anchor %p)",
                    matchCount3, bestAddr, (int)bestIsRbp, bestDist, found);
                logToFile(pickMsg);

                s_patch3Addr = bestAddr;
                DWORD oldProt3 = 0;
                if (VirtualProtect(s_patch3Addr, 6, PAGE_EXECUTE_READWRITE, &oldProt3))
                {
                    memcpy(s_patch3OrigBytes, s_patch3Addr, 6);
                    memset(s_patch3Addr, 0x90, 6);
                    VirtualProtect(s_patch3Addr, 6, oldProt3, &oldProt3);
                    FlushInstructionCache(GetCurrentProcess(), s_patch3Addr, 6);
                    logToFile("[unlock] patch3 applied: NOP je");
                }
                else
                {
                    logToFile("[unlock] VirtualProtect failed (patch3)");
                    s_patch3Addr = nullptr;
                }
            }
        }

        s_unlockApplied = true;
        logToFile("[unlock] all patches applied — console commands unlocked");
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logHex("[unlock] EXCEPTION=", GetExceptionCode());
        return 0;
    }
}

__declspec(noinline) static int shimRevertUnlock()
{
    __try
    {
        if (!s_unlockApplied) return 1;

        // Revert patch 2: restore je
        if (s_patch2Addr)
        {
            DWORD oldProt = 0;
            if (VirtualProtect(s_patch2Addr, 6, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                memcpy(s_patch2Addr, s_patch2OrigBytes, 6);
                VirtualProtect(s_patch2Addr, 6, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), s_patch2Addr, 6);
                logToFile("[unlock] patch2 reverted: je restored");
            }
        }

        // Revert patch 1: restore TEST AL,1
        if (s_patch1Addr)
        {
            DWORD oldProt = 0;
            if (VirtualProtect(s_patch1Addr, 2, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                memcpy(s_patch1Addr, s_patch1OrigBytes, 2);
                VirtualProtect(s_patch1Addr, 2, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), s_patch1Addr, 2);
                logToFile("[unlock] patch1 reverted: TEST AL,1 restored");
            }
        }

        // Revert patch 3: restore je (Settings exposure gate)
        if (s_patch3Addr)
        {
            DWORD oldProt3 = 0;
            if (VirtualProtect(s_patch3Addr, 6, PAGE_EXECUTE_READWRITE, &oldProt3))
            {
                memcpy(s_patch3Addr, s_patch3OrigBytes, 6);
                VirtualProtect(s_patch3Addr, 6, oldProt3, &oldProt3);
                FlushInstructionCache(GetCurrentProcess(), s_patch3Addr, 6);
                logToFile("[unlock] patch3 reverted: je restored (Settings exposure)");
            }
            s_patch3Addr = nullptr;
        }

        s_unlockApplied = false;
        logToFile("[unlock] all patches reverted");
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logHex("[unlock] revert EXCEPTION=", GetExceptionCode());
        return 0;
    }
}

// Vectored exception handler — installed process-wide (see DllMain) so a
// crash caused by executing a console command is logged with that command's
// name even when the crash happens on a thread other than the one currently
// sitting inside shimExecute's own __try/__except (e.g. dispatched onto the
// engine's render/main thread)
static LONG WINAPI cmdExceptionVectoredHandler(EXCEPTION_POINTERS* info)
{
    if (g_expectingOutput &&
        g_execThreadId != 0 &&
        GetCurrentThreadId() == g_execThreadId)
    {
        DWORD code = (info && info->ExceptionRecord)
            ? info->ExceptionRecord->ExceptionCode : 0;
        logCmdExceptionOnce(g_currentCmdText, code);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Worker thread — no __try, no STL
static DWORD WINAPI workerThread(LPVOID)
{
    // Check for the skip-sleep event written by the Qt host during an unlock
    // restart. If it's signalled, the host injected us post-WaitForInputIdle
    // so the game is already up — no need to wait
    bool skipSleep = false;
    {
        HANDLE hEvt = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE,
            FALSE, "Global\\FBConsoleBridge_SkipSleep");
        if (hEvt)
        {
            skipSleep = (WaitForSingleObject(hEvt, 0) == WAIT_OBJECT_0);
            if (skipSleep) ResetEvent(hEvt);
            CloseHandle(hEvt);
        }
    }

    {
        static char suspDbg[128];
        wsprintfA(suspDbg, "[worker] entry: skipSleep=%d g_suspendedForUnlock=%d",
            (int)skipSleep, (int)g_suspendedForUnlock);
        logToFile(suspDbg);
    }

    if (g_proxyMode || skipSleep || g_suspendedForUnlock)
        logToFile("[worker] skipping startup sleep");
    else
    {
        logToFile("[worker] started, sleeping 3s for game to load");
        Sleep(3000);
    }

    if (g_suspendedForUnlock)
    {
        DWORD selfTid = GetCurrentThreadId();
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hSnap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(hSnap, &te))
            {
                do {
                    if (te.th32ThreadID == selfTid) continue;
                    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (hT) { SuspendThread(hT); CloseHandle(hT); }
                } while (Thread32Next(hSnap, &te));
            }
            CloseHandle(hSnap);
        }
        logToFile("[worker] all threads frozen early (unlock path)");
    }

    // Create named pipe (inject mode only)
    FrostbiteConsole::setLogCallback([](const char* line) { logToFile(line); });

    if (!g_proxyMode)
    {
        logToFile("[worker] creating pipe");
        g_hPipe = CreateNamedPipeA(
            "\\\\.\\pipe\\FBConsoleBridge",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 5000, nullptr);

        if (g_hPipe == INVALID_HANDLE_VALUE)
        {
            logDec("[worker] CreateNamedPipe failed GLE=", GetLastError());
            return 1;
        }
        logToFile("[worker] pipe created, waiting for host");

        if (!ConnectNamedPipe(g_hPipe, nullptr))
        {
            DWORD gle = GetLastError();
            if (gle != ERROR_PIPE_CONNECTED)
            {
                logDec("[worker] ConnectNamedPipe failed GLE=", gle);
                CloseHandle(g_hPipe);
                g_hPipe = INVALID_HANDLE_VALUE;
                return 1;
            }
        }
        logToFile("[worker] host connected");
        // Callback already set above — no need to set it again here
    }
    else
    {
        // Proxy mode: spin up a background thread that creates the pipe and
        // waits indefinitely for a host to connect. The worker (and the game)
        // continue immediately — nothing is frozen
        logToFile("[worker] proxy mode — spawning background pipe listener");

        HANDLE hPipeThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD
            {
                logToFile("[proxy pipe] listener started");

                // Outer loop: each pass creates a fresh pipe instance and waits
                // for a host. When the host detaches (command loop below breaks),
                // we loop back and re-arm the pipe instead of letting the thread
                // exit — otherwise Attach -> Detach -> Attach has nothing left to
                // connect to, since the pipe was destroyed after the first detach
                while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
                {
                    HANDLE hPipe = CreateNamedPipeA(
                        "\\\\.\\pipe\\FBConsoleBridge",
                        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                        1, 65536, 65536, 0, nullptr);

                    if (hPipe == INVALID_HANDLE_VALUE)
                    {
                        DWORD gle = GetLastError();
                        logDec("[proxy pipe] CreateNamedPipe failed GLE=", gle);
                        if (gle == ERROR_PIPE_BUSY || gle == ERROR_ACCESS_DENIED)
                        {
                            // ERROR_PIPE_BUSY  (231) — previous instance still alive
                            // ERROR_ACCESS_DENIED (5) — OS hasn't fully released the
                            //   previous handle yet; happens transiently after CloseHandle
                            // Both are recoverable — wait briefly and retry
                            logToFile("[proxy pipe] pipe not ready yet, waiting 500ms and retrying...");
                            Sleep(500);
                            continue;
                        }
                        return 1;
                    }

                    logToFile("[proxy pipe] pipe created, waiting for host (no timeout)");

                    if (!ConnectNamedPipe(hPipe, nullptr))
                    {
                        DWORD gle = GetLastError();
                        if (gle != ERROR_PIPE_CONNECTED)
                        {
                            logDec("[proxy pipe] ConnectNamedPipe failed GLE=", gle);
                            CloseHandle(hPipe);
                            hPipe = INVALID_HANDLE_VALUE;
                            Sleep(200);
                            continue;  // re-arm instead of killing the thread
                        }
                    }

                    logToFile("[proxy pipe] host connected");
                    g_hPipe = hPipe;
                    FrostbiteConsole::setLogCallback([](const char* line) { logToFile(line); });

                    // Register output handler so game output flows to the pipe
                    if (FrostbiteConsole::isReady())
                    {
                        logToFile("[proxy pipe] registering output handler");
                        shimAddHandler();
                    }

                    // Send READY with the live method count
                    {
                        int cmdCount = shimGetMethods();
                        static char readyBuf[64];
                        wsprintfA(readyBuf, "READY:%d", cmdCount);
                        pipeWriteStr(readyBuf);
                    }

                    // If the unlock poll already stabilized before this host
                    // connected (e.g. attaching well after game launch), that
                    // one-time UNLOCK_STABLE:1 packet already went out with
                    // nobody listening. Re-send it now so a late attach still
                    // gets a deterministic, correct signal instead of the host
                    // having no way to know whether it should keep waiting
                    if (g_unlockPollStable)
                    {
                        logToFile("[proxy pipe] unlock already stable — resending signal for late attach");
                        static const char kStableMsg[] = "UNLOCK_STABLE:1";
                        pipeWriteRaw(kStableMsg, (uint32_t)(sizeof(kStableMsg) - 1));
                    }

                    // Command loop — identical to the main worker loop
                    logToFile("[proxy pipe] entering command loop");
                    static char proxyReadBuf[64 * 1024];
                    while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
                    {
                        uint32_t pktLen = 0;
                        {
                            DWORD r = 0;
                            if (!ReadFile(hPipe, &pktLen, 4, &r, nullptr) || r != 4) break;
                            if (pktLen == 0 || pktLen >= sizeof(proxyReadBuf)) break;
                            if (!ReadFile(hPipe, proxyReadBuf, pktLen, &r, nullptr)) break;
                            proxyReadBuf[r] = '\0';
                            pktLen = r;
                        }

                        if (pktLen <= 4 ||
                            proxyReadBuf[0] != 'C' || proxyReadBuf[1] != 'M' ||
                            proxyReadBuf[2] != 'D' || proxyReadBuf[3] != ':')
                        {
                            logToFile("[proxy pipe] unknown packet, ignoring");
                            continue;
                        }

                        const char* cmd = nullptr;
                        int seq = parseCmdSeqAndText(proxyReadBuf + 4, cmd);
                        g_currentCmdSeq = seq;

                        if (lstrcmpA(cmd, "__LIST__") == 0)
                        {
                            logToFile("[proxy pipe] handling __LIST__");
                            batchBegin();
                            batchAppend("METHODS:0", 9);

                            int count = 0;
                            const FrostbiteConsole::ConsoleMethod* const* methods =
                                FrostbiteConsole::getMethods(count);

                            if (count == 0)
                            {
                                logToFile("[proxy pipe] getMethods=0, waiting 2s and retrying");
                                Sleep(2000);
                                methods = FrostbiteConsole::getMethods(count);
                            }

                            if (methods && count > 0 && count < 100000)
                            {
                                static char lineBuf[512];
                                for (int i = 0; i < count; ++i)
                                {
                                    const FrostbiteConsole::ConsoleMethod* m = methods[i];
                                    uintptr_t mAddr = reinterpret_cast<uintptr_t>(m);
                                    if (mAddr < 0x10000 || mAddr > 0x7FFFFFFFFFFF) continue;
                                    const uint8_t* raw = reinterpret_cast<const uint8_t*>(m);
                                    uint64_t namePtr = 0;
                                    if (!FrostbiteConsole::safeRead64(
                                        const_cast<uint8_t*>(raw + 0x08), &namePtr)) continue;
                                    if (namePtr < 0x10000ULL) continue;
                                    MEMORY_BASIC_INFORMATION _mbi{};
                                    if (!VirtualQuery(reinterpret_cast<void*>(
                                        static_cast<uintptr_t>(namePtr)),
                                        &_mbi, sizeof(_mbi)) ||
                                        _mbi.State != MEM_COMMIT ||
                                        (_mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                                    const char* name = reinterpret_cast<const char*>(
                                        static_cast<uintptr_t>(namePtr));
                                    uint8_t c0u = static_cast<uint8_t>(name[0]);
                                    if (!isalpha(c0u) && c0u != '_') continue;
                                    if (name[1] == '\0') continue;
                                    bool nameValid = true;
                                    int nameLen = 0;
                                    for (int ni = 0; ni < 128; ++ni) {
                                        uint8_t c = static_cast<uint8_t>(name[ni]);
                                        if (c == 0) break;
                                        if (c < 0x20 || c > 0x7E) { nameValid = false; break; }
                                        nameLen++;
                                    }
                                    if (!nameValid || nameLen < 2) continue;
                                    static char safeName[128];
                                    (void)lstrcpynA(safeName, name, sizeof(safeName));
                                    uint64_t groupPtr = 0;
                                    FrostbiteConsole::safeRead64(
                                        const_cast<uint8_t*>(raw + 0x10), &groupPtr);
                                    // Read description at +0x18 (may be null or unreadable)
                                    static char safeDesc[256];
                                    safeDesc[0] = '\0';
                                    {
                                        uint64_t descPtr = 0;
                                        if (FrostbiteConsole::safeRead64(
                                            const_cast<uint8_t*>(raw + 0x18), &descPtr) &&
                                            descPtr >= 0x10000ULL)
                                        {
                                            MEMORY_BASIC_INFORMATION _descMbi{};
                                            if (VirtualQuery(reinterpret_cast<void*>(
                                                static_cast<uintptr_t>(descPtr)),
                                                &_descMbi, sizeof(_descMbi)) &&
                                                _descMbi.State == MEM_COMMIT &&
                                                !(_descMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                                            {
                                                const char* dp = reinterpret_cast<const char*>(
                                                    static_cast<uintptr_t>(descPtr));
                                                // Validate: printable ASCII only, max 255 chars
                                                bool descOk = true;
                                                int dlen = 0;
                                                for (int di = 0; di < 255; ++di) {
                                                    uint8_t dc = static_cast<uint8_t>(dp[di]);
                                                    if (dc == 0) break;
                                                    if (dc < 0x20 || dc > 0x7E) { descOk = false; break; }
                                                    dlen++;
                                                }
                                                if (descOk && dlen > 0)
                                                    (void)lstrcpynA(safeDesc, dp, sizeof(safeDesc));
                                            }
                                        }
                                    }

                                    int len = 0;
                                    if (groupPtr >= 0x10000ULL)
                                    {
                                        MEMORY_BASIC_INFORMATION _grpMbi{};
                                        if (VirtualQuery(reinterpret_cast<void*>(
                                            static_cast<uintptr_t>(groupPtr)),
                                            &_grpMbi, sizeof(_grpMbi)) &&
                                            _grpMbi.State == MEM_COMMIT &&
                                            !(_grpMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                                        {
                                            const char* grp = reinterpret_cast<const char*>(
                                                static_cast<uintptr_t>(groupPtr));
                                            uint8_t gc0 = static_cast<uint8_t>(grp[0]);
                                            if (isalpha(gc0) || gc0 == '_')
                                                len = safeDesc[0]
                                                ? _snprintf_s(lineBuf, sizeof(lineBuf),
                                                    _TRUNCATE, "METHOD:%s.%s\t%s", grp, safeName, safeDesc)
                                                : _snprintf_s(lineBuf, sizeof(lineBuf),
                                                    _TRUNCATE, "METHOD:%s.%s", grp, safeName);
                                        }
                                    }
                                    if (len <= 0)
                                        len = safeDesc[0]
                                        ? _snprintf_s(lineBuf, sizeof(lineBuf),
                                            _TRUNCATE, "METHOD:%s\t%s", safeName, safeDesc)
                                        : _snprintf_s(lineBuf, sizeof(lineBuf),
                                            _TRUNCATE, "METHOD:%s", safeName);
                                    if (len > 0)
                                        batchAppend(lineBuf, len);
                                }
                            }

                            // Instance methods
                            {
                                int instCount = 0, instStride = 0;
                                const uint8_t* instBase = reinterpret_cast<const uint8_t*>(
                                    FrostbiteConsole::ConsoleBridge::instance()
                                    .getInstanceMethodsBase(instCount, instStride));
                                for (int i = 0; instBase && i < instCount; ++i) {
                                    const uint8_t* elem = instBase + i * instStride;
                                    uint64_t namePtr = 0, groupPtr = 0;
                                    if (!FrostbiteConsole::safeRead64(
                                        const_cast<uint8_t*>(elem + 0x00), &namePtr)) continue;
                                    if (namePtr < 0x10000ULL) continue;
                                    MEMORY_BASIC_INFORMATION _mbi2{};
                                    if (!VirtualQuery(reinterpret_cast<void*>(
                                        static_cast<uintptr_t>(namePtr)), &_mbi2, sizeof(_mbi2)) ||
                                        _mbi2.State != MEM_COMMIT ||
                                        (_mbi2.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                                    const char* name = reinterpret_cast<const char*>(
                                        static_cast<uintptr_t>(namePtr));
                                    uint8_t c0 = static_cast<uint8_t>(name[0]);
                                    if (!isalpha(c0) && c0 != '_') continue;
                                    if (name[1] == '\0') continue;
                                    FrostbiteConsole::safeRead64(
                                        const_cast<uint8_t*>(elem + 0x08), &groupPtr);
                                    static char instLine[512];
                                    if (groupPtr >= 0x10000ULL) {
                                        const char* grp = reinterpret_cast<const char*>(
                                            static_cast<uintptr_t>(groupPtr));
                                        _snprintf_s(instLine, sizeof(instLine), _TRUNCATE,
                                            "METHOD:%s.%s", grp, name);
                                    }
                                    else {
                                        _snprintf_s(instLine, sizeof(instLine), _TRUNCATE,
                                            "METHOD:%s", name);
                                    }
                                    batchAppend(instLine, lstrlenA(instLine));
                                }
                            }
                            {
                                char resTerm[32];
                                int resLen = wsprintfA(resTerm, "RESULT:%d:", seq);
                                batchAppend(resTerm, resLen);
                            }
                            batchFlush();
                            logToFile("[proxy pipe] list done");
                        }
                        else if (lstrcmpA(cmd, "__LIST_VARS__") == 0)
                        {
                            logToFile("[proxy pipe] handling __LIST_VARS__");
                            batchBegin();
                            batchAppend("VARS:0", 6);
                            __try { cpp_listVars(); }
                            __except (EXCEPTION_EXECUTE_HANDLER)
                            {
                                logHex("[proxy pipe] LIST_VARS exception=", GetExceptionCode());
                            }
                            {
                                char resTerm[32];
                                int resLen = wsprintfA(resTerm, "RESULT:%d:", seq);
                                batchAppend(resTerm, resLen);
                            }
                            batchFlush();
                            logToFile("[proxy pipe] list vars done");
                            stopDiskLoggingOnceConfirmed();
                        }
                        else
                        {
                            logToFile("[proxy pipe] executing:");
                            logToFile(cmd);
                            static char s_proxyExecCmd[64 * 1024];
                            (void)lstrcpynA(s_proxyExecCmd, cmd, sizeof(s_proxyExecCmd));
                            lstrcpynA(g_currentCmdText, s_proxyExecCmd, sizeof(g_currentCmdText));
                            g_cmdExceptionLogged = 0;
                            g_execThreadId = GetCurrentThreadId();
                            g_expectingOutput = true;
                            shimExecute(s_proxyExecCmd);
                            g_expectingOutput = false;
                            g_execThreadId = 0;
                            pipeWriteResultTerminator(seq);
                            logToFile("[proxy pipe] cmd done");
                        }
                    }

                    logToFile("[proxy pipe] command loop exited");
                    // Unregister the output handler before closing the pipe so that
                    // pipeWriteRaw is not called with an invalid handle, and so we
                    // can cleanly re-register it when the next host connects (line 930)
                    if (FrostbiteConsole::isReady())
                    {
                        logToFile("[proxy pipe] removing output handler before re-arm");
                        shimRemoveHandler();
                    }
                    DisconnectNamedPipe(hPipe);
                    CloseHandle(hPipe);
                    hPipe = INVALID_HANDLE_VALUE;
                    g_hPipe = INVALID_HANDLE_VALUE;
                    logToFile("[proxy pipe] host detached — re-arming pipe for next attach");
                }

                logToFile("[proxy pipe] listener thread exiting (g_running=0)");
                return 0;
            }, nullptr, 0, nullptr);

        if (hPipeThread) CloseHandle(hPipeThread);
    }

    // FrostbiteConsole::init — retry until exec command resolves
    // When injected early (before the game's main loop starts), the exec command
    // anchor string may not be reachable yet. Keep retrying until it resolves or
    // 60 seconds pass. The unlock patches are already applied from DLL_PROCESS_ATTACH
    // so settings registration is being intercepted during this whole wait
    logToFile("[worker] calling init (with retry)");
    int initResult = 0;
    static char diagBuf[1024];

    if (g_suspendedForUnlock)
    {
        // If DllMain already applied the patches successfully, skip the
        // scan entirely — the pattern bytes have been overwritten and will
        // never be found again
        if (s_unlockApplied)
        {
            logToFile("[worker] unlock path: patches already applied by DllMain, skipping scan");
        }
        else
        {

            // Fast path: scan only for the 8-byte patch pattern directly
            // This is orders of magnitude faster than running init() and does not
            // depend on any string anchor or prologue scan completing first
            // We poll every 10ms until the pattern is found, then immediately
            // freeze all threads and apply the patches
            static const uint8_t kPattern[] = {
                0x66, 0xC1, 0xE8, 0x0D,  // shr ax, 0Dh
                0xA8, 0x01,               // test al, 1
                0x0F, 0x84               // je (rel32 follows)
            };
            static const int kPatternLen = sizeof(kPattern);

            logToFile("[worker] unlock path: scanning for patch pattern directly");

            HMODULE mods[1] = {};
            DWORD needed = 0;
            EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed);
            MODULEINFO mi{};
            GetModuleInformation(GetCurrentProcess(), mods[0], &mi, sizeof(mi));
            uint8_t* modBase = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
            size_t   modSize = static_cast<size_t>(mi.SizeOfImage);

            DWORD selfTid = GetCurrentThreadId();

            // Scan helper — one full pass over executable regions
            auto scanOnce = [&]() -> bool {
                MEMORY_BASIC_INFORMATION pmbi{};
                uintptr_t cursor = reinterpret_cast<uintptr_t>(modBase);
                uintptr_t modEnd = cursor + modSize;
                while (cursor < modEnd)
                {
                    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &pmbi, sizeof(pmbi)))
                        break;
                    uintptr_t rEnd = reinterpret_cast<uintptr_t>(pmbi.BaseAddress) + pmbi.RegionSize;
                    if (rEnd > modEnd) rEnd = modEnd;
                    bool isExec = (pmbi.State == MEM_COMMIT) &&
                        (pmbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
                    if (isExec)
                    {
                        uint8_t* region = reinterpret_cast<uint8_t*>(pmbi.BaseAddress);
                        size_t   rsize = static_cast<size_t>(rEnd - reinterpret_cast<uintptr_t>(pmbi.BaseAddress));
                        for (size_t i = 0; i + kPatternLen + 4 <= rsize; ++i)
                            if (memcmp(region + i, kPattern, kPatternLen) == 0)
                                return true;
                    }
                    if (rEnd <= cursor) break;
                    cursor = rEnd;
                }
                return false;
                };

            auto freezeThreads = [&]() {
                HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (hSnap != INVALID_HANDLE_VALUE)
                {
                    THREADENTRY32 te{};
                    te.dwSize = sizeof(te);
                    if (Thread32First(hSnap, &te))
                    {
                        do {
                            if (te.th32ThreadID == selfTid) continue;
                            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                            HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                            if (hT) { SuspendThread(hT); CloseHandle(hT); }
                        } while (Thread32Next(hSnap, &te));
                    }
                    CloseHandle(hSnap);
                }
                };

            auto thawThreads = [&]() {
                HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (hSnap != INVALID_HANDLE_VALUE)
                {
                    THREADENTRY32 te{};
                    te.dwSize = sizeof(te);
                    if (Thread32First(hSnap, &te))
                    {
                        do {
                            if (te.th32ThreadID == selfTid) continue;
                            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                            HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                            if (hT) { ResumeThread(hT); CloseHandle(hT); }
                        } while (Thread32Next(hSnap, &te));
                    }
                    CloseHandle(hSnap);
                }
                };

            // Threads are already frozen from the early-freeze block at worker entry
            // Pulse-scan: thaw briefly to let the loader advance, re-freeze, scan
            // Exits immediately on first match — no unnecessary delay
            bool patternFound = false;
            logToFile("[worker] unlock path: scanning for patch pattern (frozen, with pulses)");
            for (int attempt = 0; attempt < 300 && !patternFound; ++attempt)
            {
                patternFound = scanOnce();
                if (!patternFound)
                {
                    static char pulseMsg[64];
                    wsprintfA(pulseMsg, "[worker] pattern not found attempt %d, pulsing", attempt + 1);
                    logToFile(pulseMsg);
                    thawThreads();
                    Sleep(10);
                    freezeThreads();
                }
            }

            if (patternFound)
                logToFile("[worker] all threads frozen for pattern scan");
            else
                logToFile("[worker] pattern not found after 300 attempts — proceeding");

            if (patternFound)
            {
                logToFile("[worker] patch pattern found — applying patches");

                int unlockResult = shimApplyUnlock();
                static char unlockMsg[64];
                wsprintfA(unlockMsg, "[worker] shimApplyUnlock result=%d", unlockResult);
                logToFile(unlockMsg);

                if (unlockResult != 1)
                {
                    logToFile("[worker] shimApplyUnlock failed — resuming threads");
                    HANDLE hSnap2 = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (hSnap2 != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 te{};
                        te.dwSize = sizeof(te);
                        if (Thread32First(hSnap2, &te))
                        {
                            do {
                                if (te.th32ThreadID == selfTid) continue;
                                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                                HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                                if (hT) { ResumeThread(hT); CloseHandle(hT); }
                            } while (Thread32Next(hSnap2, &te));
                        }
                        CloseHandle(hSnap2);
                    }
                    g_suspendedForUnlock = false;
                }
                // Threads remain frozen until post-READY resume block.
            }
            else
            {
                logToFile("[worker] patch pattern not found within timeout — resuming threads and proceeding without unlock");
                // Pattern never appeared; thaw now since we won't reach the post-READY resume block
                HANDLE hSnap2 = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (hSnap2 != INVALID_HANDLE_VALUE)
                {
                    THREADENTRY32 te{};
                    te.dwSize = sizeof(te);
                    if (Thread32First(hSnap2, &te))
                    {
                        do {
                            if (te.th32ThreadID == selfTid) continue;
                            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                            HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                            if (hT) { ResumeThread(hT); CloseHandle(hT); }
                        } while (Thread32Next(hSnap2, &te));
                    }
                    CloseHandle(hSnap2);
                }
                g_suspendedForUnlock = false;
            }

            // Skip full init pre-resume — consoleMethods won't be populated while
            // threads are frozen so the scan is wasted. Just mark initResult=1
            // so the worker proceeds to the resume block. The real init runs
            // post-resume where consoleMethods is actually live
            initResult = 1;
            diagBuf[0] = '\0';

        } // end else (scan block)

        initResult = 1;
        diagBuf[0] = '\0';
    }
    else
    {
        // Normal (non-unlock) path
        // Freeze all other threads for the duration of the init scan so the
        // game's memory state is stable while FrostbiteConsole::init() walks it
        // Threads are resumed immediately after init resolves (or gives up)
        DWORD selfTid = GetCurrentThreadId();

        // Freeze
        {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (Thread32First(hSnap, &te))
                {
                    do {
                        if (te.th32ThreadID == selfTid) continue;
                        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                        HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hT) { SuspendThread(hT); CloseHandle(hT); }
                    } while (Thread32Next(hSnap, &te));
                }
                CloseHandle(hSnap);
            }
            logToFile("[worker] all threads frozen for init scan");
        }

        for (int attempt = 0; attempt < 120; ++attempt)
        {
            initResult = shimInit();
            diagBuf[0] = '\0';
            shimGetDiag(diagBuf, sizeof(diagBuf));

            if (initResult == 1)
            {
                int methodCheck = shimGetMethods();
                if (methodCheck == 0)
                {
                    static char earlyMsg[64];
                    wsprintfA(earlyMsg, "[worker] init ok but getMethods=0, re-running init (attempt %d)", attempt + 1);
                    logToFile(earlyMsg);
                    initResult = 0;
                    Sleep(500);
                    continue;
                }
                break;
            }

            static char retryMsg[64];
            wsprintfA(retryMsg, "[worker] init attempt %d failed, retrying in 500ms", attempt + 1);
            logToFile(retryMsg);
            Sleep(500);
        }

        // Thaw
        {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (Thread32First(hSnap, &te))
                {
                    do {
                        if (te.th32ThreadID == selfTid) continue;
                        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                        HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hT) { ResumeThread(hT); CloseHandle(hT); }
                    } while (Thread32Next(hSnap, &te));
                }
                CloseHandle(hSnap);
            }
            logToFile("[worker] all threads resumed after init scan");
        }
    }

    diagBuf[0] = '\0';
    shimGetDiag(diagBuf, sizeof(diagBuf));

    logDec("[worker] init result=", (DWORD)initResult);
    logToFile("[worker] diag:");
    logToFile(diagBuf);

    if (initResult != 1)
    {
        static char errBuf[1100];
        lstrcpyA(errBuf, "ERROR:");
        lstrcatA(errBuf, diagBuf);
        pipeWriteStr(errBuf);
        // Don't return — still send READY so host doesn't hang
    }

    // Resume all threads now that patches are applied and init is done
    if (g_suspendedForUnlock)
    {
        // Baseline "before" count for the post-unlock summary. In inject
        // mode this is the real count the host captured right before the
        // user clicked "Unlock All Commands" (see g_baselineCommandCount).
        // A live shimGetMethods() call here would always read 0 since
        // consoleMethods isn't populated yet while threads are still frozen.
        // Proxy mode has no "before" state at all — it's unlocked from the
        // moment it loads — so this stays 0 and the summary format below
        // omits the "(was X, +Y unlocked)" part entirely in that case
        int countBefore = g_proxyMode ? 0 : g_baselineCommandCount;

        g_suspendedForUnlock = false;
        DWORD selfTid = GetCurrentThreadId();

        // Re-run init while still frozen — consoleMethods may already be
        // populated if the game's static initializers ran before our inject
        // If it resolves here we skip the post-resume re-init entirely
        logToFile("[unlock] attempting init while still frozen");
        bool resolvedWhileFrozen = false;
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            shimResetForReinit();
            int reInit = shimInit();
            if (reInit == 1)
            {
                int c = shimGetMethods();
                if (c > 0)
                {
                    static char resolvedMsg[80];
                    wsprintfA(resolvedMsg, "[unlock] init resolved while frozen, getMethods=%d — resuming now", c);
                    logToFile(resolvedMsg);
                    resolvedWhileFrozen = true;
                    break;
                }
            }
            static char frozenWaitMsg[64];
            wsprintfA(frozenWaitMsg, "[unlock] frozen init attempt %d, consoleMethods still empty", attempt + 1);
            logToFile(frozenWaitMsg);
            // Thaw briefly so static initializers can run, then re-freeze
            {
                HANDLE hSnapThaw = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (hSnapThaw != INVALID_HANDLE_VALUE)
                {
                    THREADENTRY32 te{};
                    te.dwSize = sizeof(te);
                    if (Thread32First(hSnapThaw, &te))
                    {
                        do {
                            if (te.th32ThreadID == selfTid) continue;
                            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                            HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                            if (hT) { ResumeThread(hT); CloseHandle(hT); }
                        } while (Thread32Next(hSnapThaw, &te));
                    }
                    CloseHandle(hSnapThaw);
                }
            }
            Sleep(500);
            {
                HANDLE hSnapFreeze = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (hSnapFreeze != INVALID_HANDLE_VALUE)
                {
                    THREADENTRY32 te{};
                    te.dwSize = sizeof(te);
                    if (Thread32First(hSnapFreeze, &te))
                    {
                        do {
                            if (te.th32ThreadID == selfTid) continue;
                            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                            HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                            if (hT) { SuspendThread(hT); CloseHandle(hT); }
                        } while (Thread32Next(hSnapFreeze, &te));
                    }
                    CloseHandle(hSnapFreeze);
                }
            }
        }

        // Now resume — either we resolved while frozen, or we need the game
        // to run its static initializers so post-resume init can find the vector
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hSnap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(hSnap, &te))
            {
                do {
                    if (te.th32ThreadID == selfTid) continue;
                    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (hT) { ResumeThread(hT); CloseHandle(hT); }
                } while (Thread32Next(hSnap, &te));
            }
            CloseHandle(hSnap);
        }
        logToFile("[worker] all threads resumed — game proceeds with patches active");

        // Snapshot the baseline count now (before the background thread reads it)
        // so the "Commands after unlock: X (was Y)" message is accurate
        int countBeforeResume = countBefore;

        // Kick off a background thread to handle post-resume init and poll for
        // method-count stability. The worker continues straight to READY so the
        // output handler and ConsoleOverlay are registered without delay
        struct PollCtx {
            bool resolvedWhileFrozen;
            int  countBefore;
        };
        static PollCtx s_pollCtx;
        s_pollCtx.resolvedWhileFrozen = resolvedWhileFrozen;
        s_pollCtx.countBefore = countBeforeResume;

        HANDLE hPollThread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            PollCtx* ctx = reinterpret_cast<PollCtx*>(param);

            if (!ctx->resolvedWhileFrozen)
            {
                // Post-resume init — wait for the game's static initializers to
                // populate consoleMethods, then re-resolve
                logToFile("[unlock] re-running init post-resume to re-resolve consoleMethods");
                for (int attempt = 0; attempt < 120; ++attempt)
                {
                    shimResetForReinit();
                    int reInit = shimInit();
                    if (reInit == 1)
                    {
                        int c = shimGetMethods();
                        if (c > 0)
                        {
                            static char resolvedMsg[64];
                            wsprintfA(resolvedMsg, "[unlock] post-resume init resolved, getMethods=%d", c);
                            logToFile(resolvedMsg);
                            break;
                        }
                    }
                    static char waitMsg[64];
                    wsprintfA(waitMsg, "[unlock] post-resume init attempt %d, waiting...", attempt + 1);
                    logToFile(waitMsg);
                    Sleep(500);
                }
            }

            // Wait for the game to finish registering all its settings groups
            int countAfter = 0;
            int stable = 0;
            int totalPolls = 0;
            const int kMinPolls = 10;   // at least 5s before we can exit (10 x 500ms)
            const int kStableNeeded = 20; // 20 ticks of no change required
            const int kMaxPolls = 240;  // 120s hard cap

            // On some games (e.g. NFS Payback) the consoleMethods vec is
            // populated lazily — it's empty while frozen and only fills once
            // the game's render/init threads have run for a bit post-resume
            // The post-resume init loop above exits because getMethods()=0,
            // then the poll loop stabilises on 0 and declares done too early
            // Fix: if we're still at 0 after kZeroRetryAfter ticks, fire a
            // fresh resetForReinit+init to re-resolve against the now-live vec
            const int kZeroRetryAfter = 6; // 3s — enough for Payback's loader
            bool zeroPollRetryDone = false;

            for (int i = 0; i < kMaxPolls; ++i)
            {
                Sleep(500);
                ++totalPolls;
                int c = shimGetMethods();

                // If still zero after kZeroRetryAfter ticks, re-run init once
                // This handles games that populate consoleMethods lazily post-resume
                if (c == 0 && !zeroPollRetryDone && totalPolls >= kZeroRetryAfter)
                {
                    logToFile("[unlock] getMethods still 0, re-running init (lazy vec game)");
                    shimResetForReinit();
                    shimInit();
                    zeroPollRetryDone = true;
                    c = shimGetMethods();
                    static char retryMsg[96];
                    wsprintfA(retryMsg, "[unlock] post-reinit getMethods=%d", c);
                    logToFile(retryMsg);
                }

                if (c == countAfter) ++stable;
                else { stable = 0; countAfter = c; }

                static char pollMsg[96];
                wsprintfA(pollMsg, "[unlock] poll %d: count=%d stable=%d", i, c, stable);
                logToFile(pollMsg);

                if (stable >= kStableNeeded && totalPolls >= kMinPolls)
                    break;
            }

            static char afterMsg[128];
            if (g_proxyMode)
            {
                wsprintfA(afterMsg, "OUTPUT:[Unlock]|Commands after unlock: %d", countAfter);
            }
            else
            {
                wsprintfA(afterMsg, "OUTPUT:[Unlock]|Commands after unlock: %d (was %d, +%d unlocked)",
                    countAfter, ctx->countBefore, countAfter - ctx->countBefore);
            }
            pipeWriteRaw(afterMsg, (uint32_t)lstrlenA(afterMsg));

            // Dedicated control packet — signals the host that the unlocked
            // command count has finished stabilizing and it's now safe to
            // request the full command list. Using an explicit packet type
            // here (rather than having the host pattern-match the display
            // text above) avoids depending on exact wording and sidesteps
            // the tag-based debug/result classification that text line
            // goes through on the host
            //
            // Flip the "already stable" flag BEFORE writing the packet, not
            // after — a host connecting on another thread right around this
            // moment should see this as true rather than momentarily false,
            // since the proxy pipe listener's own re-send (see below) is
            // the only other opportunity that host will ever get
            g_unlockPollStable = true;
            static const char kStableMsg[] = "UNLOCK_STABLE:1";
            pipeWriteRaw(kStableMsg, (uint32_t)(sizeof(kStableMsg) - 1));
            return 0;
            }, &s_pollCtx, 0, nullptr);

        if (hPollThread) CloseHandle(hPollThread);
    }

    // Register output handler
    FrostbiteConsole::setLogCallback(nullptr);
    {
        static char readyBuf[64];
        wsprintfA(readyBuf, "READY:0");
        if (!g_proxyMode)
            pipeWriteStr(readyBuf);
        logToFile(readyBuf);
    }

    if (FrostbiteConsole::isReady())
    {
        logToFile("[worker] registering output handler");
        shimAddHandler();
    }
    else
    {
        logToFile("[worker] not ready, skipping output handler");
    }

    // ConsoleOverlay init — run on a background thread so READY: and the
    // command loop (including __LIST__ / __LIST_VARS__) are not blocked
    // waiting for D3D resolution, which can take up to 10 seconds
    HANDLE hOverlayInitThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            for (int overlayAttempt = 0; overlayAttempt < 10; ++overlayAttempt)
            {
                if (overlayAttempt > 0)
                {
                    Sleep(1000);
                    static char retryMsg[64];
                    wsprintfA(retryMsg, "[worker] retrying ConsoleOverlay init (attempt %d/10)", overlayAttempt + 1);
                    logToFile(retryMsg);
                }
                logToFile("[worker] initializing ConsoleOverlay");
                bool overlayOk = false;
                __try
                {
                    ConsoleOverlay::initialize();
                    logToFile("[worker] ConsoleOverlay initialized OK");
                    overlayOk = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    char overlayErr[64];
                    wsprintfA(overlayErr, "[worker] ConsoleOverlay::initialize EXCEPTION=0x%08lX",
                        GetExceptionCode());
                    logToFile(overlayErr);
                }
                if (overlayOk) break;
            }
            return 0;
        }, nullptr, 0, nullptr);
    if (hOverlayInitThread) CloseHandle(hOverlayInitThread);

    // In proxy mode, all pipe I/O for the session is owned exclusively by
    // the dedicated background "[proxy pipe]" listener thread spawned above
    if (!g_proxyMode)
    {
        bool firstConnect = true;
        do
        {
            if (!firstConnect && !g_proxyMode)
            {
                // Clean up the old pipe instance before re-arming
                // Shut down the overlay FIRST so its Present hook stops calling
                // g_pipeOutputHandler before we deregister the output handler
                __try { ConsoleOverlay::shutdown(); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                logToFile("[worker] overlay shut down for re-arm");

                if (FrostbiteConsole::isReady())
                {
                    logToFile("[worker] removing output handler before re-arm");
                    shimRemoveHandler();
                }
                DisconnectNamedPipe(g_hPipe);
                CloseHandle(g_hPipe);
                g_hPipe = INVALID_HANDLE_VALUE;
                logToFile("[worker] host detached — re-arming pipe for next attach");

                bool pipeReady = false;
                while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
                {
                    g_hPipe = CreateNamedPipeA(
                        "\\\\.\\pipe\\FBConsoleBridge",
                        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                        1, 65536, 65536, 5000, nullptr);

                    if (g_hPipe == INVALID_HANDLE_VALUE)
                    {
                        DWORD gle = GetLastError();
                        logDec("[worker] re-arm CreateNamedPipe failed GLE=", gle);
                        if (gle == ERROR_PIPE_BUSY || gle == ERROR_ACCESS_DENIED)
                        {
                            logToFile("[worker] pipe not released yet, retrying in 500ms");
                            Sleep(500);
                            continue;
                        }
                        logToFile("[worker] unrecoverable pipe error — thread exiting");
                        goto worker_exit;
                    }

                    logToFile("[worker] re-armed pipe, waiting for host");
                    if (!ConnectNamedPipe(g_hPipe, nullptr))
                    {
                        DWORD gle = GetLastError();
                        if (gle != ERROR_PIPE_CONNECTED)
                        {
                            logDec("[worker] re-arm ConnectNamedPipe failed GLE=", gle);
                            CloseHandle(g_hPipe);
                            g_hPipe = INVALID_HANDLE_VALUE;
                            Sleep(200);
                            continue;
                        }
                    }
                    logToFile("[worker] host reconnected");
                    pipeReady = true;
                    break;
                }

                if (!pipeReady)
                    goto worker_exit;

                FrostbiteConsole::setLogCallback([](const char* line) { logToFile(line); });
                if (FrostbiteConsole::isReady())
                {
                    logToFile("[worker] re-registering output handler for new session");
                    shimAddHandler();
                }
                {
                    int cmdCount = shimGetMethods();
                    static char readyBuf[64];
                    wsprintfA(readyBuf, "READY:%d", cmdCount);
                    pipeWriteStr(readyBuf);
                }

                // Re-initialize the overlay for the new session — background thread
                // so the command loop is not blocked waiting for D3D resolution
                HANDLE hOverlayReInitThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD
                    {
                        for (int overlayAttempt = 0; overlayAttempt < 10; ++overlayAttempt)
                        {
                            if (overlayAttempt > 0)
                            {
                                Sleep(1000);
                                static char retryMsg[64];
                                wsprintfA(retryMsg, "[worker] retrying ConsoleOverlay re-init (attempt %d/10)", overlayAttempt + 1);
                                logToFile(retryMsg);
                            }
                            logToFile("[worker] re-initializing ConsoleOverlay for new session");
                            bool overlayOk = false;
                            __try
                            {
                                ConsoleOverlay::initialize();
                                logToFile("[worker] ConsoleOverlay re-initialized OK");
                                overlayOk = true;
                            }
                            __except (EXCEPTION_EXECUTE_HANDLER)
                            {
                                char overlayErr[64];
                                wsprintfA(overlayErr, "[worker] ConsoleOverlay::initialize EXCEPTION=0x%08lX",
                                    GetExceptionCode());
                                logToFile(overlayErr);
                            }
                            if (overlayOk) break;
                        }
                        return 0;
                    }, nullptr, 0, nullptr);
                if (hOverlayReInitThread) CloseHandle(hOverlayReInitThread);
            }
            firstConnect = false;

            logToFile("[worker] entering command loop");

            while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
            {
                uint32_t pktLen = pipeReadRaw(g_readBuf, sizeof(g_readBuf));
                if (pktLen == 0)
                {
                    logDec("[worker] pipeRead returned 0 GLE=", GetLastError());
                    break;
                }

                {
                    static char preview[129];
                    int cl = pktLen < 128 ? (int)pktLen : 128;
                    CopyMemory(preview, g_readBuf, cl);
                    preview[cl] = '\0';
                    logToFile("[worker] pkt:");
                }

                if (pktLen <= 4 ||
                    g_readBuf[0] != 'C' || g_readBuf[1] != 'M' ||
                    g_readBuf[2] != 'D' || g_readBuf[3] != ':')
                {
                    logToFile("[worker] unknown packet, ignoring");
                    continue;
                }

                const char* cmd = nullptr;
                int seq = parseCmdSeqAndText(g_readBuf + 4, cmd);
                g_currentCmdSeq = seq; // stays set until the NEXT command parses
                // here — see g_currentCmdSeq's comment

                if (lstrcmpA(cmd, "__LIST__") == 0)
                {
                    logToFile("[worker] handling __LIST__");
                    batchBegin();
                    batchAppend("METHODS:0", 9);

                    // Enumerate directly from the resolved vector — no game lock held
                    int count = 0;
                    const FrostbiteConsole::ConsoleMethod* const* methods =
                        FrostbiteConsole::getMethods(count);

                    if (count == 0)
                    {
                        logToFile("[worker] getMethods returned 0, waiting 2s and retrying");
                        Sleep(2000);
                        methods = FrostbiteConsole::getMethods(count);
                        {
                            static char retryBuf[64];
                            wsprintfA(retryBuf, "[worker] retry getMethods count=%d", count);
                            logToFile(retryBuf);
                        }
                    }

                    if (methods && count > 0 && count < 100000)
                    {
                        // stride is not needed — name is always a raw const char* at +0x08

                        static char lineBuf[512];
                        for (int i = 0; i < count; ++i)
                        {
                            const FrostbiteConsole::ConsoleMethod* m = methods[i];
                            uintptr_t mAddr = reinterpret_cast<uintptr_t>(m);
                            if (mAddr < 0x10000 || mAddr > 0x7FFFFFFFFFFF) continue;

                            // Real ConsoleMethod layout (matches Console.h exactly):
                            //   +0x00  pfn         (void*)       — .text pointer
                            //   +0x08  name        (const char*) — .rdata pointer
                            //   +0x10  groupName   (const char*) — .rdata pointer, may be null
                            //   +0x18  description (const char*) — .rdata pointer, may be null
                            const uint8_t* raw = reinterpret_cast<const uint8_t*>(m);

                            // name is a raw const char* at +0x08
                            // May point into any module, heap, or arena — not just the exe
                            uint64_t namePtr = 0;
                            if (!FrostbiteConsole::safeRead64(
                                const_cast<uint8_t*>(raw + 0x08), &namePtr)) continue;
                            if (namePtr < 0x10000ULL) continue;
                            // Verify readable.
                            MEMORY_BASIC_INFORMATION _nameMbi{};
                            if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(namePtr)),
                                &_nameMbi, sizeof(_nameMbi)) ||
                                _nameMbi.State != MEM_COMMIT ||
                                (_nameMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                            const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(namePtr));

                            // Name must start with a letter or underscore, be at least 2 chars,
                            // and contain only printable ASCII (no parens, no hex digits as names)
                            uint8_t c0u = static_cast<uint8_t>(name[0]);
                            if (!isalpha(c0u) && c0u != '_') continue;
                            if (name[1] == '\0') continue;
                            bool nameValid = true;
                            int nameLen = 0;
                            for (int ni = 0; ni < 128; ++ni) {
                                uint8_t c = static_cast<uint8_t>(name[ni]);
                                if (c == 0) break;
                                if (c < 0x20 || c > 0x7E) { nameValid = false; break; }
                                nameLen++;
                            }
                            if (!nameValid || nameLen < 2) continue;

                            static char safeName[128];
                            (void)lstrcpynA(safeName, name, sizeof(safeName));

                            // Read groupName at +0x10 (may be null)
                            uint64_t groupPtr = 0;
                            FrostbiteConsole::safeRead64(
                                const_cast<uint8_t*>(raw + 0x10), &groupPtr);

                            // Read description at +0x18 (may be null or unreadable)
                            static char safeDesc[256];
                            safeDesc[0] = '\0';
                            {
                                uint64_t descPtr = 0;
                                if (FrostbiteConsole::safeRead64(
                                    const_cast<uint8_t*>(raw + 0x18), &descPtr) &&
                                    descPtr >= 0x10000ULL)
                                {
                                    MEMORY_BASIC_INFORMATION _descMbi{};
                                    if (VirtualQuery(reinterpret_cast<void*>(
                                        static_cast<uintptr_t>(descPtr)),
                                        &_descMbi, sizeof(_descMbi)) &&
                                        _descMbi.State == MEM_COMMIT &&
                                        !(_descMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                                    {
                                        const char* dp = reinterpret_cast<const char*>(
                                            static_cast<uintptr_t>(descPtr));
                                        bool descOk = true;
                                        int dlen = 0;
                                        for (int di = 0; di < 255; ++di) {
                                            uint8_t dc = static_cast<uint8_t>(dp[di]);
                                            if (dc == 0) break;
                                            if (dc < 0x20 || dc > 0x7E) { descOk = false; break; }
                                            dlen++;
                                        }
                                        if (descOk && dlen > 0)
                                            (void)lstrcpynA(safeDesc, dp, sizeof(safeDesc));
                                    }
                                }
                            }

                            int len = 0;
                            if (groupPtr >= 0x10000ULL)
                            {
                                MEMORY_BASIC_INFORMATION _grpMbi{};
                                if (VirtualQuery(reinterpret_cast<void*>(
                                    static_cast<uintptr_t>(groupPtr)),
                                    &_grpMbi, sizeof(_grpMbi)) &&
                                    _grpMbi.State == MEM_COMMIT &&
                                    !(_grpMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                                {
                                    const char* grp = reinterpret_cast<const char*>(
                                        static_cast<uintptr_t>(groupPtr));
                                    uint8_t gc0 = static_cast<uint8_t>(grp[0]);
                                    if (isalpha(gc0) || gc0 == '_')
                                        len = safeDesc[0]
                                        ? _snprintf_s(lineBuf, sizeof(lineBuf),
                                            _TRUNCATE, "METHOD:%s.%s\t%s", grp, safeName, safeDesc)
                                        : _snprintf_s(lineBuf, sizeof(lineBuf),
                                            _TRUNCATE, "METHOD:%s.%s", grp, safeName);
                                }
                            }
                            if (len <= 0)
                                len = safeDesc[0]
                                ? _snprintf_s(lineBuf, sizeof(lineBuf),
                                    _TRUNCATE, "METHOD:%s\t%s", safeName, safeDesc)
                                : _snprintf_s(lineBuf, sizeof(lineBuf),
                                    _TRUNCATE, "METHOD:%s", safeName);
                            if (len > 0)
                                batchAppend(lineBuf, len);
                        }
                    }

                    // Instance methods (WorldRender, audio, etc.)
                    {
                        int instCount = 0, instStride = 0;
                        const uint8_t* instBase = reinterpret_cast<const uint8_t*>(
                            FrostbiteConsole::ConsoleBridge::instance()
                            .getInstanceMethodsBase(instCount, instStride));
                        for (int i = 0; instBase && i < instCount; ++i) {
                            const uint8_t* elem = instBase + i * instStride;
                            uint64_t namePtr = 0, groupPtr = 0;
                            if (!FrostbiteConsole::safeRead64(
                                const_cast<uint8_t*>(elem + 0x00), &namePtr)) continue;
                            if (namePtr < 0x10000ULL) continue;
                            MEMORY_BASIC_INFORMATION _mbi{};
                            if (!VirtualQuery(reinterpret_cast<void*>(
                                static_cast<uintptr_t>(namePtr)), &_mbi, sizeof(_mbi)) ||
                                _mbi.State != MEM_COMMIT ||
                                (_mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                            const char* name = reinterpret_cast<const char*>(
                                static_cast<uintptr_t>(namePtr));
                            uint8_t c0 = static_cast<uint8_t>(name[0]);
                            if (!isalpha(c0) && c0 != '_') continue;
                            if (name[1] == '\0') continue;
                            FrostbiteConsole::safeRead64(
                                const_cast<uint8_t*>(elem + 0x08), &groupPtr);
                            static char instLine[512];
                            if (groupPtr >= 0x10000ULL) {
                                const char* grp = reinterpret_cast<const char*>(
                                    static_cast<uintptr_t>(groupPtr));
                                _snprintf_s(instLine, sizeof(instLine), _TRUNCATE,
                                    "METHOD:%s.%s", grp, name);
                            }
                            else {
                                _snprintf_s(instLine, sizeof(instLine), _TRUNCATE,
                                    "METHOD:%s", name);
                            }
                            batchAppend(instLine, lstrlenA(instLine));
                        }
                    }
                    {
                        char resTerm[32];
                        int resLen = wsprintfA(resTerm, "RESULT:%d:", seq);
                        batchAppend(resTerm, resLen);
                    }
                    batchFlush();
                    logToFile("[worker] list done");
                }
                else if (lstrcmpA(cmd, "__LIST_VARS__") == 0)
                {
                    logToFile("[worker] handling __LIST_VARS__");
                    batchBegin();
                    batchAppend("VARS:0", 6);
                    __try { cpp_listVars(); }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        logHex("[worker] LIST_VARS exception=", GetExceptionCode());
                    }
                    {
                        char resTerm[32];
                        int resLen = wsprintfA(resTerm, "RESULT:%d:", seq);
                        batchAppend(resTerm, resLen);
                    }
                    batchFlush();
                    logToFile("[worker] list vars done");
                    stopDiskLoggingOnceConfirmed();
            }
                else
                {
                    logToFile("[worker] executing:");
                    logToFile(cmd);
                    static char s_execCmd[64 * 1024];
                    (void)lstrcpynA(s_execCmd, cmd, sizeof(s_execCmd));
                    lstrcpynA(g_currentCmdText, s_execCmd, sizeof(g_currentCmdText));
                    g_cmdExceptionLogged = 0;
                    g_execThreadId = GetCurrentThreadId();
                    g_expectingOutput = true;
                    shimExecute(s_execCmd);
                    g_expectingOutput = false;
                    g_execThreadId = 0;
                    // gameOutputHandler fires during shimExecute and sends OUTPUT: packets
                    // Now send the seq-tagged RESULT: terminator
                    pipeWriteResultTerminator(seq);
                    logToFile("[worker] cmd done");
                }
            }

            logToFile("[worker] loop exited");

        } while (!g_proxyMode && InterlockedCompareExchange(&g_running, 1, 1) == 1);
    } // !g_proxyMode

worker_exit:
    // In proxy mode keep the thread alive so the overlay Present hook keeps
    // firing. The command loop exits immediately (pipeReadRaw returns 0 with
    // no pipe), so we need this to prevent the thread from tearing down the
    // overlay right after init
    if (g_proxyMode)
    {
        logToFile("[worker] proxy standalone idle loop — overlay active");
        while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
            Sleep(500);
        logToFile("[worker] proxy idle loop exited");
    }

    // Shut down overlay before removing the output handler
    __try { ConsoleOverlay::shutdown(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (FrostbiteConsole::isReady())
        shimRemoveHandler();

    DisconnectNamedPipe(g_hPipe);
    CloseHandle(g_hPipe);
    g_hPipe = INVALID_HANDLE_VALUE;
    logToFile("[worker] thread done");
    return 0;
}

// findGameDir
static void findGameDir()
{
    HMODULE mods[1] = {};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) && mods[0])
    {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameExA(GetCurrentProcess(), mods[0], exePath, MAX_PATH))
        {
            char* slash = strrchr(exePath, '\\');
            if (slash)
            {
                int len = (int)(slash - exePath);
                CopyMemory(g_gameDir, exePath, len);
                g_gameDir[len] = '\0';
            }
        }
    }
}

// DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        // Install first (call order 1) so it sees exceptions before any
        // other vectored/frame-based handler in the process, including the
        // engine's own — see cmdExceptionVectoredHandler's comment
        AddVectoredExceptionHandler(1, cmdExceptionVectoredHandler);

        {
            char exePath[MAX_PATH] = {};
            HMODULE mods[1] = {};
            DWORD needed = 0;
            if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed) && mods[0])
                GetModuleFileNameExA(GetCurrentProcess(), mods[0], exePath, MAX_PATH);
            const char* exeBase = strrchr(exePath, '\\');
            exeBase = exeBase ? exeBase + 1 : exePath;
            if (lstrcmpiA(exeBase, "InitfsTools.exe") == 0)
                return TRUE; // loaded by our own tool, skip everything
        }

        findGameDir();

        // Proxy detection
        {
            char dllPath[MAX_PATH] = {};
            GetModuleFileNameA(hModule, dllPath, MAX_PATH);
            char* slash = strrchr(dllPath, '\\');
            if (slash) *slash = '\0';
            if (g_gameDir[0] && lstrcmpiA(dllPath, g_gameDir) == 0)
            {
                g_proxyMode = true;
                g_suspendedForUnlock = true;
                // No flag file needed in proxy mode — always unlock
            }
        }

        // Truncate log file fresh on each inject (only when disk logging is enabled)
        if (g_enableDiskLog && g_gameDir[0])
        {
            char logPath[MAX_PATH];
            lstrcpyA(logPath, g_gameDir);
            lstrcatA(logPath, "\\FBConsoleBridge_log.txt");
            HANDLE hLog = CreateFileA(logPath, GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
        }

        logToFile("=== FBConsoleBridge DLL_PROCESS_ATTACH ===");
        logToFile(g_gameDir[0] ? g_gameDir : "(gameDir unknown)");
        // Explicit mode marker
        logToFile(g_proxyMode
            ? "[DllMain] mode: PROXY (dxgi.dll loaded from game install dir — always-unlock, no host pipe wait)"
            : "[DllMain] mode: INJECT (loaded via remote LoadLibraryA into a running process)");

        // Startup unlock
        // If the host wrote FBConsole_unlock.flag next to the DLL, apply both
        // patches immediately (before the game registers its settings groups)
        // and then delete the flag so the next restart is clean
        //
        // Patch 1: Nop(0x140235C2E, 6) — Enable All Console Commands
        // Patch 2: Hook registerSettingsGroup to force exposeToConsole=true
        // 
        // Proxy mode never uses the restart-flag handshake at all — it's
        // already unconditionally unlocked via g_suspendedForUnlock=true
        // in the proxy-detection block above. Checking for/logging about
        // FBConsole_unlock.flag here only makes sense for the inject flow,
        // where the host writes that flag before killing/relaunching the
        // game. Skip the whole check in proxy mode so the log doesn't
        // print a misleading "no unlock flag — normal launch" line for a
        // mechanism that was never in play to begin with
        bool flagExists = false;
        if (!g_proxyMode)
        {
            // Locate the DLL's own directory to find the flag file
            // The DLL sits next to initfstools.exe, not in the game dir
            char dllDir[MAX_PATH] = {};
            if (GetModuleFileNameA(hModule, dllDir, MAX_PATH))
            {
                char* slash = strrchr(dllDir, '\\');
                if (slash) slash[0] = '\0';
            }

            char flagPath[MAX_PATH];
            lstrcpyA(flagPath, dllDir);
            lstrcatA(flagPath, "\\FBConsole_unlock.flag");

            DWORD flagAttrib = GetFileAttributesA(flagPath);
            flagExists = (flagAttrib != INVALID_FILE_ATTRIBUTES &&
                !(flagAttrib & FILE_ATTRIBUTE_DIRECTORY));

            if (flagExists)
            {
                g_suspendedForUnlock = true;

                // Read the baseline command count the host wrote into the flag
                // file before restarting — this is the real "before" number
                {
                    HANDLE hFlag = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFlag != INVALID_HANDLE_VALUE)
                    {
                        char buf[32] = {};
                        DWORD r = 0;
                        if (ReadFile(hFlag, buf, sizeof(buf) - 1, &r, nullptr) && r > 0)
                        {
                            buf[r] = '\0';
                            g_baselineCommandCount = parseIntA(buf);
                        }
                        CloseHandle(hFlag);
                    }
                }

                logToFile("[startup] FBConsole_unlock.flag found — will apply patches after init resolves");

                // Delete the flag now so subsequent launches are clean regardless
                DeleteFileA(flagPath);
                logToFile("[startup] flag deleted");
            }
            else
            {
                logToFile("[startup] no unlock flag — normal launch");
            }
        }

        // Early patch application
        // If the unlock flag was set, apply the console unlock patches RIGHT NOW
        // in DLL_PROCESS_ATTACH before any game code runs. shimApplyUnlock only
        // needs the module to be mapped — it does its own pattern scan and does
        // not depend on FrostbiteConsole::init(). This ensures patches are active
        // before the game's static initializers register any settings groups
        if (g_suspendedForUnlock)
        {
            logToFile(g_proxyMode
                ? "[DllMain] applying unlock patches immediately (proxy mode — always unlocked)"
                : "[DllMain] applying unlock patches immediately at attach (post-restart unlock flag)");
            int earlyUnlock = shimApplyUnlock();
            static char earlyMsg[64];
            wsprintfA(earlyMsg, "[DllMain] early shimApplyUnlock result=%d", earlyUnlock);
            logToFile(earlyMsg);
            if (earlyUnlock != 1)
            {
                // Pattern not found yet (too early for this image) — fall back
                // to the worker thread path which retries after init resolves
                logToFile("[DllMain] early patch failed, worker thread will retry");
            }
        }

        InterlockedExchange(&g_running, 1);
        g_thread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (!g_thread)
        {
            logDec("[DllMain] CreateThread failed GLE=", GetLastError());
            return FALSE;
        }
        logToFile("[DllMain] worker thread created OK");
        break;
    }

    case DLL_PROCESS_DETACH:
        logToFile("[DllMain] DLL_PROCESS_DETACH");
        InterlockedExchange(&g_running, 0);
        if (g_hPipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(g_hPipe);
            CloseHandle(g_hPipe);
            g_hPipe = INVALID_HANDLE_VALUE;
        }
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 2000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        logToFile("[DllMain] detach done");
        break;
    }
    return TRUE;
}