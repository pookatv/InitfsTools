#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

static HMODULE hOriginalDInput8 = NULL;
static HMODULE hOriginalBCrypt = NULL;
static HMODULE hOriginalCrypt32 = NULL;

// ─── BCryptVerifySignature hook ───────────────────────────────────────────────

typedef NTSTATUS(WINAPI* PFN_BCryptVerifySignature)(
    BCRYPT_KEY_HANDLE hKey,
    VOID* pPaddingInfo,
    PUCHAR pbHash,
    ULONG cbHash,
    PUCHAR pbSignature,
    ULONG cbSignature,
    ULONG dwFlags);

static PFN_BCryptVerifySignature Real_BCryptVerifySignature = NULL;

NTSTATUS WINAPI Hooked_BCryptVerifySignature(
    BCRYPT_KEY_HANDLE hKey,
    VOID* pPaddingInfo,
    PUCHAR pbHash,
    ULONG cbHash,
    PUCHAR pbSignature,
    ULONG cbSignature,
    ULONG dwFlags)
{
    if (!Real_BCryptVerifySignature)
        return 0xC0000002;

    NTSTATUS result = Real_BCryptVerifySignature(hKey, pPaddingInfo, pbHash,
        cbHash, pbSignature, cbSignature, dwFlags);

    if (result == 0xC000A000)
        return 0;

    return result;
}

// ─── CryptQueryObject hook ────────────────────────────────────────────────────
// FCLiveEditor uses PLH x64Detour to hook CryptQueryObject via Crypt32.dll
// to bypass EA anticheat signature verification on game boot.
// We replicate this with an IAT patch on the game exe's Crypt32 import.

typedef BOOL(WINAPI* PFN_CryptQueryObject)(
    DWORD dwObjectType,
    const void* pvObject,
    DWORD dwExpectedContentTypeFlags,
    DWORD dwExpectedFormatTypeFlags,
    DWORD dwFlags,
    DWORD* pdwMsgAndCertEncodingType,
    DWORD* pdwContentType,
    DWORD* pdwFormatType,
    HCERTSTORE* phCertStore,
    HCRYPTMSG* phMsg,
    const void** ppvContext);

static PFN_CryptQueryObject Real_CryptQueryObject = NULL;

BOOL WINAPI Hooked_CryptQueryObject(
    DWORD dwObjectType,
    const void* pvObject,
    DWORD dwExpectedContentTypeFlags,
    DWORD dwExpectedFormatTypeFlags,
    DWORD dwFlags,
    DWORD* pdwMsgAndCertEncodingType,
    DWORD* pdwContentType,
    DWORD* pdwFormatType,
    HCERTSTORE* phCertStore,
    HCRYPTMSG* phMsg,
    const void** ppvContext)
{
    if (Real_CryptQueryObject)
    {
        BOOL result = Real_CryptQueryObject(
            dwObjectType, pvObject,
            dwExpectedContentTypeFlags, dwExpectedFormatTypeFlags,
            dwFlags,
            pdwMsgAndCertEncodingType, pdwContentType, pdwFormatType,
            phCertStore, phMsg, ppvContext);

        if (result)
            return result;
    }

    // Verification failed — spoof success so anticheat lets the game boot
    if (pdwContentType) *pdwContentType = CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED;
    if (pdwFormatType)  *pdwFormatType = CERT_QUERY_FORMAT_BINARY;
    if (pdwMsgAndCertEncodingType) *pdwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    return TRUE;
}

// ─── IAT patcher ─────────────────────────────────────────────────────────────

static void PatchIATEntry(HMODULE hExe, const char* targetDll,
    const char* funcName, ULONGLONG hookAddr)
{
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)hExe;
    PIMAGE_NT_HEADERS pNT = (PIMAGE_NT_HEADERS)((BYTE*)hExe + pDos->e_lfanew);
    DWORD importRVA = pNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRVA) return;

    PIMAGE_IMPORT_DESCRIPTOR pImport = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hExe + importRVA);
    for (; pImport->Name; pImport++)
    {
        const char* dllName = (const char*)((BYTE*)hExe + pImport->Name);
        if (_stricmp(dllName, targetDll) != 0) continue;

        PIMAGE_THUNK_DATA pOrigThunk = (PIMAGE_THUNK_DATA)((BYTE*)hExe + pImport->OriginalFirstThunk);
        PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)((BYTE*)hExe + pImport->FirstThunk);

        for (; pOrigThunk->u1.AddressOfData; pOrigThunk++, pThunk++)
        {
            if (pOrigThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;

            PIMAGE_IMPORT_BY_NAME pName = (PIMAGE_IMPORT_BY_NAME)
                ((BYTE*)hExe + pOrigThunk->u1.AddressOfData);

            if (strcmp((char*)pName->Name, funcName) == 0)
            {
                DWORD oldProtect;
                VirtualProtect(&pThunk->u1.Function, sizeof(ULONGLONG),
                    PAGE_EXECUTE_READWRITE, &oldProtect);
                pThunk->u1.Function = hookAddr;
                VirtualProtect(&pThunk->u1.Function, sizeof(ULONGLONG),
                    oldProtect, &oldProtect);
            }
        }
    }
}

static void PatchIAT()
{
    HMODULE hExe = GetModuleHandleA(NULL);
    if (!hExe) return;

    // BCryptVerifySignature — bypass signature check (existing)
    PatchIATEntry(hExe, "bcrypt.dll", "BCryptVerifySignature",
        (ULONGLONG)Hooked_BCryptVerifySignature);

    // CryptQueryObject — bypass certificate query used by EA anticheat
    // to verify game executable signature before allowing boot
    PatchIATEntry(hExe, "crypt32.dll", "CryptQueryObject",
        (ULONGLONG)Hooked_CryptQueryObject);
}

// ─── RET-patch helpers ────────────────────────────────────────────────────────

static void PatchFunction(const char* module, const char* funcName)
{
    HMODULE hMod = GetModuleHandleA(module);
    if (!hMod) hMod = LoadLibraryA(module);
    if (!hMod) return;

    FARPROC addr = GetProcAddress(hMod, funcName);
    if (!addr) return;

    DWORD oldProtect = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    *reinterpret_cast<BYTE*>(addr) = 0xC3;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), 8, oldProtect, &oldProtect);
}

static DWORD WINAPI PatchThread(LPVOID)
{
    PatchFunction("user32.dll", "MessageBoxTimeoutA");
    PatchFunction("kernelbase.dll", "TerminateProcess");
    return 1;
}

// ─── DInput8 export forwards ──────────────────────────────────────────────────

HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
    LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    typedef HRESULT(WINAPI* PFN)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalDInput8, "DirectInput8Create");
    return Real ? Real(hinst, dwVersion, riidltf, ppvOut, punkOuter) : E_NOTIMPL;
}

HRESULT WINAPI DllCanUnloadNow()
{
    typedef HRESULT(WINAPI* PFN)();
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalDInput8, "DllCanUnloadNow");
    return Real ? Real() : E_NOTIMPL;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    typedef HRESULT(WINAPI* PFN)(REFCLSID, REFIID, LPVOID*);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalDInput8, "DllGetClassObject");
    return Real ? Real(rclsid, riid, ppv) : E_NOTIMPL;
}

HRESULT WINAPI DllRegisterServer()
{
    typedef HRESULT(WINAPI* PFN)();
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalDInput8, "DllRegisterServer");
    return Real ? Real() : E_NOTIMPL;
}

HRESULT WINAPI DllUnregisterServer()
{
    typedef HRESULT(WINAPI* PFN)();
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalDInput8, "DllUnregisterServer");
    return Real ? Real() : E_NOTIMPL;
}

// ─── DllMain ─────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        char systemPath[MAX_PATH];
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat_s(systemPath, "\\dinput8.dll");
        hOriginalDInput8 = LoadLibraryExA(systemPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hOriginalDInput8)
            return FALSE;

        char bcryptPath[MAX_PATH];
        GetSystemDirectoryA(bcryptPath, MAX_PATH);
        strcat_s(bcryptPath, "\\bcrypt.dll");
        hOriginalBCrypt = LoadLibraryExA(bcryptPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hOriginalBCrypt)
        {
            Real_BCryptVerifySignature = (PFN_BCryptVerifySignature)
                GetProcAddress(hOriginalBCrypt, "BCryptVerifySignature");
        }

        // Load Crypt32 and grab the real CryptQueryObject before patching
        hOriginalCrypt32 = LoadLibraryA("crypt32.dll");
        if (hOriginalCrypt32)
        {
            Real_CryptQueryObject = (PFN_CryptQueryObject)
                GetProcAddress(hOriginalCrypt32, "CryptQueryObject");
        }

        PatchIAT();
        CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        if (hOriginalDInput8) FreeLibrary(hOriginalDInput8);
        if (hOriginalBCrypt)  FreeLibrary(hOriginalBCrypt);
        if (hOriginalCrypt32) FreeLibrary(hOriginalCrypt32);
    }

    return TRUE;
}