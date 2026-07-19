#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCKAPI_
#include <windows.h>
#include <winsock2.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <wintrust.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wintrust.lib")

// The FC/FIFA dlls have built in checks to allow these games to boot.
// The source for these are not public, but you can reverse them further.
// FC24 now uses FC25's protection - reverse FC25.dll to bypass this.
// These will eventually be replaced with our own in-house version
// when we understand more about what exactly these dlls do to bypass.
//
// Generic path (non-FC games): IAT patches CryptQueryObject + connect
// on the exe and all loaded modules, applied from a background thread.

static HMODULE hOriginalWinHttp = NULL;

// ── Logging ───────────────────────────────────────────────────────────────────

static HANDLE g_hLog = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;

// Forward-declared here because Log_Init references them before the connect
// block where they are fully defined
static CRITICAL_SECTION g_spoofedSocketsCs;
static bool g_spoofedSocketsCsInit = false;

static void Log_Init()
{
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&g_spoofedSocketsCs);
    g_spoofedSocketsCsInit = true;

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char logPath[MAX_PATH];
    sprintf_s(logPath, MAX_PATH, "%swinhttp_bypass.log", exePath);

    g_hLog = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void Log(const char* fmt, ...)
{
    if (g_hLog == INVALID_HANDLE_VALUE) return;

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(msg, sizeof(msg) - 3, fmt, args);
    va_end(args);
    if (len < 0) len = 0;
    msg[len++] = '\r';
    msg[len++] = '\n';
    msg[len] = '\0';

    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamped[1152];
    int slen = sprintf_s(stamped, sizeof(stamped),
        "[%02d:%02d:%02d.%03d] %s",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    EnterCriticalSection(&g_logCs);
    DWORD written = 0;
    WriteFile(g_hLog, stamped, (DWORD)slen, &written, nullptr);
    LeaveCriticalSection(&g_logCs);
}

// ── IAT hook helpers ──────────────────────────────────────────────────────────

static bool PatchIATEntry(HMODULE hMod, const char* targetDll,
    const char* funcName, ULONGLONG hookAddr)
{
    if (!hMod) return false;
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)hMod;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    PIMAGE_NT_HEADERS pNT = (PIMAGE_NT_HEADERS)((BYTE*)hMod + pDos->e_lfanew);
    if (pNT->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD importRVA = pNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRVA) return false;

    PIMAGE_IMPORT_DESCRIPTOR pImport = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + importRVA);
    for (; pImport->Name; pImport++)
    {
        const char* dllName = (const char*)((BYTE*)hMod + pImport->Name);
        if (_stricmp(dllName, targetDll) != 0) continue;

        PIMAGE_THUNK_DATA pOrig = (PIMAGE_THUNK_DATA)((BYTE*)hMod + pImport->OriginalFirstThunk);
        PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)((BYTE*)hMod + pImport->FirstThunk);

        for (; pOrig->u1.AddressOfData; pOrig++, pThunk++)
        {
            if (pOrig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            PIMAGE_IMPORT_BY_NAME pName =
                (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + pOrig->u1.AddressOfData);
            if (strcmp((char*)pName->Name, funcName) == 0)
            {
                DWORD old = 0;
                VirtualProtect(&pThunk->u1.Function, sizeof(ULONGLONG),
                    PAGE_EXECUTE_READWRITE, &old);
                pThunk->u1.Function = hookAddr;
                VirtualProtect(&pThunk->u1.Function, sizeof(ULONGLONG), old, &old);
                return true;
            }
        }
    }
    return false;
}

// ── connect block ─────────────────────────────────────────────────────────────

typedef int (WSAAPI* PFN_connect)(SOCKET, const struct sockaddr*, int);
static PFN_connect Real_connect = nullptr;

// Spoofed-socket tracking — sockets where we faked connect success so that
// subsequent send/recv can return plausible network errors instead of WSAENOTCONN
#define MAX_SPOOFED_SOCKETS 32
static SOCKET g_spoofedSockets[MAX_SPOOFED_SOCKETS];
static int    g_spoofedSocketCount = 0;

static void SpoofedSockets_Add(SOCKET s)
{
    if (!g_spoofedSocketsCsInit) return;
    EnterCriticalSection(&g_spoofedSocketsCs);
    if (g_spoofedSocketCount < MAX_SPOOFED_SOCKETS)
        g_spoofedSockets[g_spoofedSocketCount++] = s;
    LeaveCriticalSection(&g_spoofedSocketsCs);
}

static bool SpoofedSockets_Contains(SOCKET s)
{
    if (!g_spoofedSocketsCsInit) return false;
    EnterCriticalSection(&g_spoofedSocketsCs);
    bool found = false;
    for (int i = 0; i < g_spoofedSocketCount; i++)
        if (g_spoofedSockets[i] == s) { found = true; break; }
    LeaveCriticalSection(&g_spoofedSocketsCs);
    return found;
}

// Defined after CallThroughConnectTrampoline — see below
static int WSAAPI Hooked_connect(SOCKET s, const struct sockaddr* name, int namelen);

// Defined after CallThroughTrampoline — see below
static BOOL WINAPI Hooked_CryptQueryObject(
    DWORD, const void*, DWORD, DWORD, DWORD,
    DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);

// ── CryptQueryObject spoof ────────────────────────────────────────────────────

typedef BOOL(WINAPI* PFN_CryptQueryObject)(
    DWORD, const void*, DWORD, DWORD, DWORD,
    DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);
static PFN_CryptQueryObject Real_CryptQueryObject = nullptr;

// Inline trampoline for early CryptQueryObject hook (installed in DllMain,
// before Activation64.dll's DllMain runs and Detours commits its inline patches).
// We overwrite the first 14 bytes of crypt32!CryptQueryObject with an
// absolute indirect JMP [rip+0] and save the original bytes for the trampoline.
static BYTE  g_CryptOrig14[14] = {};
static BYTE  g_CryptTramp[32] = {};
static bool  g_earlyHookInstalled = false;

// Same treatment for ws2_32!connect — AC resolves it via GetProcAddress
// and stores the pointer internally, so IAT patches never reach it.
static BYTE  g_ConnectOrig14[14] = {};
static BYTE  g_ConnectTramp[32] = {};
static bool  g_earlyConnectHookInstalled = false;

static BYTE g_SendOrig14[14] = {};
static BYTE g_SendTramp[32] = {};
static bool g_earlySendHookInstalled = false;

// Trampoline caller — jumps into the saved original prologue bytes then back
// into crypt32 past our hook patch. Cast and call as the real function.
static BOOL CallThroughTrampoline(
    DWORD dwObjectType, const void* pvObject,
    DWORD dwExpectedContentTypeFlags, DWORD dwExpectedFormatTypeFlags,
    DWORD dwFlags, DWORD* pdwMsgAndCertEncodingType,
    DWORD* pdwContentType, DWORD* pdwFormatType,
    HCERTSTORE* phCertStore, HCRYPTMSG* phMsg, const void** ppvContext)
{
    auto fn = (PFN_CryptQueryObject)(void*)g_CryptTramp;
    return fn(dwObjectType, pvObject,
        dwExpectedContentTypeFlags, dwExpectedFormatTypeFlags,
        dwFlags, pdwMsgAndCertEncodingType,
        pdwContentType, pdwFormatType,
        phCertStore, phMsg, ppvContext);
}

static int CallThroughConnectTrampoline(SOCKET s, const struct sockaddr* name, int namelen)
{
    auto fn = (PFN_connect)(void*)g_ConnectTramp;
    return fn(s, name, namelen);
}

static int WSAAPI Hooked_connect(SOCKET s, const struct sockaddr* name, int namelen)
{
    if (name && namelen >= (int)sizeof(struct sockaddr_in))
    {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)name;
        BYTE* ip = (BYTE*)&sin->sin_addr;
        Log("Hooked_connect: intercepted connect to %u.%u.%u.%u:%u (socket=%llu)",
            ip[0], ip[1], ip[2], ip[3], ntohs(sin->sin_port), (ULONG64)s);
    }
    else
    {
        Log("Hooked_connect: intercepted connect (socket=%llu)", (ULONG64)s);
    }

    if (name && namelen >= (int)sizeof(struct sockaddr_in))
    {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)name;
        DWORD addr = ntohl(sin->sin_addr.s_addr);

        // Allow loopback (127.x.x.x) and unspecified (0.0.0.0) —
        // the AC uses 0.0.0.0 to connect to its local background service.
        // Blocking these kills the AC before it finishes initialising.
        if ((addr >> 24) == 127 || addr == 0)
        {
            Log("Hooked_connect: allowing local address %u.%u.%u.%u:%u",
                (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
                (addr >> 8) & 0xFF, addr & 0xFF,
                ntohs(sin->sin_port));

            if (addr == 0)
            {
                // The AC connects to 0.0.0.0:port to reach its local background service.
                // If that service isn't running, the real connect returns WSAECONNREFUSED
                // which the AC treats as fatal. Instead we spoof success (return 0) —
                // subsequent send/recv on this socket will fail with WSAENOTCONN which
                // EA's AC handles as a degraded/offline condition rather than aborting.
                Log("Hooked_connect: spoofing success for 0.0.0.0:%u (background service offline)",
                    ntohs(sin->sin_port));
                SpoofedSockets_Add(s);
                WSASetLastError(0);
                return 0;
            }

            return g_earlyConnectHookInstalled
                ? CallThroughConnectTrampoline(s, name, namelen)
                : 0;
        }
    }

    WSASetLastError(WSAECONNREFUSED);
    Log("Hooked_connect: returning SOCKET_ERROR/WSAECONNREFUSED");
    return SOCKET_ERROR;
}

static int WSAAPI Hooked_send(SOCKET s, const char* buf, int len, int flags)
{
    if (SpoofedSockets_Contains(s))
    {
        Log("Hooked_send: socket=%llu is spoofed, returning WSAENETDOWN", (ULONG64)s);
        WSASetLastError(WSAENETDOWN);
        return SOCKET_ERROR;
    }
    if (g_earlySendHookInstalled)
    {
        typedef int (WSAAPI* PFN_send)(SOCKET, const char*, int, int);
        auto fn = (PFN_send)(void*)g_SendTramp;
        return fn(s, buf, len, flags);
    }
    typedef int (WSAAPI* PFN_send)(SOCKET, const char*, int, int);
    static PFN_send real = nullptr;
    if (!real) { HMODULE h = GetModuleHandleA("ws2_32.dll"); if (h) real = (PFN_send)GetProcAddress(h, "send"); }
    return real ? real(s, buf, len, flags) : SOCKET_ERROR;
}

static int WSAAPI Hooked_recv(SOCKET s, char* buf, int len, int flags)
{
    if (SpoofedSockets_Contains(s))
    {
        Log("Hooked_recv: socket=%llu is spoofed, returning WSAENETDOWN", (ULONG64)s);
        WSASetLastError(WSAENETDOWN);
        return SOCKET_ERROR;
    }
    typedef int (WSAAPI* PFN_recv)(SOCKET, char*, int, int);
    static PFN_recv real = nullptr;
    if (!real)
    {
        HMODULE h = GetModuleHandleA("ws2_32.dll");
        if (h) real = (PFN_recv)GetProcAddress(h, "recv");
    }
    return real ? real(s, buf, len, flags) : SOCKET_ERROR;
}

static void InstallEarlySendHook()
{
    HMODULE hWs2 = GetModuleHandleA("ws2_32.dll");
    if (!hWs2) hWs2 = LoadLibraryA("ws2_32.dll");
    if (!hWs2) { Log("EarlyHook: ws2_32.dll not found for send"); return; }

    BYTE* target = (BYTE*)GetProcAddress(hWs2, "send");
    if (!target) { Log("EarlyHook: send not found"); return; }

    Log("EarlyHook: send at %p, prologue: "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X",
        (void*)target,
        target[0], target[1], target[2], target[3],
        target[4], target[5], target[6], target[7],
        target[8], target[9], target[10], target[11],
        target[12], target[13]);

    memcpy(g_SendOrig14, target, 14);
    memcpy(g_SendTramp, target, 14);

    BYTE* jmpBack = g_SendTramp + 14;
    jmpBack[0] = 0xFF; jmpBack[1] = 0x25;
    jmpBack[2] = jmpBack[3] = jmpBack[4] = jmpBack[5] = 0x00;
    uintptr_t resumeAddr = (uintptr_t)(target + 14);
    memcpy(jmpBack + 6, &resumeAddr, 8);

    DWORD old = 0;
    VirtualProtect(g_SendTramp, sizeof(g_SendTramp), PAGE_EXECUTE_READWRITE, &old);

    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old);
    uintptr_t hookAddr = (uintptr_t)Hooked_send;
    target[0] = 0xFF; target[1] = 0x25;
    target[2] = target[3] = target[4] = target[5] = 0x00;
    memcpy(target + 6, &hookAddr, 8);
    VirtualProtect(target, 14, old, &old);

    g_earlySendHookInstalled = true;
    Log("EarlyHook: installed inline hook on send, trampoline at %p", (void*)g_SendTramp);
}

static void InstallEarlyConnectHook()
{
    HMODULE hWs2 = GetModuleHandleA("ws2_32.dll");
    if (!hWs2) hWs2 = LoadLibraryA("ws2_32.dll");
    if (!hWs2) { Log("EarlyHook: ws2_32.dll not found"); return; }

    BYTE* target = (BYTE*)GetProcAddress(hWs2, "connect");
    if (!target) { Log("EarlyHook: connect not found"); return; }

    Log("EarlyHook: connect at %p", (void*)target);
    Log("EarlyHook: connect prologue bytes: "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X",
        target[0], target[1], target[2], target[3],
        target[4], target[5], target[6], target[7],
        target[8], target[9], target[10], target[11],
        target[12], target[13], target[14], target[15]);

    memcpy(g_ConnectOrig14, target, 14);
    memcpy(g_ConnectTramp, target, 14);

    BYTE* jmpBack = g_ConnectTramp + 14;
    jmpBack[0] = 0xFF; jmpBack[1] = 0x25;
    jmpBack[2] = 0x00; jmpBack[3] = 0x00; jmpBack[4] = 0x00; jmpBack[5] = 0x00;
    uintptr_t resumeAddr = (uintptr_t)(target + 14);
    memcpy(jmpBack + 6, &resumeAddr, 8);

    DWORD old = 0;
    VirtualProtect(g_ConnectTramp, sizeof(g_ConnectTramp), PAGE_EXECUTE_READWRITE, &old);

    DWORD old2 = 0;
    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old2);

    uintptr_t hookAddr = (uintptr_t)Hooked_connect;
    target[0] = 0xFF; target[1] = 0x25;
    target[2] = 0x00; target[3] = 0x00; target[4] = 0x00; target[5] = 0x00;
    memcpy(target + 6, &hookAddr, 8);

    VirtualProtect(target, 14, old2, &old2);

    g_earlyConnectHookInstalled = true;
    Log("EarlyHook: installed inline hook on connect, trampoline at %p", (void*)g_ConnectTramp);
}

static void InstallEarlyCryptHook()
{
    HMODULE hCrypt = GetModuleHandleA("crypt32.dll");
    if (!hCrypt) hCrypt = LoadLibraryA("crypt32.dll");
    if (!hCrypt) { Log("EarlyHook: crypt32.dll not found"); return; }

    BYTE* target = (BYTE*)GetProcAddress(hCrypt, "CryptQueryObject");
    if (!target) { Log("EarlyHook: CryptQueryObject not found"); return; }

    Log("EarlyHook: CryptQueryObject at %p", (void*)target);
    Log("EarlyHook: CryptQueryObject prologue bytes: "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X",
        target[0], target[1], target[2], target[3],
        target[4], target[5], target[6], target[7],
        target[8], target[9], target[10], target[11],
        target[12], target[13], target[14], target[15]);

    memcpy(g_CryptOrig14, target, 14);
    memcpy(g_CryptTramp, target, 14);

    // JMP [RIP+0] absolute indirect: FF 25 00 00 00 00 <8-byte address>
    BYTE* jmpBack = g_CryptTramp + 14;
    jmpBack[0] = 0xFF; jmpBack[1] = 0x25;
    jmpBack[2] = 0x00; jmpBack[3] = 0x00; jmpBack[4] = 0x00; jmpBack[5] = 0x00;
    uintptr_t resumeAddr = (uintptr_t)(target + 14);
    memcpy(jmpBack + 6, &resumeAddr, 8);

    // Make trampoline buffer executable
    DWORD old = 0;
    VirtualProtect(g_CryptTramp, sizeof(g_CryptTramp), PAGE_EXECUTE_READWRITE, &old);

    // Write JMP [RIP+0] + hook address into crypt32!CryptQueryObject
    DWORD old2 = 0;
    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old2);

    uintptr_t hookAddr = (uintptr_t)Hooked_CryptQueryObject;
    target[0] = 0xFF; target[1] = 0x25;
    target[2] = 0x00; target[3] = 0x00; target[4] = 0x00; target[5] = 0x00;
    memcpy(target + 6, &hookAddr, 8);
    // bytes 14+ are left intact (trampoline jumps back to target+14)

    VirtualProtect(target, 14, old2, &old2);

    g_earlyHookInstalled = true;
    Log("EarlyHook: installed inline hook on CryptQueryObject, trampoline at %p", (void*)g_CryptTramp);
}

static BOOL WINAPI Hooked_CryptQueryObject(
    DWORD dwObjectType, const void* pvObject,
    DWORD dwExpectedContentTypeFlags, DWORD dwExpectedFormatTypeFlags,
    DWORD dwFlags, DWORD* pdwMsgAndCertEncodingType,
    DWORD* pdwContentType, DWORD* pdwFormatType,
    HCERTSTORE* phCertStore, HCRYPTMSG* phMsg, const void** ppvContext)
{
    if (dwObjectType == CERT_QUERY_OBJECT_FILE && pvObject)
        Log("Hooked_CryptQueryObject: FILE='%ls' contentFlags=0x%08X", (LPCWSTR)pvObject, dwExpectedContentTypeFlags);
    else
        Log("Hooked_CryptQueryObject: objectType=%lu pvObject=%p contentFlags=0x%08X", dwObjectType, pvObject, dwExpectedContentTypeFlags);

    // Try the real function via trampoline first
    if (g_earlyHookInstalled)
    {
        BOOL r = CallThroughTrampoline(
            dwObjectType, pvObject,
            dwExpectedContentTypeFlags, dwExpectedFormatTypeFlags,
            dwFlags, pdwMsgAndCertEncodingType,
            pdwContentType, pdwFormatType,
            phCertStore, phMsg, ppvContext);
        if (r) { Log("Hooked_CryptQueryObject: real succeeded"); return TRUE; }
        Log("Hooked_CryptQueryObject: real failed (GLE=%lu), spoofing", GetLastError());
    }

    if (pdwMsgAndCertEncodingType) *pdwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    if (pdwContentType)            *pdwContentType = CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED;
    if (pdwFormatType)             *pdwFormatType = CERT_QUERY_FORMAT_BINARY;
    if (phCertStore)               *phCertStore = NULL;
    if (phMsg)                     *phMsg = NULL;
    if (ppvContext)                *ppvContext = NULL;
    Log("Hooked_CryptQueryObject: returning spoofed TRUE");
    return TRUE;
}

// ── WinVerifyTrust spoof ──────────────────────────────────────────────────────

typedef LONG(WINAPI* PFN_WinVerifyTrust)(HWND, GUID*, WINTRUST_DATA*);
static PFN_WinVerifyTrust Real_WinVerifyTrust = nullptr;

static LONG WINAPI Hooked_WinVerifyTrust(HWND hwnd, GUID* pgActionID, WINTRUST_DATA* pWVTData)
{
    Log("Hooked_WinVerifyTrust: called, spoofing success");
    return 0; // ERROR_SUCCESS
}

// ── Thread helpers ────────────────────────────────────────────────────────────

static void SuspendDllThreads(HMODULE hTarget)
{
    DWORD currentPid = GetCurrentProcessId();
    DWORD currentTid = GetCurrentThreadId();

    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), hTarget, &modInfo, sizeof(modInfo));
    uintptr_t modStart = (uintptr_t)modInfo.lpBaseOfDll;
    uintptr_t modEnd = modStart + modInfo.SizeOfImage;

    Log("SuspendDllThreads: scanning threads for module range [%p, %p)", (void*)modStart, (void*)modEnd);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        Log("SuspendDllThreads: CreateToolhelp32Snapshot failed (GLE=%lu)", GetLastError());
        return;
    }

    int suspended = 0;
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(snap, &te))
    {
        do {
            if (te.th32OwnerProcessID != currentPid) continue;
            if (te.th32ThreadID == currentTid)        continue;

            HANDLE hThread = OpenThread(
                THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                FALSE, te.th32ThreadID);
            if (!hThread) continue;

            PVOID startAddr = nullptr;
            NtQueryInformationThread(hThread, (THREADINFOCLASS)9,
                &startAddr, sizeof(startAddr), nullptr);

            if ((uintptr_t)startAddr >= modStart && (uintptr_t)startAddr < modEnd)
            {
                SuspendThread(hThread);
                Log("SuspendDllThreads: suspended TID %lu (start=%p)", te.th32ThreadID, startAddr);
                suspended++;
            }

            CloseHandle(hThread);
        } while (Thread32Next(snap, &te));
    }

    CloseHandle(snap);
    Log("SuspendDllThreads: done, suspended %d thread(s)", suspended);
}

// ── File watcher ──────────────────────────────────────────────────────────────

struct WatcherContext
{
    char dirPath[MAX_PATH];
    char fileFilter[MAX_PATH];  // empty = delete directory itself when done
};

static DWORD WINAPI FileWatcherThread(LPVOID lpParam)
{
    WatcherContext* ctx = (WatcherContext*)lpParam;
    Log("FileWatcherThread: watching '%s' filter='%s'",
        ctx->dirPath, ctx->fileFilter[0] ? ctx->fileFilter : "(none, delete dir)");

    CreateDirectoryA(ctx->dirPath, nullptr);

    HANDLE hDir = CreateFileA(ctx->dirPath,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE)
    {
        Log("FileWatcherThread: failed to open dir '%s' (GLE=%lu)", ctx->dirPath, GetLastError());
        return 1;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    BYTE buf[4096];

    while (true)
    {
        ResetEvent(ov.hEvent);

        ReadDirectoryChangesW(hDir, buf, sizeof(buf), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_CREATION,
            nullptr, &ov, nullptr);

        if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0)
        {
            DWORD bytesReturned = 0;
            GetOverlappedResult(hDir, &ov, &bytesReturned, FALSE);

            FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)buf;
            do {
                if (fni->Action == FILE_ACTION_ADDED ||
                    fni->Action == FILE_ACTION_RENAMED_NEW_NAME)
                {
                    char fileName[MAX_PATH] = {};
                    WideCharToMultiByte(CP_ACP, 0,
                        fni->FileName, fni->FileNameLength / sizeof(WCHAR),
                        fileName, MAX_PATH, nullptr, nullptr);

                    if (ctx->fileFilter[0] == '\0' || strstr(fileName, ctx->fileFilter))
                    {
                        char fullPath[MAX_PATH];
                        sprintf_s(fullPath, MAX_PATH, "%s\\%s", ctx->dirPath, fileName);
                        SetFileAttributesA(fullPath, FILE_ATTRIBUTE_NORMAL);
                        BOOL deleted = DeleteFileA(fullPath);
                        Log("FileWatcherThread: deleted '%s' result=%d", fullPath, deleted);
                    }
                }

                if (fni->NextEntryOffset == 0) break;
                fni = (FILE_NOTIFY_INFORMATION*)((BYTE*)fni + fni->NextEntryOffset);
            } while (true);
        }

        if (ctx->fileFilter[0] == '\0')
            RemoveDirectoryA(ctx->dirPath);
    }

    CloseHandle(ov.hEvent);
    CloseHandle(hDir);
    return 0;
}

// ── Force-close file handles by path substring ────────────────────────────────

static void ForceCloseFileHandles(const char* filePath)
{
    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    if (!hNtDll) return;

    typedef NTSTATUS(WINAPI* PFN_NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
    typedef NTSTATUS(WINAPI* PFN_NtQueryObject)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    auto NtQuerySystemInformation = (PFN_NtQuerySystemInformation)
        GetProcAddress(hNtDll, "NtQuerySystemInformation");
    auto NtQueryObject = (PFN_NtQueryObject)
        GetProcAddress(hNtDll, "NtQueryObject");
    if (!NtQuerySystemInformation || !NtQueryObject)
    {
        Log("ForceCloseFileHandles: failed to resolve ntdll exports");
        return;
    }

    wchar_t wFilePath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, filePath, -1, wFilePath, MAX_PATH);

    ULONG bufSize = 1024 * 1024;
    BYTE* buf = (BYTE*)malloc(bufSize);
    if (!buf) return;

    ULONG retLen = 0;
    NTSTATUS status;
    while ((status = NtQuerySystemInformation(16, buf, bufSize, &retLen)) == 0xC0000004L)
    {
        bufSize *= 2;
        buf = (BYTE*)realloc(buf, bufSize);
        if (!buf) return;
    }
    if (status != 0) { free(buf); return; }

    DWORD currentPid = GetCurrentProcessId();
    HANDLE hSelf = GetCurrentProcess();

    struct SYSTEM_HANDLE
    {
        ULONG PID; BYTE ObjTypeIndex; BYTE Flags;
        USHORT Handle; PVOID Object; ACCESS_MASK GrantedAccess;
    };
    struct SYSTEM_HANDLE_INFORMATION { ULONG Count; SYSTEM_HANDLE Handles[1]; };

    int closed = 0;
    auto* shi = (SYSTEM_HANDLE_INFORMATION*)buf;
    for (ULONG i = 0; i < shi->Count; i++)
    {
        auto& h = shi->Handles[i];
        if (h.PID != currentPid) continue;

        HANDLE hDup = nullptr;
        if (!DuplicateHandle(hSelf, (HANDLE)(uintptr_t)h.Handle,
            hSelf, &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            continue;

        BYTE nameBuf[1024] = {};
        ULONG nameLen = 0;
        NtQueryObject(hDup, 1, nameBuf, sizeof(nameBuf), &nameLen);
        CloseHandle(hDup);

        UNICODE_STRING* uns = (UNICODE_STRING*)nameBuf;
        if (!uns->Buffer || uns->Length == 0) continue;

        if (wcsstr(uns->Buffer, L"Logs"))
        {
            DuplicateHandle(hSelf, (HANDLE)(uintptr_t)h.Handle,
                hSelf, &hDup, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
            if (hDup) CloseHandle(hDup);
            closed++;
        }
    }

    Log("ForceCloseFileHandles: closed %d handle(s) for '%s'", closed, filePath);
    free(buf);
}

// ── Generic IAT patch helper (patches exe + all loaded modules) ───────────────

static void ApplyIATPatches()
{
    Log("ApplyIATPatches: resolving real function pointers");

    HMODULE hCrypt32 = LoadLibraryA("crypt32.dll");
    if (hCrypt32)
    {
        Real_CryptQueryObject = (PFN_CryptQueryObject)
            GetProcAddress(hCrypt32, "CryptQueryObject");
        Log("ApplyIATPatches: Real_CryptQueryObject = %p", (void*)Real_CryptQueryObject);
    }
    else
    {
        Log("ApplyIATPatches: WARNING - failed to load crypt32.dll (GLE=%lu)", GetLastError());
    }

    HMODULE hWintrust = LoadLibraryA("wintrust.dll");
    if (hWintrust)
    {
        Real_WinVerifyTrust = (PFN_WinVerifyTrust)GetProcAddress(hWintrust, "WinVerifyTrust");
        Log("ApplyIATPatches: Real_WinVerifyTrust = %p", (void*)Real_WinVerifyTrust);
    }
    else
    {
        Log("ApplyIATPatches: WARNING - failed to load wintrust.dll (GLE=%lu)", GetLastError());
    }

    HMODULE hWs2 = GetModuleHandleA("ws2_32.dll");
    if (!hWs2) hWs2 = LoadLibraryA("ws2_32.dll");
    if (hWs2)
    {
        Real_connect = (PFN_connect)GetProcAddress(hWs2, "connect");
        Log("ApplyIATPatches: Real_connect = %p", (void*)Real_connect);
    }
    else
    {
        Log("ApplyIATPatches: WARNING - failed to get ws2_32.dll (GLE=%lu)", GetLastError());
    }

    HMODULE hExe = GetModuleHandleA(NULL);
    int patchCount = 0;

    // Patch the main exe
    if (PatchIATEntry(hExe, "ws2_32.dll", "send", (ULONGLONG)Hooked_send))
    {
        Log("ApplyIATPatches: patched send in exe"); patchCount++;
    }
    if (PatchIATEntry(hExe, "ws2_32.dll", "recv", (ULONGLONG)Hooked_recv))
    {
        Log("ApplyIATPatches: patched recv in exe"); patchCount++;
    }
    if (PatchIATEntry(hExe, "crypt32.dll", "CryptQueryObject", (ULONGLONG)Hooked_CryptQueryObject))
    {
        Log("ApplyIATPatches: patched CryptQueryObject in exe");
        patchCount++;
    }
    if (PatchIATEntry(hExe, "ws2_32.dll", "connect", (ULONGLONG)Hooked_connect))
    {
        Log("ApplyIATPatches: patched connect in exe");
        patchCount++;
    }
    if (PatchIATEntry(hExe, "wintrust.dll", "WinVerifyTrust", (ULONGLONG)Hooked_WinVerifyTrust))
    {
        Log("ApplyIATPatches: patched WinVerifyTrust in exe");
        patchCount++;
    }

    // Patch all other loaded modules
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32 me = { sizeof(me) };
        if (Module32First(hSnap, &me))
        {
            do {
                if (me.hModule == hExe) continue;

                bool c = PatchIATEntry(me.hModule, "crypt32.dll", "CryptQueryObject",
                    (ULONGLONG)Hooked_CryptQueryObject);
                bool k = PatchIATEntry(me.hModule, "ws2_32.dll", "connect",
                    (ULONGLONG)Hooked_connect);
                bool w = PatchIATEntry(me.hModule, "wintrust.dll", "WinVerifyTrust",
                    (ULONGLONG)Hooked_WinVerifyTrust);
                bool sn = PatchIATEntry(me.hModule, "ws2_32.dll", "send",
                    (ULONGLONG)Hooked_send);
                bool rv = PatchIATEntry(me.hModule, "ws2_32.dll", "recv",
                    (ULONGLONG)Hooked_recv);

                if (c || k || w || sn || rv)
                {
                    Log("ApplyIATPatches: patched module '%s' (Crypt=%d connect=%d WinVerifyTrust=%d send=%d recv=%d)",
                        me.szModule, c, k, w, sn, rv);
                    patchCount++;
                }
            } while (Module32Next(hSnap, &me));
        }
        CloseHandle(hSnap);
    }
    else
    {
        Log("ApplyIATPatches: WARNING - module snapshot failed (GLE=%lu)", GetLastError());
    }

    Log("ApplyIATPatches: done, %d patch(es) applied", patchCount);
}

// ── AC DLL internal slot patcher ──────────────────────────────────────────────
// Overwrites the function pointer slots the AC DLL's Detours trampoline
// dispatcher resolves at call time. IAT patches are irrelevant after Detours
// has committed its inline patches; these slots are the only hooks that stick.
//
// RVAs sourced from IDA disassembly of EAAntiCheat.GameServiceLauncher.dll:
//   qword_180008710  = CryptQueryObject pointer (RVA 0x8710)
//   off_180008048    = connect pointer           (RVA 0x8048)

static void PatchACDllSlots(HMODULE hAC)
{
    uintptr_t base = (uintptr_t)hAC;

    // RVA 0x8710 — CryptQueryObject slot
    auto* pCrypt = (PFN_CryptQueryObject*)(base + 0x8710);
    Log("PatchACDllSlots: CryptQueryObject slot (RVA 0x8710, VA=%p) currently holds %p (expect ~crypt32 range)",
        (void*)pCrypt, (void*)*pCrypt);
    DWORD old = 0;
    VirtualProtect(pCrypt, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *pCrypt = Hooked_CryptQueryObject;
    VirtualProtect(pCrypt, sizeof(void*), old, &old);
    Log("PatchACDllSlots: wrote Hooked_CryptQueryObject -> slot at RVA 0x8710");

    // RVA 0x8048 — connect slot
    auto* pConnect = (PFN_connect*)(base + 0x8048);
    Log("PatchACDllSlots: connect slot (RVA 0x8048, VA=%p) currently holds %p (expect ~ws2_32 range)",
        (void*)pConnect, (void*)*pConnect);
    VirtualProtect(pConnect, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *pConnect = Hooked_connect;
    VirtualProtect(pConnect, sizeof(void*), old, &old);
    Log("PatchACDllSlots: wrote Hooked_connect -> slot at RVA 0x8048");
}

// ── FC/FIFA path ──────────────────────────────────────────────────────────────
// Only runs for FC26.exe, FC25.exe, FC24.exe, FIFA23.exe.
// Loads the companion bypass DLL, watches for telemetry files to delete,
// waits for the game window, then suspends the anticheat DLL's threads.

static void RunFCPath(const char* exePath, const char* exeBase, const char* fcDllName)
{
    char dllPath[MAX_PATH];
    sprintf_s(dllPath, MAX_PATH, "%s%s", exePath, fcDllName);
    Log("FC path: loading bypass DLL '%s'", dllPath);

    HMODULE hFCLE = LoadLibraryA(dllPath);
    if (!hFCLE)
    {
        Log("FC path: LoadLibraryA FAILED (GLE=%lu) — cannot continue FC boot", GetLastError());
        return;
    }
    Log("FC path: bypass DLL loaded at %p", (void*)hFCLE);

    // Watch the game directory for .json telemetry files and delete them on sight
    static WatcherContext ctxRoot = {};
    sprintf_s(ctxRoot.dirPath, MAX_PATH, "%s", exePath);
    size_t len = strlen(ctxRoot.dirPath);
    if (len > 0 && ctxRoot.dirPath[len - 1] == '\\')
        ctxRoot.dirPath[len - 1] = '\0';
    sprintf_s(ctxRoot.fileFilter, MAX_PATH, ".json");
    CreateThread(nullptr, 0, FileWatcherThread, &ctxRoot, 0, nullptr);
    Log("FC path: started .json watcher on '%s'", ctxRoot.dirPath);

    // Watch the Logs directory and delete everything written there
    static WatcherContext ctxLogs = {};
    sprintf_s(ctxLogs.dirPath, MAX_PATH, "%sLogs", exePath);
    ctxLogs.fileFilter[0] = '\0';
    CreateThread(nullptr, 0, FileWatcherThread, &ctxLogs, 0, nullptr);
    Log("FC path: started Logs watcher on '%s'", ctxLogs.dirPath);

    // Wait for the game's main window to appear (confirms the process is fully up)
    Log("FC path: waiting for game window (exe='%s')", exeBase);
    HWND hGameWnd = NULL;

    struct FindCtx { HWND* pHwnd; const char* exeName; };
    static FindCtx fctx;
    fctx.pHwnd = &hGameWnd;
    fctx.exeName = exeBase;

    while (!hGameWnd)
    {
        Sleep(500);
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* ctx = (FindCtx*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc)
            {
                char name[MAX_PATH] = {};
                DWORD sz = MAX_PATH;
                QueryFullProcessImageNameA(hProc, 0, name, &sz);
                CloseHandle(hProc);
                if (strstr(name, ctx->exeName) && IsWindowVisible(hwnd))
                {
                    *ctx->pHwnd = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
            }, (LPARAM)&fctx);
    }
    Log("FC path: game window found (HWND=%p), sleeping 5s before suspend", (void*)hGameWnd);

    Sleep(5000);
    SuspendDllThreads(hFCLE);

    // Force-close and delete any log files the anticheat wrote
    char logsPath[MAX_PATH];
    sprintf_s(logsPath, MAX_PATH, "%sLogs", exePath);
    Log("FC path: cleaning up logs in '%s'", logsPath);

    WIN32_FIND_DATAA fd;
    char searchPath[MAX_PATH];
    sprintf_s(searchPath, MAX_PATH, "%s\\*", logsPath);
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            char fullPath[MAX_PATH];
            sprintf_s(fullPath, MAX_PATH, "%s\\%s", logsPath, fd.cFileName);
            ForceCloseFileHandles(fullPath);
            SetFileAttributesA(fullPath, FILE_ATTRIBUTE_NORMAL);
            BOOL deleted = DeleteFileA(fullPath);
            Log("FC path: deleted log '%s' result=%d", fullPath, deleted);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    BOOL dirRemoved = RemoveDirectoryA(logsPath);
    Log("FC path: removed Logs dir result=%d", dirRemoved);
    Log("FC path: all done");
}

// ── Late AC DLL watcher ───────────────────────────────────────────────────────
// Polls for EAAntiCheat.GameServiceLauncher.dll after the initial IAT sweep,
// in case it loads as a late dependency (common in EA titles where the launcher
// component initialises after the main activation layer).

static DWORD WINAPI LateACWatcherThread(LPVOID)
{
    Log("LateACWatcher: started, polling for EAAntiCheat.GameServiceLauncher.dll");

    // Poll for up to 30 seconds in 250ms increments
    for (int i = 0; i < 120; i++)
    {
        Sleep(250);
        HMODULE hAC = GetModuleHandleA("EAAntiCheat.GameServiceLauncher.dll");
        if (hAC)
        {
            Log("LateACWatcher: found EAAntiCheat.GameServiceLauncher.dll at %p after %dms",
                (void*)hAC, i * 250);

            // Give Detours 500ms to commit its inline patches into this module
            Sleep(500);

            // Re-check handle in case it was unloaded during the sleep
            hAC = GetModuleHandleA("EAAntiCheat.GameServiceLauncher.dll");
            if (!hAC)
            {
                Log("LateACWatcher: module unloaded during sleep, aborting slot patch");
                return 0;
            }

            // Verify the slots look like pointers before writing
            // (sanity check that RVAs match this build)
            uintptr_t base = (uintptr_t)hAC;
            void* cryptSlot = *(void**)(base + 0x8710);
            void* connSlot = *(void**)(base + 0x8048);
            Log("LateACWatcher: pre-patch CryptQueryObject slot=%p (expect crypt32 range ~00007FF8E4E.....)", cryptSlot);
            Log("LateACWatcher: pre-patch connect slot=%p (expect ws2_32 range ~00007FF8E678.....)", connSlot);

            // Only patch if slots look like they're in a plausible DLL range
            // (not instruction bytes masquerading as a pointer)
            uintptr_t cryptAddr = (uintptr_t)cryptSlot;
            uintptr_t connAddr = (uintptr_t)connSlot;
            bool cryptOk = (cryptAddr > 0x00007FF000000000ULL && cryptAddr < 0x00007FFFFFFFFFFFULL);
            bool connOk = (connAddr > 0x00007FF000000000ULL && connAddr < 0x00007FFFFFFFFFFFULL);

            if (cryptOk)
            {
                PatchACDllSlots(hAC);
            }
            else
            {
                Log("LateACWatcher: slot sanity check FAILED — RVAs 0x8710/0x8048 do not hold"
                    " pointer-range values in this build of EAAntiCheat.GameServiceLauncher.dll");
                Log("LateACWatcher: you need to re-derive the correct RVAs from IDA for this version");
            }

            return 0;
        }
    }

    Log("LateACWatcher: EAAntiCheat.GameServiceLauncher.dll never appeared within 30s");
    return 0;
}

// ── Generic path ──────────────────────────────────────────────────────────────
// Runs for every game that is NOT an FC/FIFA title.
// Sleeps briefly to let the process finish loading, then IAT-patches
// CryptQueryObject and connect across the exe and all loaded modules.

static void RunGenericPath()
{
    Log("Generic path: waiting for activation64.dll");

    HMODULE hAC = NULL;
    for (int i = 0; i < 600 && !hAC; i++)
    {
        Sleep(100);
        hAC = GetModuleHandleA("activation64.dll");
    }

    if (!hAC)
    {
        Log("Generic path: activation64.dll not found after 60s, patching anyway");
    }
    else
    {
        Log("Generic path: activation64.dll found at %p, sleeping 500ms for Detours commit", (void*)hAC);
        Sleep(500);
    }

    // Dump every loaded module so we can confirm which binary is the real AC DLL
    // and whether EAAntiCheat.GameServiceLauncher.dll is present under a different name.
    {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (hSnap != INVALID_HANDLE_VALUE)
        {
            Log("Generic path: --- loaded module dump ---");
            MODULEENTRY32 me = { sizeof(me) };
            if (Module32First(hSnap, &me))
            {
                do {
                    Log("  [%p] %s", (void*)me.modBaseAddr, me.szExePath);
                } while (Module32Next(hSnap, &me));
            }
            Log("Generic path: --- end module dump ---");
            CloseHandle(hSnap);
        }
    }

    Log("Generic path: applying IAT patches");
    ApplyIATPatches();

    // If GameServiceLauncher is already up, patch it immediately.
    // Otherwise hand off to the watcher thread which polls for 30s.
    HMODULE hACSlots = GetModuleHandleA("EAAntiCheat.GameServiceLauncher.dll");
    if (hACSlots)
    {
        Log("Generic path: EAAntiCheat.GameServiceLauncher.dll already loaded at %p, patching now", (void*)hACSlots);
        PatchACDllSlots(hACSlots);
    }
    else
    {
        Log("Generic path: EAAntiCheat.GameServiceLauncher.dll not yet loaded, spawning late watcher");
        HANDLE hWatcher = CreateThread(nullptr, 0, LateACWatcherThread, nullptr, 0, nullptr);
        if (hWatcher)
            CloseHandle(hWatcher);
        else
            Log("Generic path: WARNING - failed to create LateACWatcherThread (GLE=%lu)", GetLastError());
    }

    Log("Generic path: done");
}

// ── Main load thread ──────────────────────────────────────────────────────────

static DWORD WINAPI LoadThread(LPVOID)
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char exeFullPath[MAX_PATH];
    GetModuleFileNameA(NULL, exeFullPath, MAX_PATH);
    char* exeSlash = strrchr(exeFullPath, '\\');
    const char* exeBase = exeSlash ? exeSlash + 1 : exeFullPath;

    Log("LoadThread: exe='%s' dir='%s'", exeBase, exePath);

    // Determine if this is an FC/FIFA title
    const char* fcDllName = nullptr;
    if (_stricmp(exeBase, "FC26.exe") == 0) fcDllName = "FC26.dll";
    else if (_stricmp(exeBase, "FC25.exe") == 0) fcDllName = "FC25.dll";
    else if (_stricmp(exeBase, "FC24.exe") == 0) fcDllName = "FC24.dll";
    else if (_stricmp(exeBase, "FIFA23.exe") == 0) fcDllName = "FIFA23.dll";

    if (fcDllName)
    {
        Log("LoadThread: detected FC/FIFA game ('%s') -> FC path with DLL '%s'",
            exeBase, fcDllName);
        RunFCPath(exePath, exeBase, fcDllName);
    }
    else
    {
        Log("LoadThread: not an FC/FIFA game ('%s') -> generic IAT patch path", exeBase);
        RunGenericPath();
    }

    return 0;
}

// ── WinHTTP passthrough exports ───────────────────────────────────────────────

HINTERNET WINAPI WinHttpOpen(LPCWSTR a, DWORD b, LPCWSTR c, LPCWSTR d, DWORD e)
{
    static decltype(&WinHttpOpen) fn = NULL; if (!fn) fn = (decltype(&WinHttpOpen))GetProcAddress(hOriginalWinHttp, "WinHttpOpen"); return fn ? fn(a, b, c, d, e) : NULL;
}

HINTERNET WINAPI WinHttpConnect(HINTERNET a, LPCWSTR b, INTERNET_PORT c, DWORD d)
{
    static decltype(&WinHttpConnect) fn = NULL; if (!fn) fn = (decltype(&WinHttpConnect))GetProcAddress(hOriginalWinHttp, "WinHttpConnect"); return fn ? fn(a, b, c, d) : NULL;
}

HINTERNET WINAPI WinHttpOpenRequest(HINTERNET a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR* f, DWORD g)
{
    static decltype(&WinHttpOpenRequest) fn = NULL; if (!fn) fn = (decltype(&WinHttpOpenRequest))GetProcAddress(hOriginalWinHttp, "WinHttpOpenRequest"); return fn ? fn(a, b, c, d, e, f, g) : NULL;
}

BOOL WINAPI WinHttpCloseHandle(HINTERNET a)
{
    static decltype(&WinHttpCloseHandle) fn = NULL; if (!fn) fn = (decltype(&WinHttpCloseHandle))GetProcAddress(hOriginalWinHttp, "WinHttpCloseHandle"); return fn ? fn(a) : FALSE;
}

BOOL WINAPI WinHttpSendRequest(HINTERNET a, LPCWSTR b, DWORD c, LPVOID d, DWORD e, DWORD f, DWORD_PTR g)
{
    static decltype(&WinHttpSendRequest) fn = NULL; if (!fn) fn = (decltype(&WinHttpSendRequest))GetProcAddress(hOriginalWinHttp, "WinHttpSendRequest"); return fn ? fn(a, b, c, d, e, f, g) : FALSE;
}

BOOL WINAPI WinHttpReceiveResponse(HINTERNET a, LPVOID b)
{
    static decltype(&WinHttpReceiveResponse) fn = NULL; if (!fn) fn = (decltype(&WinHttpReceiveResponse))GetProcAddress(hOriginalWinHttp, "WinHttpReceiveResponse"); return fn ? fn(a, b) : FALSE;
}

BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET a, LPDWORD b)
{
    static decltype(&WinHttpQueryDataAvailable) fn = NULL; if (!fn) fn = (decltype(&WinHttpQueryDataAvailable))GetProcAddress(hOriginalWinHttp, "WinHttpQueryDataAvailable"); return fn ? fn(a, b) : FALSE;
}

BOOL WINAPI WinHttpReadData(HINTERNET a, LPVOID b, DWORD c, LPDWORD d)
{
    static decltype(&WinHttpReadData) fn = NULL; if (!fn) fn = (decltype(&WinHttpReadData))GetProcAddress(hOriginalWinHttp, "WinHttpReadData"); return fn ? fn(a, b, c, d) : FALSE;
}

DWORD WINAPI WinHttpReadDataEx(HINTERNET a, LPVOID b, DWORD c, LPDWORD d, ULONGLONG e, DWORD f, PVOID g)
{
    typedef DWORD(WINAPI* PFN)(HINTERNET, LPVOID, DWORD, LPDWORD, ULONGLONG, DWORD, PVOID); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpReadDataEx"); return fn ? fn(a, b, c, d, e, f, g) : 0;
}

BOOL WINAPI WinHttpWriteData(HINTERNET a, LPCVOID b, DWORD c, LPDWORD d)
{
    static decltype(&WinHttpWriteData) fn = NULL; if (!fn) fn = (decltype(&WinHttpWriteData))GetProcAddress(hOriginalWinHttp, "WinHttpWriteData"); return fn ? fn(a, b, c, d) : FALSE;
}

BOOL WINAPI WinHttpQueryHeaders(HINTERNET a, DWORD b, LPCWSTR c, LPVOID d, LPDWORD e, LPDWORD f)
{
    static decltype(&WinHttpQueryHeaders) fn = NULL; if (!fn) fn = (decltype(&WinHttpQueryHeaders))GetProcAddress(hOriginalWinHttp, "WinHttpQueryHeaders"); return fn ? fn(a, b, c, d, e, f) : FALSE;
}

DWORD WINAPI WinHttpQueryHeadersEx(HINTERNET a, DWORD b, ULONGLONG c, UINT d, LPCWSTR e, LPVOID f, LPDWORD g)
{
    typedef DWORD(WINAPI* PFN)(HINTERNET, DWORD, ULONGLONG, UINT, LPCWSTR, LPVOID, LPDWORD); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpQueryHeadersEx"); return fn ? fn(a, b, c, d, e, f, g) : 0;
}

BOOL WINAPI WinHttpAddRequestHeaders(HINTERNET a, LPCWSTR b, DWORD c, DWORD d)
{
    static decltype(&WinHttpAddRequestHeaders) fn = NULL; if (!fn) fn = (decltype(&WinHttpAddRequestHeaders))GetProcAddress(hOriginalWinHttp, "WinHttpAddRequestHeaders"); return fn ? fn(a, b, c, d) : FALSE;
}

DWORD WINAPI WinHttpAddRequestHeadersEx(HINTERNET a, DWORD b, ULONGLONG c, ULONGLONG d, DWORD e, PWINHTTP_EXTENDED_HEADER f)
{
    static decltype(&WinHttpAddRequestHeadersEx) fn = NULL; if (!fn) fn = (decltype(&WinHttpAddRequestHeadersEx))GetProcAddress(hOriginalWinHttp, "WinHttpAddRequestHeadersEx"); return fn ? fn(a, b, c, d, e, f) : 0;
}

BOOL WINAPI WinHttpSetOption(HINTERNET a, DWORD b, LPVOID c, DWORD d)
{
    static decltype(&WinHttpSetOption) fn = NULL; if (!fn) fn = (decltype(&WinHttpSetOption))GetProcAddress(hOriginalWinHttp, "WinHttpSetOption"); return fn ? fn(a, b, c, d) : FALSE;
}

BOOL WINAPI WinHttpQueryOption(HINTERNET a, DWORD b, LPVOID c, LPDWORD d)
{
    static decltype(&WinHttpQueryOption) fn = NULL; if (!fn) fn = (decltype(&WinHttpQueryOption))GetProcAddress(hOriginalWinHttp, "WinHttpQueryOption"); return fn ? fn(a, b, c, d) : FALSE;
}

BOOL WINAPI WinHttpSetTimeouts(HINTERNET a, int b, int c, int d, int e)
{
    static decltype(&WinHttpSetTimeouts) fn = NULL; if (!fn) fn = (decltype(&WinHttpSetTimeouts))GetProcAddress(hOriginalWinHttp, "WinHttpSetTimeouts"); return fn ? fn(a, b, c, d, e) : FALSE;
}

WINHTTP_STATUS_CALLBACK WINAPI WinHttpSetStatusCallback(HINTERNET a, WINHTTP_STATUS_CALLBACK b, DWORD c, DWORD_PTR d)
{
    static decltype(&WinHttpSetStatusCallback) fn = NULL; if (!fn) fn = (decltype(&WinHttpSetStatusCallback))GetProcAddress(hOriginalWinHttp, "WinHttpSetStatusCallback"); return fn ? fn(a, b, c, d) : WINHTTP_INVALID_STATUS_CALLBACK;
}

BOOL WINAPI WinHttpSetCredentials(HINTERNET a, DWORD b, DWORD c, LPCWSTR d, LPCWSTR e, LPVOID f)
{
    static decltype(&WinHttpSetCredentials) fn = NULL; if (!fn) fn = (decltype(&WinHttpSetCredentials))GetProcAddress(hOriginalWinHttp, "WinHttpSetCredentials"); return fn ? fn(a, b, c, d, e, f) : FALSE;
}

BOOL WINAPI WinHttpQueryAuthSchemes(HINTERNET a, LPDWORD b, LPDWORD c, LPDWORD d)
{
    static decltype(&WinHttpQueryAuthSchemes) fn = NULL; if (!fn) fn = (decltype(&WinHttpQueryAuthSchemes))GetProcAddress(hOriginalWinHttp, "WinHttpQueryAuthSchemes"); return fn ? fn(a, b, c, d) : FALSE;
}

BOOL WINAPI WinHttpCheckPlatform()
{
    static decltype(&WinHttpCheckPlatform) fn = NULL; if (!fn) fn = (decltype(&WinHttpCheckPlatform))GetProcAddress(hOriginalWinHttp, "WinHttpCheckPlatform"); return fn ? fn() : FALSE;
}

BOOL WINAPI WinHttpCrackUrl(LPCWSTR a, DWORD b, DWORD c, LPURL_COMPONENTS d)
{
    static decltype(&WinHttpCrackUrl) fn = NULL; if (!fn) fn = (decltype(&WinHttpCrackUrl))GetProcAddress(hOriginalWinHttp, "WinHttpCrackUrl"); return fn ? fn(a, b, c, d) : FALSE;
}

BOOL WINAPI WinHttpCreateUrl(LPURL_COMPONENTS a, DWORD b, LPWSTR c, LPDWORD d)
{
    static decltype(&WinHttpCreateUrl) fn = NULL; if (!fn) fn = (decltype(&WinHttpCreateUrl))GetProcAddress(hOriginalWinHttp, "WinHttpCreateUrl"); return fn ? fn(a, b, c, d) : FALSE;
}

BOOL WINAPI WinHttpDetectAutoProxyConfigUrl(DWORD a, LPWSTR* b)
{
    static decltype(&WinHttpDetectAutoProxyConfigUrl) fn = NULL; if (!fn) fn = (decltype(&WinHttpDetectAutoProxyConfigUrl))GetProcAddress(hOriginalWinHttp, "WinHttpDetectAutoProxyConfigUrl"); return fn ? fn(a, b) : FALSE;
}

BOOL WINAPI WinHttpGetDefaultProxyConfiguration(WINHTTP_PROXY_INFO* a)
{
    static decltype(&WinHttpGetDefaultProxyConfiguration) fn = NULL; if (!fn) fn = (decltype(&WinHttpGetDefaultProxyConfiguration))GetProcAddress(hOriginalWinHttp, "WinHttpGetDefaultProxyConfiguration"); return fn ? fn(a) : FALSE;
}

BOOL WINAPI WinHttpSetDefaultProxyConfiguration(WINHTTP_PROXY_INFO* a)
{
    static decltype(&WinHttpSetDefaultProxyConfiguration) fn = NULL; if (!fn) fn = (decltype(&WinHttpSetDefaultProxyConfiguration))GetProcAddress(hOriginalWinHttp, "WinHttpSetDefaultProxyConfiguration"); return fn ? fn(a) : FALSE;
}

BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG* a)
{
    static decltype(&WinHttpGetIEProxyConfigForCurrentUser) fn = NULL; if (!fn) fn = (decltype(&WinHttpGetIEProxyConfigForCurrentUser))GetProcAddress(hOriginalWinHttp, "WinHttpGetIEProxyConfigForCurrentUser"); return fn ? fn(a) : FALSE;
}

BOOL WINAPI WinHttpGetProxyForUrl(HINTERNET a, LPCWSTR b, WINHTTP_AUTOPROXY_OPTIONS* c, WINHTTP_PROXY_INFO* d)
{
    static decltype(&WinHttpGetProxyForUrl) fn = NULL; if (!fn) fn = (decltype(&WinHttpGetProxyForUrl))GetProcAddress(hOriginalWinHttp, "WinHttpGetProxyForUrl"); return fn ? fn(a, b, c, d) : FALSE;
}

void WINAPI WinHttpFreeProxyResult(WINHTTP_PROXY_RESULT* a)
{
    typedef void(WINAPI* PFN)(WINHTTP_PROXY_RESULT*); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpFreeProxyResult"); if (fn) fn(a);
}

void WINAPI WinHttpFreeProxyResultEx(WINHTTP_PROXY_RESULT_EX* a)
{
    typedef void(WINAPI* PFN)(WINHTTP_PROXY_RESULT_EX*); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpFreeProxyResultEx"); if (fn) fn(a);
}

void WINAPI WinHttpFreeProxySettings(WINHTTP_PROXY_SETTINGS* a)
{
    typedef void(WINAPI* PFN)(WINHTTP_PROXY_SETTINGS*); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpFreeProxySettings"); if (fn) fn(a);
}

DWORD WINAPI WinHttpFreeProxySettingsEx(WINHTTP_PROXY_SETTINGS_TYPE a, PVOID b)
{
    static decltype(&WinHttpFreeProxySettingsEx) fn = NULL; if (!fn) fn = (decltype(&WinHttpFreeProxySettingsEx))GetProcAddress(hOriginalWinHttp, "WinHttpFreeProxySettingsEx"); return fn ? fn(a, b) : 0;
}

void WINAPI WinHttpFreeQueryConnectionGroupResult(LPVOID a)
{
    typedef void(WINAPI* PFN)(LPVOID); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpFreeQueryConnectionGroupResult"); if (fn) fn(a);
}

DWORD WINAPI WinHttpResetAutoProxy(HINTERNET a, DWORD b)
{
    typedef DWORD(WINAPI* PFN)(HINTERNET, DWORD); static PFN fn = NULL; if (!fn) fn = (PFN)GetProcAddress(hOriginalWinHttp, "WinHttpResetAutoProxy"); return fn ? fn(a, b) : 0;
}

BOOL WINAPI WinHttpTimeFromSystemTime(const SYSTEMTIME* a, LPWSTR b)
{
    static decltype(&WinHttpTimeFromSystemTime) fn = NULL; if (!fn) fn = (decltype(&WinHttpTimeFromSystemTime))GetProcAddress(hOriginalWinHttp, "WinHttpTimeFromSystemTime"); return fn ? fn(a, b) : FALSE;
}

BOOL WINAPI WinHttpTimeToSystemTime(LPCWSTR a, SYSTEMTIME* b)
{
    static decltype(&WinHttpTimeToSystemTime) fn = NULL; if (!fn) fn = (decltype(&WinHttpTimeToSystemTime))GetProcAddress(hOriginalWinHttp, "WinHttpTimeToSystemTime"); return fn ? fn(a, b) : FALSE;
}

HRESULT WINAPI DllCanUnloadNow()
{
    static HRESULT(WINAPI * fn)() = NULL; if (!fn) fn = (HRESULT(WINAPI*)())GetProcAddress(hOriginalWinHttp, "DllCanUnloadNow"); return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c)
{
    static HRESULT(WINAPI * fn)(REFCLSID, REFIID, LPVOID*) = NULL; if (!fn) fn = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))GetProcAddress(hOriginalWinHttp, "DllGetClassObject"); return fn ? fn(a, b, c) : E_NOTIMPL;
}

// ── DllMain ───────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        Log_Init();
        Log("DllMain: DLL_PROCESS_ATTACH");

        char systemPath[MAX_PATH];
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat_s(systemPath, "\\winhttp.dll");
        Log("DllMain: loading real winhttp from '%s'", systemPath);

        hOriginalWinHttp = LoadLibraryExA(systemPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hOriginalWinHttp)
        {
            Log("DllMain: FATAL - failed to load real winhttp.dll (GLE=%lu)", GetLastError());
            return FALSE;
        }
        Log("DllMain: real winhttp.dll at %p", (void*)hOriginalWinHttp);

        // Install the inline hook on crypt32!CryptQueryObject NOW, before
        // Activation64.dll's DllMain runs and Detours commits its inline patches.
        // A background thread would be too late — this must happen synchronously.
// Activation64.dll is already loaded by the time our DllMain fires.
        // Patch its IAT entry for CryptQueryObject NOW — sub_180001000 reads
        // this IAT slot and stores it in qword_180008710 during DllMain.
        // Do NOT use inline hooks on crypt32 exports — sub_1800018A0 detects
        // FF 25 patterns and causes __report_rangecheckfailure.
        {
            HMODULE hAct = GetModuleHandleA("Activation64.dll");
            if (hAct)
            {
                Real_CryptQueryObject = (PFN_CryptQueryObject)
                    GetProcAddress(LoadLibraryA("crypt32.dll"), "CryptQueryObject");
                if (PatchIATEntry(hAct, "crypt32.dll", "CryptQueryObject",
                    (ULONGLONG)Hooked_CryptQueryObject))
                    Log("DllMain: patched CryptQueryObject IAT in Activation64.dll (pre-DllMain)");
                else
                    Log("DllMain: WARNING - could not patch CryptQueryObject IAT in Activation64.dll");
            }
            else
            {
                Log("DllMain: Activation64.dll not yet loaded, CryptQueryObject will be patched by LoadThread");
            }
        }

        InstallEarlyConnectHook();
        InstallEarlySendHook();

        HANDLE hThread = CreateThread(nullptr, 0, LoadThread, nullptr, 0, nullptr);
        if (hThread)
        {
            Log("DllMain: LoadThread created (TID=%lu)", GetThreadId(hThread));
            CloseHandle(hThread);
        }
        else
        {
            Log("DllMain: WARNING - failed to create LoadThread (GLE=%lu)", GetLastError());
        }
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        Log("DllMain: DLL_PROCESS_DETACH");
        if (hOriginalWinHttp) FreeLibrary(hOriginalWinHttp);
        if (g_hLog != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
    }

    return TRUE;
}